/*
 * Unit tests for update_VolumeStatistics() in usr/mhvtl_log.c
 *
 * Demonstrates the packing bug: when mam.num_partitions < MAX_PARTITIONS,
 * update_VolumeStatistics uses pointer arithmetic to pack partition records
 * tightly, but the result no longer matches the VolumeStatistics_pg struct
 * layout. A SCSI initiator walking the log page by pc_header.len will see
 * wrong parameter codes for subsequent fields.
 *
 * Strategy: We pull in just the function under test by including the
 * relevant source snippet with stubs for the heavy dependencies.
 */
#include "acutest.h"

#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>

/* No debug logging */
#include "vtl_common.h"
#include "be_byteshift.h"

/* Stub syslog */
void syslog(int priority, const char *fmt, ...) { (void)priority; (void)fmt; }

/* Globals that the function references */
uint8_t verbose = 0;
uint8_t debug = 0;
char *mhvtl_driver_name = "test";
long my_id = 1;
int current_state = 0;

/* Need ssc.h for struct priv_lu_ssc */
#include "vtllib.h"
#include "ssc.h"
#include "mhvtl_log.h"

struct MAM mam;
struct priv_lu_ssc lu_ssc;
struct lu_phy_attr lunit;

/* Controllable tape load status for tests */
static int fake_tape_status = TAPE_LOADED;
int get_tape_load_status(void) { return fake_tape_status; }

/*
 * Pull in just the SET_VOLSTAT_PARAM_H macros and update_VolumeStatistics.
 * These are copy-pasted from usr/mhvtl_log.c to avoid linking the whole file
 * which has many transitive dependencies.
 */
#define SET_VOLSTAT_PARAM_H(paramCode, paramFlags, paramStruct)                               \
	do {                                                                                      \
		put_unaligned_be16((paramCode), header);                                              \
		((struct pc_header *)header)->flags = (paramFlags);                                   \
		((struct pc_header *)header)->len   = mam.num_partitions * sizeof(paramStruct);       \
		for (i = 0; i < mam.num_partitions; ++i) {                                            \
			paramStruct *p_header = ((paramStruct *)(header + sizeof(struct pc_header))) + i; \
			p_header->header.len  = sizeof(paramStruct) - 1;                                  \
			put_unaligned_be16(i, &p_header->header.partition_no);                            \
		}                                                                                     \
	} while (0)
#define SET_VOLSTAT_PARAM_H4(paramCode, paramFlags) \
	SET_VOLSTAT_PARAM_H((paramCode), (paramFlags), struct partition_record_size4)
#define SET_VOLSTAT_PARAM_H6(paramCode, paramFlags) \
	SET_VOLSTAT_PARAM_H((paramCode), (paramFlags), struct partition_record_size6)

/* Exact copy of update_VolumeStatistics from usr/mhvtl_log.c (after fix) */
static size_t test_update_VolumeStatistics(struct VolumeStatistics_pg *pg, struct priv_lu_ssc *lu_priv) {
	uint8_t *header;
	uint64_t cap __attribute__((unused));
	int      i;

	memset(&pg->h_FirstEncryptedLogicalObj, 0,
		   sizeof(struct VolumeStatistics_pg) - offsetof(struct VolumeStatistics_pg, h_FirstEncryptedLogicalObj));

	/* h_FirstEncryptedLogicalObj */
	header = (uint8_t *)pg + offsetof(struct VolumeStatistics_pg, h_FirstEncryptedLogicalObj);
	SET_VOLSTAT_PARAM_H6(0x0200, 0x03);
	for (i = 0; i < mam.num_partitions; ++i) {
		struct partition_record_size6 *p_header =
			((struct partition_record_size6 *)(header + sizeof(struct pc_header))) + i;
		put_unaligned_be48(0, &p_header->data);
	}

	/* h_FirstUnencryptedLogicalObj */
	header += sizeof(struct pc_header) + ((struct pc_header *)header)->len;
	SET_VOLSTAT_PARAM_H6(0x0201, 0x03);
	for (i = 0; i < mam.num_partitions; ++i) {
		struct partition_record_size6 *p_header =
			((struct partition_record_size6 *)(header + sizeof(struct pc_header))) + i;
		put_unaligned_be48(0, &p_header->data);
	}

	/* h_ApproxNativeCapacityPartition */
	header += sizeof(struct pc_header) + ((struct pc_header *)header)->len;
	SET_VOLSTAT_PARAM_H4(0x0202, 0x03);
	if (get_tape_load_status() == TAPE_LOADED) {
		for (i = 0; i < mam.num_partitions; ++i) {
			struct partition_record_size4 *p_header =
				((struct partition_record_size4 *)(header + sizeof(struct pc_header))) + i;
			cap = get_unaligned_be64(&mam.max_capacity) / lu_priv->capacity_unit;
			put_unaligned_be32(0xfffffffe, &p_header->data);
		}
	}

	/* h_ApproxUsedNativeCapacityPartition */
	header += sizeof(struct pc_header) + ((struct pc_header *)header)->len;
	SET_VOLSTAT_PARAM_H4(0x0203, 0x03);
	for (i = 0; i < mam.num_partitions; ++i) {
		struct partition_record_size4 *p_header =
			((struct partition_record_size4 *)(header + sizeof(struct pc_header))) + i;
		put_unaligned_be32(0xfffffffe, &p_header->data);
	}

	/* h_RemainingCapacityToEWPartition */
	header += sizeof(struct pc_header) + ((struct pc_header *)header)->len;
	SET_VOLSTAT_PARAM_H4(0x0204, 0x03);
	if (get_tape_load_status() == TAPE_LOADED) {
		for (i = 0; i < mam.num_partitions; ++i) {
			struct partition_record_size4 *p_header =
				((struct partition_record_size4 *)(header + sizeof(struct pc_header))) + i;
			cap = get_unaligned_be64(&mam.remaining_capacity) / lu_priv->capacity_unit;
			put_unaligned_be32(0xfffffffe, &p_header->data);
		}
	}

	/* Return total packed page size */
	header += sizeof(struct pc_header) + ((struct pc_header *)header)->len;
	return (size_t)(header - (uint8_t *)pg);
}

