/*
 * Raw block-device cartridge backend.
 *
 * One physical block device holds one cartridge. This adapter implements the
 * struct cart_backend seam (include/cart_backend.h) against the on-disk raw
 * format (include/mhvtl_raw.h), mirroring the byte-stream semantics of the
 * file backend in usr/vtlcart.c:
 *
 *   - CART_STREAM_DATA  variable-block payload chunks (the old data.N file)
 *   - CART_STREAM_INDX  array of 512-byte raw_header records (indx.N)
 *   - CART_STREAM_META  meta_header followed by filemark block numbers (meta.N)
 *
 * Each (stream, partition) maps to a fixed device region described by the
 * partition table in the superblock. The device is opened once per cartridge
 * (raw_open) and the per-op callbacks translate (stream, partition, offset)
 * into an absolute device byte offset. truncate() records a logical length in
 * the partition table rather than resizing the device; sync() performs a
 * durable commit (payload/index flushed before the superblock, with a
 * generation_id bump).
 *
 * Copyright (C) 2024 mhvtl modernization. GPLv2 or later, as the rest of mhvtl.
 */

#define _FILE_OFFSET_BITS 64
#define _XOPEN_SOURCE 600	/* pread/pwrite, posix_fadvise */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>		/* memfd_create */
#include <linux/fs.h>		/* BLKGETSIZE64, BLKSSZGET, BLKPBSZGET */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "logging.h"
#include "mhvtl_scsi.h"
#include "vtlcart.h"		/* read_mam/write_mam, TAPE_FMT_VERSION */
#include "vtllib.h"		/* struct MAM, global mam, MAM_VERSION */
#include "mhvtl_raw.h"
#include "cart_backend.h"
#include "cart_backend_raw.h"
#include "ccan/crc32c/crc32c.h"
#include "be_byteshift.h"

/*
 * sizeof(struct meta_header) in usr/vtlcart.c: a uint32_t filemark_count
 * padded out to 512 bytes. The raw backend does not need the struct itself -
 * an initial meta_header (filemark_count == 0, zeroed padding) is simply 512
 * zero bytes - but it must agree on the size.
 */
#define RAW_META_HEADER_SIZE	512u

/* One raw_header index record is padded to 512 bytes (usr/vtlcart.c). */
#define RAW_INDX_RECORD_SIZE	512u

/* Floor for a partition's meta region (meta_header + filemark map). */
#define RAW_META_MIN_CAP	(16ull << 20)	/* 16 MiB */

/* ------------------------------------------------------------------ state */

/*
 * The cartridge is a single open device. The loaded superblock carries the
 * per-partition region table; op callbacks resolve offsets straight out of it.
 */
static int devfd = -1;
static int sb_loaded;
static unsigned dev_logical_sector = 512;
static unsigned dev_physical_sector = 512;
static struct mhvtl_raw_superblock sb;

/* ------------------------------------------------------- small utilities */

static uint64_t align_down_u64(uint64_t v, uint64_t a)
{
	return v - (v % a);
}

static uint64_t align_up_u64(uint64_t v, uint64_t a)
{
	return ((v + a - 1) / a) * a;
}

/* Pointers into the active superblock's partition table for one stream. */
static uint64_t region_off(enum cart_stream s, int partition)
{
	const struct mhvtl_raw_partition *p = &sb.part[partition];

	switch (s) {
	case CART_STREAM_DATA: return p->data_off;
	case CART_STREAM_INDX: return p->indx_off;
	case CART_STREAM_META: return p->meta_off;
	}
	return 0;
}

static uint64_t region_cap(enum cart_stream s, int partition)
{
	const struct mhvtl_raw_partition *p = &sb.part[partition];

	switch (s) {
	case CART_STREAM_DATA: return p->data_cap;
	case CART_STREAM_INDX: return p->indx_cap;
	case CART_STREAM_META: return p->meta_cap;
	}
	return 0;
}

static uint64_t region_used(enum cart_stream s, int partition)
{
	const struct mhvtl_raw_partition *p = &sb.part[partition];

	switch (s) {
	case CART_STREAM_DATA: return p->data_used;
	case CART_STREAM_INDX: return p->indx_used;
	case CART_STREAM_META: return p->meta_used;
	}
	return 0;
}

/* Record a stream's logical used length. Assigns directly to the packed
 * partition-table member (taking its address would warn on packed structs). */
static void region_set_used(enum cart_stream s, int partition, uint64_t len)
{
	struct mhvtl_raw_partition *p = &sb.part[partition];

	switch (s) {
	case CART_STREAM_DATA: p->data_used = len; break;
	case CART_STREAM_INDX: p->indx_used = len; break;
	case CART_STREAM_META: p->meta_used = len; break;
	}
}

/* ------------------------------------------------- superblock validation */

static int sb_is_valid(const struct mhvtl_raw_superblock *s)
{
	if (s->magic != MVTL_MAGIC)
		return 0;
	if (s->version != MVTL_VERSION)
		return 0;
	if (s->header_len != MVTL_SB_CRC_LEN)
		return 0;
	if (s->num_partitions == 0 || s->num_partitions > MVTL_MAX_PARTITIONS)
		return 0;
	if (crc32c(0, s, MVTL_SB_CRC_LEN) != s->header_crc32c)
		return 0;
	return 1;
}

static int sb_read_at(int fd, off_t off, struct mhvtl_raw_superblock *out)
{
	ssize_t n = pread(fd, out, sizeof(*out), off);

	if (n != (ssize_t)sizeof(*out))
		return 0;
	return sb_is_valid(out);
}

/*
 * Recompute the CRC over the active superblock and write both copies. Payload
 * and index writes are flushed first (fdatasync) so the superblock - which
 * commits the new generation - only reaches stable storage after the data it
 * describes. The mirror is written after the primary and both are flushed.
 */
