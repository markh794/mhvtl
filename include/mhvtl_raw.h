/*
 * mhvtl_raw.h — on-disk format for the raw block-device cartridge backend.
 *
 * One physical disk = one cartridge. Layout (absolute byte offsets; LBAs are
 * 512 B only for the reserved-area description — all struct offsets are bytes):
 *
 *   0          .. 1 MiB-1   Reserved / protective (no host auto-mount)
 *   1 MiB                   Primary superblock   (struct mhvtl_raw_superblock)
 *   2 MiB                   Mirror  superblock   (redundant copy)
 *   3 MiB      .. 4 MiB-1   MAM reserved region  (length-delimited, mam_off/len)
 *   4 MiB      .. EOD       Per-partition data/indx/meta regions
 *
 * See docs/design/raw-disk-backend.md.
 */
#ifndef _MHVTL_RAW_H_
#define _MHVTL_RAW_H_

#include <stdint.h>
#include <stddef.h>
#include <assert.h>

#define MVTL_MAGIC		0x4D56544Cu	/* "MVTL" */
#define MVTL_VERSION		2u		/* on-disk layout version */
#define MVTL_MAX_PARTITIONS	4		/* must equal MAX_PARTITIONS */

/* Byte offsets of the fixed regions. */
#define MVTL_RESERVED_BYTES	(1ull << 20)	/* 1 MiB protective area       */
#define MVTL_SB_PRIMARY_OFF	(1ull << 20)	/* primary superblock @ 1 MiB  */
#define MVTL_SB_MIRROR_OFF	(2ull << 20)	/* mirror  superblock @ 2 MiB  */
#define MVTL_MAM_OFF		(3ull << 20)	/* MAM region @ 3 MiB          */
#define MVTL_MAM_RESERVE	(1ull << 20)	/* MAM region capacity (1 MiB) */
#define MVTL_DATA_START		(4ull << 20)	/* partitions begin @ 4 MiB    */

/* Per-partition region table entry. All offsets are absolute device bytes. */
struct __attribute__((__packed__)) mhvtl_raw_partition {
	uint64_t data_off, data_cap, data_used;		/* payload stream        */
	uint64_t indx_off, indx_cap, indx_used;		/* raw_header index array */
	uint64_t meta_off, meta_cap, meta_used;		/* meta_header+filemarks  */
	uint64_t max_records;		/* indx_cap / 512 (capacity-planning cap) */
	uint64_t advertised_capacity;	/* capacity the emulated tape reports   */
};

struct __attribute__((__packed__)) mhvtl_raw_superblock {
	uint32_t magic;			/* MVTL_MAGIC                            */
	uint32_t header_len;		/* exact #bytes covered by header_crc32c */
	uint16_t version;		/* MVTL_VERSION                          */
	uint16_t logical_sector;	/* device logical sector at format time  */
	uint8_t  cartridge_uuid[16];	/* random, immutable durable identity    */
	char     barcode[32];		/* must match the requested pcl          */
	char     vendor_id[8];
	char     product_id[16];
	uint64_t total_capacity_bytes;
	uint64_t generation_id;		/* monotonic; bumped on each durable commit */
	uint32_t num_partitions;
	uint32_t reserved0;		/* alignment / future use                */
	struct mhvtl_raw_partition part[MVTL_MAX_PARTITIONS];
	uint64_t mam_off;		/* MAM region offset (== MVTL_MAM_OFF)   */
	uint64_t mam_len;		/* current serialized MAM length         */
	uint32_t header_crc32c;		/* crc32c over the first header_len bytes */
};

/* The CRC covers everything up to (not including) the header_crc32c field. */
#define MVTL_SB_CRC_LEN	((uint32_t)offsetof(struct mhvtl_raw_superblock, header_crc32c))

/* Build-time invariants. */
_Static_assert(sizeof(struct mhvtl_raw_superblock) <=
		(MVTL_SB_MIRROR_OFF - MVTL_SB_PRIMARY_OFF),
		"raw superblock exceeds its reserved 1 MiB region");
_Static_assert(MVTL_DATA_START >= MVTL_MAM_OFF + MVTL_MAM_RESERVE,
		"MAM region overlaps partition data");
#ifdef MAX_PARTITIONS
_Static_assert(MVTL_MAX_PARTITIONS == MAX_PARTITIONS,
		"MVTL_MAX_PARTITIONS must equal MAX_PARTITIONS (vtllib.h)");
#endif

#endif /* _MHVTL_RAW_H_ */