/*
 * Helper: walk a packed SCSI log page starting from a given offset,
 * extracting parameter codes. Each parameter is: pc_header (4 bytes) +
 * pc_header.len bytes of data.
 */
static int walk_packed_params(const uint8_t *buf, size_t start_offset,
                              size_t buf_size, uint16_t *param_codes,
                              int max_params, size_t *total_sz) {
	size_t pos = start_offset;
	int count = 0;

	while (pos + sizeof(struct pc_header) <= buf_size && count < max_params) {
		const struct pc_header *h = (const struct pc_header *)(buf + pos);
		uint16_t pcode = get_unaligned_be16(&h->head0);

		/* Stop if we hit a zeroed-out header (end of packed data) */
		if (pcode == 0 && h->flags == 0 && h->len == 0)
			break;

		param_codes[count++] = pcode;
		pos += sizeof(struct pc_header) + h->len;
	}

	if (total_sz)
		*total_sz = pos;

	return count;
}

/*
 * Test: With num_partitions=1, the partition-dependent parameters (0x0200-0x0204)
 * should still appear in order when walking the packed buffer by pc_header.len.
 *
 * BUG: update_VolumeStatistics packs tightly using pointer arithmetic, but
 * ssc_log_sense reports l->size = sizeof(VolumeStatistics_pg) which assumes
 * MAX_PARTITIONS=4. With 1 partition, param 0x0201's header lands in the
 * middle of FirstEncryptedLogicalObj[1..3] array slots — the struct abstraction
 * is broken.
 */
void test_volstat_packing_1_partition(void) {
	struct VolumeStatistics_pg pg;
	struct priv_lu_ssc priv;

	memset(&pg, 0, sizeof(pg));
	memset(&priv, 0, sizeof(priv));
	memset(&mam, 0, sizeof(mam));

	mam.num_partitions = 1;
	priv.capacity_unit = 1;
	put_unaligned_be64(1000000, &mam.max_capacity);
	put_unaligned_be64(500000, &mam.remaining_capacity);
	fake_tape_status = TAPE_LOADED;

	pg.pcode_head.pcode = VOLUME_STATISTICS;

	/* Call the function under test */
	test_update_VolumeStatistics(&pg, &priv);

	/* Walk the packed buffer starting at the first partition-dependent param */
	size_t start = offsetof(struct VolumeStatistics_pg, h_FirstEncryptedLogicalObj);
	uint16_t codes[16];
	size_t packed_end;
	int n = walk_packed_params((uint8_t *)&pg, start, sizeof(pg), codes, 16, &packed_end);

	/* We expect exactly 5 partition-dependent parameters: 0x0200..0x0204 */
	TEST_CHECK(n == 5);
	TEST_MSG("Expected 5 partition params, got %d", n);

	if (n >= 5) {
		TEST_CHECK(codes[0] == 0x0200);
		TEST_MSG("param[0] = 0x%04x, expected 0x0200", codes[0]);

		TEST_CHECK(codes[1] == 0x0201);
		TEST_MSG("param[1] = 0x%04x, expected 0x0201", codes[1]);

		TEST_CHECK(codes[2] == 0x0202);
		TEST_MSG("param[2] = 0x%04x, expected 0x0202", codes[2]);

		TEST_CHECK(codes[3] == 0x0203);
		TEST_MSG("param[3] = 0x%04x, expected 0x0203", codes[3]);

		TEST_CHECK(codes[4] == 0x0204);
		TEST_MSG("param[4] = 0x%04x, expected 0x0204", codes[4]);
	}

	/* With 1 partition, packed size must be smaller than struct size */
	size_t struct_end = sizeof(struct VolumeStatistics_pg);
	TEST_CHECK(packed_end < struct_end);
	TEST_MSG("Packed end (%zu) should be < struct end (%zu) with 1 partition",
			 packed_end, struct_end);

	/* Verify each partition-dependent param has len for exactly 1 partition */
	const uint8_t *buf = (const uint8_t *)&pg;
	size_t pos = start;

	/* 0x0200: size6 records */
	const struct pc_header *h = (const struct pc_header *)(buf + pos);
	TEST_CHECK(h->len == 1 * sizeof(struct partition_record_size6));
	TEST_MSG("0x0200 len=%u, expected %zu", h->len, sizeof(struct partition_record_size6));
	pos += sizeof(struct pc_header) + h->len;

	/* 0x0201: size6 records */
	h = (const struct pc_header *)(buf + pos);
	TEST_CHECK(h->len == 1 * sizeof(struct partition_record_size6));
	TEST_MSG("0x0201 len=%u, expected %zu", h->len, sizeof(struct partition_record_size6));
	pos += sizeof(struct pc_header) + h->len;

	/* 0x0202: size4 records */
	h = (const struct pc_header *)(buf + pos);
	TEST_CHECK(h->len == 1 * sizeof(struct partition_record_size4));
	TEST_MSG("0x0202 len=%u, expected %zu", h->len, sizeof(struct partition_record_size4));
}