static int sb_persist(int bump_generation)
{
	if (fdatasync(devfd) < 0) {
		MHVTL_ERR("fdatasync before superblock commit failed: %s",
			  strerror(errno));
		return -1;
	}

	if (bump_generation)
		sb.generation_id++;

	sb.header_crc32c = crc32c(0, &sb, MVTL_SB_CRC_LEN);

	/*
	 * Commit the mirror first, then the primary (design §6). Data+index are
	 * already durable (fdatasync above) and each 480-byte superblock is a
	 * single-sector atomic write, so a crash between the two always leaves at
	 * least one CRC-valid copy whose referenced data is durable; recovery
	 * picks the highest valid generation.
	 */
	if (pwrite(devfd, &sb, sizeof(sb), MVTL_SB_MIRROR_OFF) !=
	    (ssize_t)sizeof(sb)) {
		MHVTL_ERR("mirror superblock write failed: %s", strerror(errno));
		return -1;
	}
	if (fdatasync(devfd) < 0) {
		MHVTL_ERR("fdatasync after mirror superblock failed: %s",
			  strerror(errno));
		return -1;
	}
	if (pwrite(devfd, &sb, sizeof(sb), MVTL_SB_PRIMARY_OFF) !=
	    (ssize_t)sizeof(sb)) {
		MHVTL_ERR("primary superblock write failed: %s", strerror(errno));
		return -1;
	}
	if (fdatasync(devfd) < 0) {
		MHVTL_ERR("fdatasync after primary superblock failed: %s",
			  strerror(errno));
		return -1;
	}
	return 0;
}

/* --------------------------------------------------------- open / close */

/*
 * Both the drive and mhvtl-raw-format open the device with O_EXCL. On a block
 * device Linux then refuses the open with EBUSY while the device is mounted,
 * claimed by a holder (device-mapper/LVM, md, an imported ZFS vdev, bcache)
 * or opened O_EXCL by another process - which includes a drive that has the
 * cartridge loaded. See open(2), "O_EXCL". An active swap device is refused
 * for writing with ETXTBSY, with or without O_EXCL.
 */
static const char *excl_hint(int err)
{
	return (err == EBUSY || err == ETXTBSY) ?
		" (device in use: mounted, swap, held by LVM/md/ZFS, or "
		"loaded in a drive)" : "";
}

int raw_open(const char *devpath)
{
	int fd;
	uint64_t dev_bytes = 0;
	int sector = 0;
	struct mhvtl_raw_superblock primary, mirror;
	int vp, vm;

	fd = open(devpath, O_RDWR | O_LARGEFILE | O_EXCL);
	if (fd < 0) {
		MHVTL_ERR("open of raw device %s failed: %s%s",
			  devpath, strerror(errno), excl_hint(errno));
		return -1;
	}

	if (ioctl(fd, BLKGETSIZE64, &dev_bytes) < 0) {
		MHVTL_ERR("BLKGETSIZE64 on %s failed: %s",
			  devpath, strerror(errno));
		close(fd);
		return -1;
	}
	if (ioctl(fd, BLKSSZGET, &sector) < 0 || sector <= 0) {
		MHVTL_ERR("BLKSSZGET on %s failed: %s",
			  devpath, strerror(errno));
		close(fd);
		return -1;
	}

	vp = sb_read_at(fd, MVTL_SB_PRIMARY_OFF, &primary);
	vm = sb_read_at(fd, MVTL_SB_MIRROR_OFF, &mirror);
	if (!vp && !vm) {
		MHVTL_ERR("no valid MVTL superblock on %s", devpath);
		close(fd);
		errno = EINVAL;
		return -1;
	}

	/* Keep the valid copy with the highest generation_id. */
	if (vp && (!vm || primary.generation_id >= mirror.generation_id))
		memcpy(&sb, &primary, sizeof(sb));
	else
		memcpy(&sb, &mirror, sizeof(sb));

	if (dev_bytes < sb.total_capacity_bytes)
		MHVTL_LOG("raw device %s is smaller (%llu) than the superblock "
			  "records (%llu); using recorded layout",
			  devpath, (unsigned long long)dev_bytes,
			  (unsigned long long)sb.total_capacity_bytes);

	devfd = fd;
	dev_logical_sector = (unsigned)sector;
	/* Physical sector is informational here (region alignment was fixed at
	 * format time); query it best-effort and fall back to the logical size. */
	{
		int pbsz = 0;

		dev_physical_sector =
			(ioctl(fd, BLKPBSZGET, &pbsz) == 0 && pbsz > 0)
				? (unsigned)pbsz : (unsigned)sector;
	}
	sb_loaded = 1;

	MHVTL_DBG(1, "Opened raw cartridge %s: barcode '%s', %u partition(s), "
		  "generation %llu (sector logical/physical %u/%u)",
		  devpath, sb.barcode, sb.num_partitions,
		  (unsigned long long)sb.generation_id,
		  dev_logical_sector, dev_physical_sector);
	return 0;
}

void raw_close(void)
{
	if (devfd >= 0) {
		fdatasync(devfd);
		close(devfd);
	}
	devfd = -1;
	sb_loaded = 0;
}

/*
 * Probe a block device for an MVTL cartridge WITHOUT loading it as the active
 * cartridge (leaves the devfd/sb backend state untouched — safe to call on a
 * device while another cartridge is open). Reads + validates both superblocks,
 * takes the highest valid generation, and reports the cartridge identity. Used
 * by the bay provisioner / hot-plug monitor to learn a disk's barcode before
 * deciding whether/where to place it. Returns 0 if a valid MVTL cartridge is
 * present, -1 otherwise (errno set).
 */
