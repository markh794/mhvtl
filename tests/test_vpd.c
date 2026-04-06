/*
 * Unit tests for VPD page allocation and mode page management.
 *
 * Tests:
 *   - alloc_vpd / dealloc_vpd  (from usr/spc.c)
 *   - alloc_mode_page / lookup_mode_pg / dealloc_all_mode_pages
 *     (from usr/mode.c)
 *
 * These functions use zalloc (malloc+memset) and linked lists. We
 * re-implement the minimal subset needed rather than linking the
 * full daemon object files.
 */
#include "acutest.h"

#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>

/* Stub syslog */
void syslog(int priority, const char *fmt, ...) { (void)priority; (void)fmt; }
int verbose = 0;
int debug = 0;
char *mhvtl_driver_name = "test";

/* ---- kernel-style list from the project ---- */
#include "mhvtl_list.h"

/* ---- zalloc from vtllib.c ---- */
static void *zalloc(int sz) {
	void *p = malloc(sz);
	if (p)
		memset(p, 0, sz);
	return p;
}

/* ---- VPD structures (from vtllib.h) ---- */
struct vpd {
	uint16_t sz;
	uint8_t *data;
};

/* ---- Mode structures (from vtllib.h) ---- */
struct mode {
	struct list_head siblings;
	uint8_t          pcode;
	uint8_t          subpcode;
	int32_t          pcodeSize;
	uint8_t         *pcodePointerBitMap;
	uint8_t         *pcodePointer;
	char            *description;
};

/* Minimal lu_phy_attr for mode page dealloc */
struct lu_phy_attr {
	struct list_head mode_pg;
};

/* ---- Re-implement alloc_vpd / dealloc_vpd (from spc.c) ---- */

static struct vpd *alloc_vpd(uint16_t sz) {
	struct vpd *vpd_pg;

	vpd_pg = zalloc(sizeof(struct vpd));
	if (!vpd_pg)
		return NULL;
	vpd_pg->data = zalloc(sz);
	if (!vpd_pg->data) {
		free(vpd_pg);
		return NULL;
	}
	vpd_pg->sz = sz;
	return vpd_pg;
}

static void dealloc_vpd(struct vpd *pg) {
	free(pg->data);
	free(pg);
}

/* ---- Re-implement mode page functions (from mode.c) ---- */

static struct mode *lookup_mode_pg(struct list_head *m, uint8_t page,
								   uint8_t subpage) {
	struct mode *mp;
	list_for_each_entry(mp, m, siblings) {
		if (mp->pcode == page && mp->subpcode == subpage)
			return mp;
	}
	return NULL;
}

static struct mode *alloc_mode_page(struct list_head *m,
									uint8_t pcode, uint8_t subpcode,
									int size) {
	struct mode *mp;

	mp = lookup_mode_pg(m, pcode, subpcode);
	if (!mp)
		mp = (struct mode *)zalloc(sizeof(struct mode));
	if (mp) {
		mp->pcodePointer = (uint8_t *)zalloc(size);
		if (mp->pcodePointer) {
			mp->pcode     = pcode;
			mp->subpcode  = subpcode;
			mp->pcodeSize = size;
			mp->pcodePointerBitMap = zalloc(size);
			if (!mp->pcodePointerBitMap) {
				free(mp->pcodePointer);
				free(mp);
				return NULL;
			}
			list_add_tail(&mp->siblings, m);
			return mp;
		} else {
			free(mp);
		}
	}
	return NULL;
}

static void dealloc_all_mode_pages(struct lu_phy_attr *lu) {
	struct mode *mp, *mn;
	list_for_each_entry_safe(mp, mn, &lu->mode_pg, siblings) {
		free(mp->pcodePointer);
		free(mp->pcodePointerBitMap);
		list_del(&mp->siblings);
		free(mp);
	}
}

/* ================================================================
 * Tests: VPD allocation
 * ================================================================ */

void test_alloc_vpd_basic(void) {
	struct vpd *pg = alloc_vpd(64);
	TEST_CHECK(pg != NULL);
	TEST_CHECK(pg->sz == 64);
	TEST_CHECK(pg->data != NULL);
	/* Data should be zeroed */
	for (int i = 0; i < 64; i++) {
		TEST_CHECK(pg->data[i] == 0);
	}
	dealloc_vpd(pg);
}

void test_alloc_vpd_small(void) {
	struct vpd *pg = alloc_vpd(1);
	TEST_CHECK(pg != NULL);
	TEST_CHECK(pg->sz == 1);
	dealloc_vpd(pg);
}

void test_alloc_vpd_large(void) {
	struct vpd *pg = alloc_vpd(4096);
	TEST_CHECK(pg != NULL);
	TEST_CHECK(pg->sz == 4096);
	/* Write to last byte to verify size */
	pg->data[4095] = 0xAA;
	TEST_CHECK(pg->data[4095] == 0xAA);
	dealloc_vpd(pg);
}

void test_alloc_vpd_write_data(void) {
	struct vpd *pg = alloc_vpd(16);
	TEST_CHECK(pg != NULL);
	memset(pg->data, 0xBB, 16);
	TEST_CHECK(pg->data[0] == 0xBB);
	TEST_CHECK(pg->data[15] == 0xBB);
	dealloc_vpd(pg);
}

/* ================================================================
 * Tests: Mode page allocation and lookup
 * ================================================================ */