/*
 * Test: With num_partitions=4 (MAX_PARTITIONS), the packed output should
 * match the struct layout exactly — no packing mismatch.
 */
void test_volstat_packing_4_partitions(void) {
	struct VolumeStatistics_pg pg;
	struct priv_lu_ssc priv;

	memset(&pg, 0, sizeof(pg));
	memset(&priv, 0, sizeof(priv));
	memset(&mam, 0, sizeof(mam));

	mam.num_partitions = 4;
	priv.capacity_unit = 1;
	put_unaligned_be64(1000000, &mam.max_capacity);
	put_unaligned_be64(500000, &mam.remaining_capacity);
	fake_tape_status = TAPE_LOADED;

	pg.pcode_head.pcode = VOLUME_STATISTICS;

	test_update_VolumeStatistics(&pg, &priv);

	/* With 4 partitions, the packed data should use the full struct area */
	size_t start = offsetof(struct VolumeStatistics_pg, h_FirstEncryptedLogicalObj);
	uint16_t codes[16];
	size_t packed_end;
	int n = walk_packed_params((uint8_t *)&pg, start, sizeof(pg), codes, 16, &packed_end);

	TEST_CHECK(n == 5);
	TEST_MSG("Expected 5 partition params, got %d", n);

	if (n >= 5) {
		TEST_CHECK(codes[0] == 0x0200);
		TEST_CHECK(codes[1] == 0x0201);
		TEST_CHECK(codes[2] == 0x0202);
		TEST_CHECK(codes[3] == 0x0203);
		TEST_CHECK(codes[4] == 0x0204);
	}

	/* With MAX_PARTITIONS, packed end should equal struct end */
	TEST_CHECK(packed_end == sizeof(struct VolumeStatistics_pg));
	TEST_MSG("Packed end (%zu) should equal struct end (%zu) with 4 partitions",
			 packed_end, sizeof(struct VolumeStatistics_pg));
}

/*
 * Test: The actual packed page size differs from sizeof(VolumeStatistics_pg)
 * when num_partitions < MAX_PARTITIONS. This means ssc_log_sense must update
 * dbuf_p->sz to the packed size, not use l->size (which is the struct size).
 *
 * This test documents the expected packed sizes.
 */
void test_volstat_packed_size_differs(void) {
	size_t fixed_part = offsetof(struct VolumeStatistics_pg, h_FirstEncryptedLogicalObj);
	size_t struct_size = sizeof(struct VolumeStatistics_pg);

	for (int npart = 1; npart <= MAX_PARTITIONS; npart++) {
		size_t expected_packed =
			fixed_part +
			2 * (sizeof(struct pc_header) + npart * sizeof(struct partition_record_size6)) +
			3 * (sizeof(struct pc_header) + npart * sizeof(struct partition_record_size4));

		if (npart < MAX_PARTITIONS) {
			TEST_CHECK(expected_packed < struct_size);
			TEST_MSG("npart=%d: expected_packed=%zu < struct_size=%zu",
					 npart, expected_packed, struct_size);
		} else {
			TEST_CHECK(expected_packed == struct_size);
			TEST_MSG("npart=%d: expected_packed=%zu == struct_size=%zu",
					 npart, expected_packed, struct_size);
		}
	}
}