int raw_probe(const char *devpath, struct mhvtl_raw_probe *out)
{
	int fd;
	struct mhvtl_raw_superblock primary, mirror;
	const struct mhvtl_raw_superblock *pick;
	int vp, vm;

	if (!devpath || !out) {
		errno = EINVAL;
		return -1;
	}

	fd = open(devpath, O_RDONLY | O_LARGEFILE);
	if (fd < 0)
		return -1;

	vp = sb_read_at(fd, MVTL_SB_PRIMARY_OFF, &primary);
	vm = sb_read_at(fd, MVTL_SB_MIRROR_OFF, &mirror);
	close(fd);

	if (!vp && !vm) {
		errno = EINVAL;
		return -1;
	}
	if (vp && (!vm || primary.generation_id >= mirror.generation_id))
		pick = &primary;
	else
		pick = &mirror;

	memset(out, 0, sizeof(*out));
	memcpy(out->barcode, pick->barcode, sizeof(pick->barcode));
	out->barcode[sizeof(pick->barcode)] = '\0';
	memcpy(out->cartridge_uuid, pick->cartridge_uuid,
	       sizeof(out->cartridge_uuid));
	out->num_partitions = pick->num_partitions;
	out->generation_id = pick->generation_id;
	out->total_capacity_bytes = pick->total_capacity_bytes;
	return 0;
}

/* ------------------------------------------------------ cart_backend ops */

static int raw_be_open_partition(uint8_t partition)
{
	if (!sb_loaded || devfd < 0) {
		MHVTL_ERR("raw backend: no device open");
		return 3;
	}
	if (partition >= sb.num_partitions) {
		MHVTL_ERR("raw backend: partition %u out of range (have %u)",
			  partition, sb.num_partitions);
		return 3;
	}
	/* Confirm the region bases look initialised (a formatted partition has
	 * non-zero region offsets, all >= MVTL_DATA_START). */
	if (sb.part[partition].data_off < MVTL_DATA_START ||
	    sb.part[partition].indx_off < MVTL_DATA_START ||
	    sb.part[partition].meta_off < MVTL_DATA_START) {
		MHVTL_ERR("raw backend: partition %u region table uninitialised",
			  partition);
		return 3;
	}
	return 0;
}

static int raw_be_create_partition(int partition)
{
	char zero[RAW_META_HEADER_SIZE];

	if (!sb_loaded || devfd < 0) {
		MHVTL_ERR("raw backend: no device open");
		return 1;
	}
	if (partition < 0 || partition >= (int)sb.num_partitions) {
		MHVTL_ERR("raw backend: create partition %d out of range",
			  partition);
		return 1;
	}

	/* An initial meta_header is filemark_count == 0 with zeroed padding. */
	memset(zero, 0, sizeof(zero));
	if (pwrite(devfd, zero, sizeof(zero),
		   (off_t)sb.part[partition].meta_off) != (ssize_t)sizeof(zero)) {
		MHVTL_ERR("raw backend: init meta_header for partition %d: %s",
			  partition, strerror(errno));
		return 1;
	}

	sb.part[partition].meta_used = RAW_META_HEADER_SIZE;
	if (sb_persist(0) < 0)
		return 1;

	return 0;
}

static void raw_be_close_partition(uint8_t partition)
{
	/* The device stays open across partitions; releasing the fd is the
	 * whole-cartridge raw_close(). Nothing to do per partition. */
	(void)partition;
}

static ssize_t raw_be_pread(enum cart_stream s, int partition,
			    void *buf, size_t len, off_t off)
{
	if (!sb_loaded || devfd < 0) {
		errno = EBADF;
		return -1;
	}
	return pread(devfd, buf, len, (off_t)region_off(s, partition) + off);
}

static ssize_t raw_be_pwrite(enum cart_stream s, int partition,
			     const void *buf, size_t len, off_t off)
{
	uint64_t cap = region_cap(s, partition);

	if (!sb_loaded || devfd < 0) {
		errno = EBADF;
		return -1;
	}
	/* A write past the stream's capacity is end-of-medium, not a device
	 * resize: refuse it with ENOSPC. */
	if (off < 0 || (uint64_t)off + len > cap) {
		errno = ENOSPC;
		return -1;
	}
	ssize_t n = pwrite(devfd, buf, len, (off_t)region_off(s, partition) + off);
	/*
	 * A file stream grows implicitly when written past its end; the raw
	 * backend records the same growth in the partition table so EOD/index
	 * length survive a reload. truncate() may still shrink it explicitly.
	 */
	if (n > 0) {
		uint64_t end = (uint64_t)off + (uint64_t)n;
		if (end > region_used(s, partition))
			region_set_used(s, partition, end);
	}
	return n;
}

static int raw_be_truncate(enum cart_stream s, int partition, off_t len)
{
	if (!sb_loaded || devfd < 0) {
		errno = EBADF;
		return -1;
	}
	if (len < 0 || (uint64_t)len > region_cap(s, partition)) {
		errno = ENOSPC;
		return -1;
	}
	/* Truncation is a logical-length change, not a device resize: record
	 * the new used length in the partition table and persist it. */
	region_set_used(s, partition, (uint64_t)len);
	return sb_persist(0);
}

static int raw_be_sync(enum cart_stream s, int partition)
{
	(void)s;
	(void)partition;

	if (!sb_loaded || devfd < 0) {
		errno = EBADF;
		return -1;
	}
	/* Single device: a durable commit flushes payload+index, then writes a
	 * freshly-generationed superblock. sb_persist() does the ordering, and
	 * its fdatasync() also flushes the disk's write cache: on a block device
	 * it sends the device a cache flush. */
	return sb_persist(1);
}

static int raw_be_fadvise_dontneed(enum cart_stream s, int partition)
{
	if (!sb_loaded || devfd < 0) {
		errno = EBADF;
		return -1;
	}
	return posix_fadvise(devfd, (off_t)region_off(s, partition),
			     (off_t)region_cap(s, partition),
			     POSIX_FADV_DONTNEED);
}

/* --------------------------------------------- cartridge-level ops + MAM */

