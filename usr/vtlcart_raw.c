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
#include <linux/fs.h>		/* BLKGETSIZE64, BLKSSZGET, BLKFLSBUF */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "logging.h"
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
	return pwrite(devfd, buf, len, (off_t)region_off(s, partition) + off);
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