/*
 * Test: update_VolumeStatistics now returns the total packed page size.
 * Verify the returned size matches what walk_packed_params computes,
 * and that it's smaller than sizeof(VolumeStatistics_pg) for < MAX_PARTITIONS.
 */
void test_volstat_returned_size(void) {
	struct VolumeStatistics_pg pg;
	struct priv_lu_ssc priv;

	memset(&pg, 0, sizeof(pg));
	memset(&priv, 0, sizeof(priv));
	memset(&mam, 0, sizeof(mam));

	mam.num_partitions = 2;
	priv.capacity_unit = 1;
	fake_tape_status = TAPE_UNLOADED;

	size_t returned_sz = test_update_VolumeStatistics(&pg, &priv);

	/* Walk to find actual packed end */
	size_t start = offsetof(struct VolumeStatistics_pg, h_FirstEncryptedLogicalObj);
	uint16_t codes[16];
	size_t packed_end;
	walk_packed_params((uint8_t *)&pg, start, sizeof(pg), codes, 16, &packed_end);

	/* Returned size must match the walked packed end */
	TEST_CHECK(returned_sz == packed_end);
	TEST_MSG("Returned size (%zu) should equal walked packed end (%zu)",
			 returned_sz, packed_end);

	/* With 2 partitions, packed size must be smaller than struct size */
	TEST_CHECK(returned_sz < sizeof(pg));
	TEST_MSG("Returned size (%zu) < sizeof(VolumeStatistics_pg) (%zu)",
			 returned_sz, sizeof(pg));

	/* With MAX_PARTITIONS, returned size should equal struct size */
	memset(&pg, 0, sizeof(pg));
	mam.num_partitions = MAX_PARTITIONS;
	size_t full_sz = test_update_VolumeStatistics(&pg, &priv);
	TEST_CHECK(full_sz == sizeof(pg));
	TEST_MSG("With MAX_PARTITIONS, returned size (%zu) == sizeof(pg) (%zu)",
			 full_sz, sizeof(pg));
}

/*
 * Test: After update_VolumeStatistics with 1 partition, accessing the struct
 * fields by name gives wrong data. The struct's h_FirstUnencryptedLogicalObj
 * is at a fixed offset that assumes MAX_PARTITIONS=4 entries for the previous
 * field, but the packed data placed 0x0201's header right after 1 entry.
 *
 * This demonstrates the struct abstraction is broken.
 */
void test_volstat_struct_access_broken(void) {
	struct VolumeStatistics_pg pg;
	struct priv_lu_ssc priv;

	memset(&pg, 0, sizeof(pg));
	memset(&priv, 0, sizeof(priv));
	memset(&mam, 0, sizeof(mam));

	mam.num_partitions = 1;
	priv.capacity_unit = 1;
	put_unaligned_be64(1000000, &mam.max_capacity);
	put_unaligned_be64(500000, &mam.remaining_capacity);
	fake_tape_status = TAPE_LOADED;

	test_update_VolumeStatistics(&pg, &priv);

	/* The struct field h_FirstUnencryptedLogicalObj is at a fixed offset
	 * that assumes 4 partition_record_size6 entries before it.
	 * After packing with 1 partition, it should contain param code 0x0201.
	 * But it WON'T — the packed 0x0201 is at a different (earlier) offset. */
	uint16_t struct_pcode = get_unaligned_be16(&pg.h_FirstUnencryptedLogicalObj.head0);

	/* If struct access worked, this would be 0x0201.
	 * After packing with 1 partition, it's 0x0000 (zeroed by memset)
	 * because the packed data was written earlier in the buffer. */
	TEST_CHECK(struct_pcode != 0x0201);
	TEST_MSG("Struct field h_FirstUnencryptedLogicalObj contains 0x%04x "
			 "(expected NOT 0x0201 — proves struct access is broken after packing)",
			 struct_pcode);

	/* Similarly, h_ApproxNativeCapacityPartition at struct offset should be zeroed */
	uint16_t struct_pcode2 = get_unaligned_be16(&pg.h_ApproxNativeCapacityPartition.head0);
	TEST_CHECK(struct_pcode2 != 0x0202);
	TEST_MSG("Struct field h_ApproxNativeCapacityPartition contains 0x%04x "
			 "(expected NOT 0x0202 — proves struct access is broken)", struct_pcode2);
}

TEST_LIST = {
	{"volstat_struct_access_broken", test_volstat_struct_access_broken},
	{"volstat_packing_1_partition", test_volstat_packing_1_partition},
	{"volstat_packing_4_partitions", test_volstat_packing_4_partitions},
	{"volstat_packed_size_differs", test_volstat_packed_size_differs},
	{"volstat_returned_size", test_volstat_returned_size},
	{NULL, NULL}
};