void test_alloc_mode_page_basic(void) {
	struct lu_phy_attr lu;
	INIT_LIST_HEAD(&lu.mode_pg);

	struct mode *mp = alloc_mode_page(&lu.mode_pg, 0x0A, 0x00, 12);
	TEST_CHECK(mp != NULL);
	TEST_CHECK(mp->pcode == 0x0A);
	TEST_CHECK(mp->subpcode == 0x00);
	TEST_CHECK(mp->pcodeSize == 12);
	TEST_CHECK(mp->pcodePointer != NULL);
	TEST_CHECK(mp->pcodePointerBitMap != NULL);

	for (int i = 0; i < 12; i++)
		TEST_CHECK(mp->pcodePointer[i] == 0);

	dealloc_all_mode_pages(&lu);
}

void test_lookup_mode_pg_found(void) {
	struct lu_phy_attr lu;
	INIT_LIST_HEAD(&lu.mode_pg);

	alloc_mode_page(&lu.mode_pg, 0x0A, 0x00, 12);
	alloc_mode_page(&lu.mode_pg, 0x0F, 0x00, 16);
	alloc_mode_page(&lu.mode_pg, 0x1C, 0x01, 8);

	struct mode *mp = lookup_mode_pg(&lu.mode_pg, 0x0F, 0x00);
	TEST_CHECK(mp != NULL);
	TEST_CHECK(mp->pcode == 0x0F);
	TEST_CHECK(mp->pcodeSize == 16);

	mp = lookup_mode_pg(&lu.mode_pg, 0x1C, 0x01);
	TEST_CHECK(mp != NULL);
	TEST_CHECK(mp->subpcode == 0x01);

	dealloc_all_mode_pages(&lu);
}

void test_lookup_mode_pg_not_found(void) {
	struct lu_phy_attr lu;
	INIT_LIST_HEAD(&lu.mode_pg);

	alloc_mode_page(&lu.mode_pg, 0x0A, 0x00, 12);

	struct mode *mp = lookup_mode_pg(&lu.mode_pg, 0x0B, 0x00);
	TEST_CHECK(mp == NULL);

	/* Wrong subpage */
	mp = lookup_mode_pg(&lu.mode_pg, 0x0A, 0x01);
	TEST_CHECK(mp == NULL);

	dealloc_all_mode_pages(&lu);
}

void test_lookup_mode_pg_empty_list(void) {
	LIST_HEAD(mode_list);

	struct mode *mp = lookup_mode_pg(&mode_list, 0x0A, 0x00);
	TEST_CHECK(mp == NULL);
}

void test_dealloc_all_mode_pages(void) {
	struct lu_phy_attr lu;
	INIT_LIST_HEAD(&lu.mode_pg);

	alloc_mode_page(&lu.mode_pg, 0x01, 0x00, 10);
	alloc_mode_page(&lu.mode_pg, 0x02, 0x00, 20);
	alloc_mode_page(&lu.mode_pg, 0x03, 0x00, 30);

	/* Should not be empty */
	TEST_CHECK(!list_empty(&lu.mode_pg));

	dealloc_all_mode_pages(&lu);

	/* Should be empty now */
	TEST_CHECK(list_empty(&lu.mode_pg));
}

void test_mode_page_write_data(void) {
	struct lu_phy_attr lu;
	INIT_LIST_HEAD(&lu.mode_pg);

	struct mode *mp = alloc_mode_page(&lu.mode_pg, 0x0A, 0x00, 12);
	TEST_CHECK(mp != NULL);

	mp->pcodePointer[0] = 0x0A;
	mp->pcodePointer[1] = 10;
	mp->pcodePointer[2] = 0x04;

	struct mode *found = lookup_mode_pg(&lu.mode_pg, 0x0A, 0x00);
	TEST_CHECK(found != NULL);
	TEST_CHECK(found->pcodePointer[0] == 0x0A);
	TEST_CHECK(found->pcodePointer[1] == 10);
	TEST_CHECK(found->pcodePointer[2] == 0x04);

	dealloc_all_mode_pages(&lu);
}

void test_mode_page_bitmap_independent(void) {
	struct lu_phy_attr lu;
	INIT_LIST_HEAD(&lu.mode_pg);

	struct mode *mp = alloc_mode_page(&lu.mode_pg, 0x0A, 0x00, 8);
	TEST_CHECK(mp != NULL);

	mp->pcodePointer[0] = 0xFF;
	TEST_CHECK(mp->pcodePointerBitMap[0] == 0x00);

	mp->pcodePointerBitMap[1] = 0xFF;
	TEST_CHECK(mp->pcodePointer[1] == 0x00);

	dealloc_all_mode_pages(&lu);
}

TEST_LIST = {
	/* VPD */
	{ "alloc_vpd_basic",      test_alloc_vpd_basic },
	{ "alloc_vpd_small",      test_alloc_vpd_small },
	{ "alloc_vpd_large",      test_alloc_vpd_large },
	{ "alloc_vpd_write_data", test_alloc_vpd_write_data },
	/* Mode pages */
	{ "alloc_mode_page_basic",    test_alloc_mode_page_basic },
	{ "lookup_mode_pg_found",     test_lookup_mode_pg_found },
	{ "lookup_mode_pg_not_found", test_lookup_mode_pg_not_found },
	{ "lookup_mode_pg_empty",     test_lookup_mode_pg_empty_list },
	{ "dealloc_all_mode_pages",   test_dealloc_all_mode_pages },
	{ "mode_page_write_data",     test_mode_page_write_data },
	{ "mode_page_bitmap_independent", test_mode_page_bitmap_independent },
	{ NULL, NULL }
};
