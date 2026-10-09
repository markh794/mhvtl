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
#include <linux/fs.h>		/* BLKGETSIZE64, BLKSSZGET, BLKFLSBUF */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "logging.h"
#include "mhvtl_scsi.h"
#include "vtlcart.h"		/* read_mam/write_mam, TAPE_FMT_VERSION */
#include "vtllib.h"		/* struct MAM, global mam, MAM_VERSION */
#include "mhvtl_raw.h"
#include "cart_backend.h"
#include "cart_backend_raw.h"
#include "ccan/crc32c/crc32c.h"

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

int raw_open(const char *devpath)
{
	int fd;
	uint64_t dev_bytes = 0;
	int sector = 0;
	struct mhvtl_raw_superblock primary, mirror;
	int vp, vm;

	fd = open(devpath, O_RDWR | O_LARGEFILE);
	if (fd < 0) {
		MHVTL_ERR("open of raw device %s failed: %s",
			  devpath, strerror(errno));
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
	sb_loaded = 1;

	MHVTL_DBG(1, "Opened raw cartridge %s: barcode '%s', %u partition(s), "
		  "generation %llu", devpath, sb.barcode, sb.num_partitions,
		  (unsigned long long)sb.generation_id);
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
	 * freshly-generationed superblock. sb_persist() does the ordering. */
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
	 * then release the device. */
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
static int raw_be_save_mam(struct MAM *mamp)
{
	int mfd = -1, hfd = -1, ret = -1;
	off_t mlen, hlen;
	uint32_t hdr[2];
	uint8_t *buf = NULL;
	uint64_t total;

	(void)mamp;	/* write_mam serializes the global mam */

	if (!sb_loaded || devfd < 0) {
		errno = EBADF;
		return -1;
	}

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
	if (mfd >= 0)
		close(mfd);
	if (hfd >= 0)
		close(hfd);
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

int raw_format(const char *devpath, const char *barcode,
	       const char *partspec, uint64_t min_record_size)
{
	int fd, nparts, rc = -1;
	uint64_t dev_bytes = 0, usable, alignment, part_base, consumed = 0;
	int sector = 0;
	unsigned pct[MVTL_MAX_PARTITIONS];
	void *zbuf = NULL;
	struct mhvtl_raw_superblock newsb;

	if (min_record_size == 0) {
		MHVTL_ERR("raw_format: min_record_size must be > 0");
		errno = EINVAL;
		return -1;
	}

	nparts = parse_partspec(partspec, pct);
	if (nparts < 0) {
		MHVTL_ERR("raw_format: invalid partspec '%s' (need per-partition "
			  "percentages totalling 100)", partspec ? partspec : "");
		return -1;
	}

	fd = open(devpath, O_RDWR | O_LARGEFILE);
	if (fd < 0) {
		MHVTL_ERR("raw_format: open %s failed: %s",
			  devpath, strerror(errno));
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

	/* All region offsets/sizes are aligned to max(logical_sector, 4096). */
	alignment = (uint64_t)sector > 4096ull ? (uint64_t)sector : 4096ull;

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

	/* Finalise and write both superblocks with a valid CRC. */
	newsb.header_crc32c = crc32c(0, &newsb, MVTL_SB_CRC_LEN);
	if (pwrite(fd, &newsb, sizeof(newsb), MVTL_SB_PRIMARY_OFF) !=
	    (ssize_t)sizeof(newsb)) {
		MHVTL_ERR("raw_format: primary superblock write failed: %s",
			  strerror(errno));
		goto out;
	}
	if (pwrite(fd, &newsb, sizeof(newsb), MVTL_SB_MIRROR_OFF) !=
	    (ssize_t)sizeof(newsb)) {
		MHVTL_ERR("raw_format: mirror superblock write failed: %s",
			  strerror(errno));
		goto out;
	}
	if (fdatasync(fd) < 0) {
		MHVTL_ERR("raw_format: fdatasync failed: %s", strerror(errno));
		goto out;
	}

	MHVTL_LOG("raw_format: %s formatted, barcode '%s', %d partition(s)",
		  devpath, newsb.barcode, nparts);
	rc = 0;

out:
	free(zbuf);
	close(fd);
	return rc;
}