/* pread the whole region [off, off+len) into buf (loops over short reads). */
static int read_exact_at(int fd, void *buf, size_t len, off_t off)
{
	uint8_t *p = buf;

	while (len) {
		ssize_t n = pread(fd, p, len, off);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (n == 0) {
			errno = EIO;
			return -1;
		}
		p += n;
		off += n;
		len -= (size_t)n;
	}
	return 0;
}

/* write the whole buffer to fd at its current offset (loops over short writes). */
static int write_exact(int fd, const void *buf, size_t len)
{
	const uint8_t *p = buf;

	while (len) {
		ssize_t n = write(fd, p, len);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

/*
 * Open (create==0) or validate (create!=0) the raw cartridge. The argument is
 * the resolved cartridge path (currentPCL) — for the raw backend a block-device
 * node, not the bare barcode. A raw cartridge is formatted out of band by
 * mhvtl-raw-format, so a create request that finds a valid superblock reports
 * CART_OPEN_EXISTS (nothing to initialise) and leaves the device closed; an
 * unformatted device cannot be "created" here.
 */
static int raw_be_open_cartridge(const char *path, int create)
{
	if (raw_open(path) != 0)
		return create ? 2 : 3;

	if (create) {
		raw_close();
		return CART_OPEN_EXISTS;
	}
	return 0;
}

static void raw_be_close_cartridge(void)
{
	/* UNLOAD commits the superblock (design §6): flush payload/index and
	 * write the partition table so stream growth from writes is durable,
	 * the disk's write cache included, then release it. */
	if (sb_loaded && devfd >= 0)
		sb_persist(0);
	raw_close();
}

static int raw_be_num_partitions(void)
{
	if (!sb_loaded)
		return 0;
	return (int)sb.num_partitions;
}

static off_t raw_be_used_size(enum cart_stream s, int partition)
{
	if (!sb_loaded || partition < 0 || partition >= (int)sb.num_partitions) {
		errno = EBADF;
		return -1;
	}
	switch (s) {
	case CART_STREAM_DATA: return (off_t)sb.part[partition].data_used;
	case CART_STREAM_INDX: return (off_t)sb.part[partition].indx_used;
	case CART_STREAM_META: return (off_t)sb.part[partition].meta_used;
	}
	errno = EINVAL;
	return -1;
}

static int raw_be_partition_loaded(int partition)
{
	/* The whole cartridge opens at once (one device fd), so every valid
	 * partition is accessible while the superblock is loaded. */
	return sb_loaded && devfd >= 0 &&
	       partition >= 0 && partition < (int)sb.num_partitions;
}

/*
 * The MAM region holds two length-delimited sub-streams, matching the file
 * backend's two files (mam, mhvtl_data):
 *   [u32 mam_len][u32 mhvtl_len][mam bytes][mhvtl bytes]
 * The same read_mam()/write_mam() serializers the file backend uses are driven
 * over in-memory fds so the on-disk encoding is byte-compatible. sb.mam_len is
 * the total container length; 0 means "no MAM written yet" (fresh format).
 */
static int raw_serialize_mam(uint8_t **out, uint64_t *out_len)
{
	int mfd = -1, hfd = -1, ret = -1;
	off_t mlen, hlen;
	uint32_t hdr[2];
	uint8_t *buf = NULL;
	uint64_t total;

	mfd = memfd_create("mhvtl-mam", 0);
	hfd = memfd_create("mhvtl-mhvtl", 0);
	if (mfd < 0 || hfd < 0) {
		MHVTL_ERR("raw save_mam: memfd_create failed: %s", strerror(errno));
		goto out;
	}

	if (write_mam(mfd, hfd) < 0) {
		MHVTL_ERR("raw save_mam: write_mam failed");
		goto out;
	}

	mlen = lseek(mfd, 0, SEEK_END);
	hlen = lseek(hfd, 0, SEEK_END);
	if (mlen < 0 || hlen < 0) {
		MHVTL_ERR("raw save_mam: lseek failed: %s", strerror(errno));
		goto out;
	}

	total = sizeof(hdr) + (uint64_t)mlen + (uint64_t)hlen;
	if (total > MVTL_MAM_RESERVE) {
		MHVTL_ERR("raw save_mam: serialized MAM %llu exceeds region %llu",
			  (unsigned long long)total,
			  (unsigned long long)MVTL_MAM_RESERVE);
		errno = ENOSPC;
		goto out;
	}

	buf = malloc((size_t)total);
	if (!buf) {
		errno = ENOMEM;
		goto out;
	}
	hdr[0] = (uint32_t)mlen;
	hdr[1] = (uint32_t)hlen;
	memcpy(buf, hdr, sizeof(hdr));
	if (read_exact_at(mfd, buf + sizeof(hdr), (size_t)mlen, 0) < 0 ||
	    read_exact_at(hfd, buf + sizeof(hdr) + mlen, (size_t)hlen, 0) < 0) {
		MHVTL_ERR("raw save_mam: reading back serialized MAM failed: %s",
			  strerror(errno));
		goto out;
	}

	*out = buf;
	*out_len = total;
	buf = NULL;
	ret = 0;
out:
	free(buf);
	if (mfd >= 0)
		close(mfd);
	if (hfd >= 0)
		close(hfd);
	return ret;
}

static int raw_be_save_mam(struct MAM *mamp)
{
	uint8_t *buf = NULL;
	uint64_t total;
	int ret = -1;

	(void)mamp;	/* write_mam serializes the global mam */

	if (!sb_loaded || devfd < 0) {
		errno = EBADF;
		return -1;
	}

	if (raw_serialize_mam(&buf, &total) < 0)
		return -1;

	if (pwrite(devfd, buf, (size_t)total, (off_t)sb.mam_off) !=
	    (ssize_t)total) {
		MHVTL_ERR("raw save_mam: MAM region write failed: %s",
			  strerror(errno));
		goto out;
	}

	sb.mam_len = total;
	if (sb_persist(0) < 0)
		goto out;

	ret = 0;
out:
	free(buf);
	return ret;
}

static int raw_be_load_mam(struct MAM *mamp, uint8_t *sam_stat)
{
	int mfd = -1, hfd = -1, ret = -1;
	uint32_t hdr[2];
	uint8_t *buf = NULL;
	uint64_t mlen, hlen;

	(void)sam_stat;

	if (!sb_loaded || devfd < 0) {
		errno = EBADF;
		return -1;
	}

	if (sb.mam_len == 0) {
		/* Freshly formatted cartridge: no MAM yet. Present a current,
		 * empty MAM so load proceeds without legacy-format migration. */
		mamp->mam_fmt_version = MAM_VERSION;
		mamp->tape_fmt_version = TAPE_FMT_VERSION;
		return 0;
	}
	if (sb.mam_len < sizeof(hdr) || sb.mam_len > MVTL_MAM_RESERVE) {
		MHVTL_ERR("raw load_mam: implausible mam_len %llu",
			  (unsigned long long)sb.mam_len);
		errno = EINVAL;
		return -1;
	}

	buf = malloc((size_t)sb.mam_len);
	if (!buf) {
		errno = ENOMEM;
		return -1;
	}
	if (read_exact_at(devfd, buf, (size_t)sb.mam_len,
			  (off_t)sb.mam_off) < 0) {
		MHVTL_ERR("raw load_mam: MAM region read failed: %s",
			  strerror(errno));
		goto out;
	}
	memcpy(hdr, buf, sizeof(hdr));
	mlen = hdr[0];
	hlen = hdr[1];
	if (sizeof(hdr) + mlen + hlen > sb.mam_len) {
		MHVTL_ERR("raw load_mam: MAM sub-stream lengths overflow region");
		errno = EINVAL;
		goto out;
	}

	mfd = memfd_create("mhvtl-mam", 0);
	hfd = memfd_create("mhvtl-mhvtl", 0);
	if (mfd < 0 || hfd < 0) {
		MHVTL_ERR("raw load_mam: memfd_create failed: %s", strerror(errno));
		goto out;
	}
	if (write_exact(mfd, buf + sizeof(hdr), (size_t)mlen) < 0 ||
	    write_exact(hfd, buf + sizeof(hdr) + mlen, (size_t)hlen) < 0) {
		MHVTL_ERR("raw load_mam: staging MAM sub-streams failed: %s",
			  strerror(errno));
		goto out;
	}
	lseek(mfd, 0, SEEK_SET);
	lseek(hfd, 0, SEEK_SET);

	if (read_mam(mfd, hfd, mamp) < 0) {
		MHVTL_ERR("raw load_mam: read_mam failed");
		goto out;
	}
	ret = 0;
out:
	free(buf);
	if (mfd >= 0)
		close(mfd);
	if (hfd >= 0)
		close(hfd);
	return ret;
}

struct cart_backend cart_backend_raw = {
	.name			= "raw",
	.open_partition		= raw_be_open_partition,
	.create_partition	= raw_be_create_partition,
	.close_partition	= raw_be_close_partition,
	.pread			= raw_be_pread,
	.pwrite			= raw_be_pwrite,
	.truncate		= raw_be_truncate,
	.sync			= raw_be_sync,
	.fadvise_dontneed	= raw_be_fadvise_dontneed,
	.open_cartridge		= raw_be_open_cartridge,
	.close_cartridge	= raw_be_close_cartridge,
	.load_mam		= raw_be_load_mam,
	.save_mam		= raw_be_save_mam,
	.num_partitions		= raw_be_num_partitions,
	.used_size		= raw_be_used_size,
	.partition_loaded	= raw_be_partition_loaded,
};

/* ------------------------------------------------------------- format */

/* Parse "92,8" / "85,5,5,5" into pct[]; require 1..MVTL_MAX_PARTITIONS
 * entries each > 0 that sum to exactly 100. Returns count, or -1 (EINVAL). */
static int parse_partspec(const char *spec, unsigned pct[MVTL_MAX_PARTITIONS])
{
	const char *p = spec;
	int n = 0;
	unsigned total = 0;

	if (!spec || !*spec) {
		errno = EINVAL;
		return -1;
	}

	while (*p) {
		char *end;
		long v = strtol(p, &end, 10);

		if (end == p || v <= 0 || v > 100) {
			errno = EINVAL;
			return -1;
		}
		if (n >= MVTL_MAX_PARTITIONS) {
			errno = EINVAL;
			return -1;
		}
		pct[n++] = (unsigned)v;
		total += (unsigned)v;

		p = end;
		if (*p == ',')
			p++;
		else if (*p != '\0') {
			errno = EINVAL;
			return -1;
		}
	}

	if (n == 0 || total != 100) {
		errno = EINVAL;
		return -1;
	}
	return n;
}

static void fill_cartridge_uuid(uint8_t uuid[16])
{
	int fd = open("/dev/urandom", O_RDONLY);
	ssize_t n = -1;

	if (fd >= 0) {
		n = read(fd, uuid, 16);
		close(fd);
	}
	if (n != 16) {
		/* Fall back to a weak seed rather than fail the format. */
		for (int i = 0; i < 16; i++)
			uuid[i] = (uint8_t)(rand() & 0xff);
	}
	/* Stamp RFC 4122 v4 version/variant bits. */
	uuid[6] = (uuid[6] & 0x0f) | 0x40;
	uuid[8] = (uuid[8] & 0x3f) | 0x80;
}

/* A two-character LTO media-type suffix the raw backend accepts: data L1-L9
 * and LA (LTO-10), M8 (LTO-7 media written in the LTO-8 Type M format), and
 * WORM LT-LZ (U3-U9) and LH (U10). Labels per IBM SG24-5946 and the IBM bar
 * code label table,
 * https://www.ibm.com/docs/en/STAKKZ/con_LTOdrive_cartridges_barcodelabel.html. */
static int valid_lto_media_suffix(const char *s)
{
	if (s[0] == 'L' && ((s[1] >= '1' && s[1] <= '9') || s[1] == 'A'))
		return 1;				/* data L1-L9, LA */
	if (s[0] == 'M' && s[1] == '8')
		return 1;				/* M8 (LTO-7 media as LTO-8 Type M) */
	if (s[0] == 'L' && ((s[1] >= 'T' && s[1] <= 'Z') || s[1] == 'H'))
		return 1;				/* WORM LT-LZ, LH (U3-U10) */
	return 0;
}

/* A raw cartridge barcode is a 6-character VOLSER, optionally followed by a
 * two-character media-type suffix. Reject a malformed label (e.g. lowercase,
 * wrong length, or a bogus/mismatched suffix) before it is baked into the
 * superblock. Returns 0 if acceptable, -1 (errno EINVAL) otherwise. */
static int validate_raw_barcode(const char *barcode)
{
	size_t len, i;

	if (!barcode || !barcode[0])
		return 0;	/* no barcode supplied: format without one */

	len = strlen(barcode);
	if (len != 6 && len != 8) {
		MHVTL_ERR("raw_format: barcode '%s' must be 6 chars (VOLSER) or "
			  "8 chars (VOLSER + media suffix, e.g. VOL001L8)", barcode);
		errno = EINVAL;
		return -1;
	}
	for (i = 0; i < 6; i++) {
		unsigned char c = (unsigned char)barcode[i];
		if (!isupper(c) && !isdigit(c)) {
			MHVTL_ERR("raw_format: barcode '%s' VOLSER must be 6 "
				  "upper-case alphanumeric characters", barcode);
			errno = EINVAL;
			return -1;
		}
	}
	if (len == 8 && !valid_lto_media_suffix(barcode + 6)) {
		MHVTL_ERR("raw_format: barcode '%s' has an invalid media suffix "
			  "'%s' (expect data L1-L9/LA, M8, or WORM LT-LZ/LH)",
			  barcode, barcode + 6);
		errno = EINVAL;
		return -1;
	}
	return 0;
}

/* The density a barcode's media suffix stands for, written to *density, and
 * whether the suffix names WORM media. The data suffixes L1-L9 and LA are
 * LTO-1 to LTO-10, and the WORM suffixes LT-LZ and LH are LTO-3 to LTO-10
 * (IBM SG24-5946). M8 is the LTO-8 Type M data cartridge, density 5Dh (IBM
 * LTO SCSI Reference GA32-0928-07 table 113, p.142). Returns -1 for a
 * barcode without a suffix.
 */
static int raw_barcode_density(const char *barcode, char *density, size_t len,
			       int *worm)
{
	const char *sfx;

	*worm = 0;
	if (!barcode || strlen(barcode) != 8)
		return -1;
	sfx = barcode + 6;

	if (sfx[0] == 'L' && sfx[1] >= '1' && sfx[1] <= '9')
		snprintf(density, len, "LTO%c", sfx[1]);
	else if (sfx[0] == 'L' && sfx[1] == 'A')
		snprintf(density, len, "LTO10");
	else if (sfx[0] == 'M' && sfx[1] == '8')
		snprintf(density, len, "LTO8M8");
	else if (sfx[0] == 'L' && sfx[1] >= 'T' && sfx[1] <= 'Z') {
		snprintf(density, len, "LTO%d", sfx[1] - 'T' + 3);
		*worm = 1;
	} else if (sfx[0] == 'L' && sfx[1] == 'H') {
		snprintf(density, len, "LTO10");
		*worm = 1;
	} else
		return -1;
	return 0;
}

/*
 * Start the global mam the way mktape does for a new cartridge: the media
 * type and density from the barcode suffix, or from density_arg for a
 * barcode without one. Refuse a density_arg that contradicts the suffix and
 * more partitions than the media supports. Called before the device is
 * touched, so a refusal leaves the disk as it was.
 */
static int raw_mam_media(const char *barcode, const char *density_arg,
			 int nparts)
{
	char density[16];
	int worm;

	if (raw_barcode_density(barcode, density, sizeof(density), &worm) < 0) {
		if (!density_arg) {
			MHVTL_ERR("raw_format: barcode '%s' has no media suffix; "
				  "give the density (e.g. LTO8)",
				  barcode ? barcode : "");
			errno = EINVAL;
			return -1;
		}
		snprintf(density, sizeof(density), "%s", density_arg);
	} else if (density_arg && strcasecmp(density_arg, density)) {
		MHVTL_ERR("raw_format: density %s contradicts barcode '%s' (%s)",
			  density_arg, barcode, density);
		errno = EINVAL;
		return -1;
	}

	init_mam(&mam);
	mam.tape_fmt_version = TAPE_FMT_VERSION;
	mam.mam_fmt_version  = MAM_VERSION;
	mam.MediumType	     = worm ? MEDIA_TYPE_WORM : MEDIA_TYPE_DATA;

	if (set_media_params(&mam, density)) {
		MHVTL_ERR("raw_format: '%s' is not a known density", density);
		errno = EINVAL;
		return -1;
	}
	if (nparts > mam.max_partitions) {
		MHVTL_ERR("raw_format: %s media holds at most %d partition(s), "
			  "not %d", density, mam.max_partitions, nparts);
		errno = EINVAL;
		return -1;
	}
	return 0;
}

/*
 * Finish the mam started by raw_mam_media(), with the capacity taken from the
 * data regions of the device rather than the media type: a raw cartridge
 * holds what its disk holds. Each partition's capacity is recorded as well,
 * since the partition layout is fixed by the format.
 */
static void raw_init_mam(const struct mhvtl_raw_superblock *s,
			 const char *barcode)
{
	char ver[64];
	uint64_t total = 0;
	int major = 0, minor = 0;
	time_t now = time(NULL);

	mam.num_partitions = (uint8_t)s->num_partitions;
	for (uint32_t i = 0; i < s->num_partitions; i++) {
		put_unaligned_be64(s->part[i].advertised_capacity,
				   &mam.partition_capacity[i]);
		total += s->part[i].advertised_capacity;
	}
	put_unaligned_be64(total, &mam.max_capacity);
	put_unaligned_be64(total, &mam.remaining_capacity);

	sscanf(MHVTL_VERSION, "%d.%d", &major, &minor);
	snprintf(ver, sizeof(ver), "vtl-%d.%d      ", major, minor);
	memcpy(&mam.MediumManufacturer, "linuxVTL", 8);
	memcpy(&mam.ApplicationVendor, ver, 8);
	snprintf((char *)mam.ApplicationVersion, sizeof(mam.ApplicationVersion),
		 "%d", TAPE_FMT_VERSION);
	snprintf((char *)mam.MediumSerialNumber, sizeof(mam.MediumSerialNumber),
		 "%s_%d", barcode, (int)now);
	snprintf((char *)mam.MediumManufactureDate,
		 sizeof(mam.MediumManufactureDate), "%d", (int)now);
	snprintf((char *)mam.Barcode, sizeof(mam.Barcode), "%-31s", barcode);

	mam_space_remaining(&mam);
}

int raw_format(const char *devpath, const char *barcode, const char *density,
	       const char *partspec, uint64_t min_record_size)
{
	int fd, nparts, rc = -1;
	uint64_t dev_bytes = 0, usable, alignment, part_base, consumed = 0;
	uint64_t mam_len = 0;
	int sector = 0, psector = 0;
	unsigned pct[MVTL_MAX_PARTITIONS];
	void *zbuf = NULL;
	uint8_t *mam_buf = NULL;
	struct mhvtl_raw_superblock newsb;

	if (min_record_size == 0) {
		MHVTL_ERR("raw_format: min_record_size must be > 0");
		errno = EINVAL;
		return -1;
	}

	if (validate_raw_barcode(barcode) < 0)
		return -1;

	nparts = parse_partspec(partspec, pct);
	if (nparts < 0) {
		MHVTL_ERR("raw_format: invalid partspec '%s' (need per-partition "
			  "percentages totalling 100)", partspec ? partspec : "");
		return -1;
	}

	if (raw_mam_media(barcode, density, nparts) < 0)
		return -1;

	fd = open(devpath, O_RDWR | O_LARGEFILE | O_EXCL);
	if (fd < 0) {
		MHVTL_ERR("raw_format: open %s failed: %s%s",
			  devpath, strerror(errno), excl_hint(errno));
		return -1;
	}

	if (ioctl(fd, BLKGETSIZE64, &dev_bytes) < 0) {
		MHVTL_ERR("raw_format: BLKGETSIZE64 on %s failed: %s",
			  devpath, strerror(errno));
		goto out;
	}
	if (ioctl(fd, BLKSSZGET, &sector) < 0 || sector <= 0) {
		MHVTL_ERR("raw_format: BLKSSZGET on %s failed: %s",
			  devpath, strerror(errno));
		goto out;
	}
	/* Physical sector can exceed the logical one (e.g. 4Kn / 512e drives);
	 * query it best-effort and fall back to the logical size if unsupported. */
	if (ioctl(fd, BLKPBSZGET, &psector) < 0 || psector <= 0)
		psector = sector;

	/* Align every region to max(physical_sector, logical_sector, 4096) so no
	 * region boundary can straddle a physical sector and force the drive into
	 * a read-modify-write. */
	alignment = 4096ull;
	if ((uint64_t)sector > alignment)
		alignment = (uint64_t)sector;
	if ((uint64_t)psector > alignment)
		alignment = (uint64_t)psector;

	if (dev_bytes <= MVTL_DATA_START + alignment) {
		MHVTL_ERR("raw_format: %s too small (%llu bytes)",
			  devpath, (unsigned long long)dev_bytes);
		errno = ENOSPC;
		goto out;
	}

	usable = align_down_u64(dev_bytes - MVTL_DATA_START, alignment);

	/* Zero the reserved/protective area so the host does not auto-mount a
	 * stale partition table that used to live on this device. */
	zbuf = calloc(1, MVTL_RESERVED_BYTES);
	if (!zbuf) {
		MHVTL_ERR("raw_format: out of memory");
		errno = ENOMEM;
		goto out;
	}
	if (pwrite(fd, zbuf, MVTL_RESERVED_BYTES, 0) !=
	    (ssize_t)MVTL_RESERVED_BYTES) {
		MHVTL_ERR("raw_format: zeroing reserved area failed: %s",
			  strerror(errno));
		goto out;
	}

	/* Invalidate any earlier cartridge's superblocks before its regions are
	 * overwritten, so an interrupted reformat leaves no valid superblock
	 * describing a half-written layout. */
	if (pwrite(fd, zbuf, sizeof(newsb), MVTL_SB_MIRROR_OFF) !=
		    (ssize_t)sizeof(newsb) ||
	    pwrite(fd, zbuf, sizeof(newsb), MVTL_SB_PRIMARY_OFF) !=
		    (ssize_t)sizeof(newsb) ||
	    fdatasync(fd) < 0) {
		MHVTL_ERR("raw_format: invalidating old superblocks failed: %s",
			  strerror(errno));
		goto out;
	}

	memset(&newsb, 0, sizeof(newsb));
	newsb.magic			= MVTL_MAGIC;
	newsb.header_len		= MVTL_SB_CRC_LEN;
	newsb.version			= MVTL_VERSION;
	newsb.logical_sector		= (uint16_t)sector;
	newsb.total_capacity_bytes	= dev_bytes;
	newsb.generation_id		= 1;
	newsb.num_partitions		= (uint32_t)nparts;
	newsb.mam_off			= MVTL_MAM_OFF;
	newsb.mam_len			= 0;
	fill_cartridge_uuid(newsb.cartridge_uuid);
	if (barcode)
		strncpy(newsb.barcode, barcode, sizeof(newsb.barcode) - 1);
	/* vendor_id/product_id are populated from the drive/MAM config in a
	 * later increment; left zeroed here. */

	part_base = MVTL_DATA_START;
	for (int i = 0; i < nparts; i++) {
		struct mhvtl_raw_partition *pt = &newsb.part[i];
		uint64_t P, avail, data_cap, indx_cap, meta_cap, max_records;

		/* Static layout: each partition gets its percentage of the
		 * usable span; the last partition absorbs the alignment slack. */
		if (i == nparts - 1)
			P = align_down_u64(usable - consumed, alignment);
		else
			P = align_down_u64(usable * pct[i] / 100, alignment);
		consumed += P;

		meta_cap = align_up_u64(RAW_META_MIN_CAP, alignment);
		if (P <= meta_cap + 2 * alignment) {
			MHVTL_ERR("raw_format: partition %d too small for layout "
				  "(%llu bytes)", i, (unsigned long long)P);
			errno = ENOSPC;
			goto out;
		}

		/*
		 * Split the remaining space between data payload and the index.
		 * Each minimum-sized record costs min_record_size data bytes and
		 * one 512-byte index record, so:
		 *   data_cap = avail * min / (min + 512)
		 *   max_records = ceil(data_cap / min)   (worst case: all minimum)
		 *   indx_cap    = max_records * 512
		 * data_cap is held at this value (not stretched into the leftover)
		 * so the index always covers a data region full of minimum records.
		 */
		avail = P - meta_cap;
		data_cap = align_down_u64(
			avail * min_record_size / (min_record_size + RAW_INDX_RECORD_SIZE),
			alignment);
		if (data_cap < alignment)
			data_cap = alignment;
		max_records = (data_cap + min_record_size - 1) / min_record_size;
		indx_cap = align_up_u64(max_records * RAW_INDX_RECORD_SIZE, alignment);

		/* Alignment rounding can nudge the total over P; trim data. */
		while (meta_cap + indx_cap + data_cap > P && data_cap > alignment) {
			data_cap -= alignment;
			max_records = (data_cap + min_record_size - 1) / min_record_size;
			indx_cap = align_up_u64(max_records * RAW_INDX_RECORD_SIZE,
						alignment);
		}
		if (meta_cap + indx_cap + data_cap > P) {
			MHVTL_ERR("raw_format: partition %d layout does not fit", i);
			errno = ENOSPC;
			goto out;
		}

		/* Place meta, then index, then data within the partition span. */
		pt->meta_off	= part_base;
		pt->meta_cap	= meta_cap;
		pt->meta_used	= RAW_META_HEADER_SIZE;
		pt->indx_off	= pt->meta_off + meta_cap;
		pt->indx_cap	= indx_cap;
		pt->indx_used	= 0;
		pt->data_off	= pt->indx_off + indx_cap;
		pt->data_cap	= data_cap;
		pt->data_used	= 0;
		pt->max_records	= max_records;
		pt->advertised_capacity = data_cap;

		part_base += P;

		MHVTL_DBG(1, "raw_format p%d: meta@%llu+%llu indx@%llu+%llu "
			  "data@%llu+%llu (max_records %llu)", i,
			  (unsigned long long)pt->meta_off,
			  (unsigned long long)pt->meta_cap,
			  (unsigned long long)pt->indx_off,
			  (unsigned long long)pt->indx_cap,
			  (unsigned long long)pt->data_off,
			  (unsigned long long)pt->data_cap,
			  (unsigned long long)pt->max_records);
	}

	/* Write each partition's initial meta_header (filemark_count == 0). */
	for (int i = 0; i < nparts; i++) {
		memset(zbuf, 0, RAW_META_HEADER_SIZE);
		if (pwrite(fd, zbuf, RAW_META_HEADER_SIZE,
			   (off_t)newsb.part[i].meta_off) !=
		    (ssize_t)RAW_META_HEADER_SIZE) {
			MHVTL_ERR("raw_format: init meta_header p%d failed: %s",
				  i, strerror(errno));
			goto out;
		}
	}

	/* A new cartridge carries a MAM saying what media it is and how much it
	 * holds; without one the drive sees no capacity and reports end of
	 * medium on the first write. */
	raw_init_mam(&newsb, barcode);
	if (raw_serialize_mam(&mam_buf, &mam_len) < 0)
		goto out;
	if (pwrite(fd, mam_buf, (size_t)mam_len, (off_t)newsb.mam_off) !=
	    (ssize_t)mam_len) {
		MHVTL_ERR("raw_format: MAM region write failed: %s",
			  strerror(errno));
		goto out;
	}
	newsb.mam_len = mam_len;

	/* The superblocks make the cartridge valid, so the regions they describe
	 * go to stable storage first. Then mirror and primary, each flushed, in
	 * the order sb_persist() uses. */
	if (fdatasync(fd) < 0) {
		MHVTL_ERR("raw_format: fdatasync failed: %s", strerror(errno));
		goto out;
	}
	newsb.header_crc32c = crc32c(0, &newsb, MVTL_SB_CRC_LEN);
	if (pwrite(fd, &newsb, sizeof(newsb), MVTL_SB_MIRROR_OFF) !=
		    (ssize_t)sizeof(newsb) || fdatasync(fd) < 0) {
		MHVTL_ERR("raw_format: mirror superblock write failed: %s",
			  strerror(errno));
		goto out;
	}
	if (pwrite(fd, &newsb, sizeof(newsb), MVTL_SB_PRIMARY_OFF) !=
		    (ssize_t)sizeof(newsb) || fdatasync(fd) < 0) {
		MHVTL_ERR("raw_format: primary superblock write failed: %s",
			  strerror(errno));
		goto out;
	}

	MHVTL_LOG("raw_format: %s formatted, barcode '%s', %d partition(s)",
		  devpath, newsb.barcode, nparts);
	rc = 0;

out:
	free(mam_buf);
	free(zbuf);
	close(fd);
	return rc;
}
