/*
 * Unit tests for SMC (Storage Medium Changer) slot/element functions.
 *
 * Tests slot_type, slot_number, status bit operations, move_cart.
 * These are extracted from usr/smc.c; since several are static, we
 * re-implement the pure logic here (they're small, pure functions).
 */
#include "acutest.h"

#include <string.h>
#include <stdint.h>
#include <stdlib.h>

/* Bring in the struct definitions and constants we need */
#include "vtl_common.h"
#include "mhvtl_scsi.h"

/* Element type codes from smc.h */
#define ANY              0
#define MEDIUM_TRANSPORT 1
#define STORAGE_ELEMENT  2
#define MAP_ELEMENT      3
#define DATA_TRANSFER    4

#define ROBOT_ARM  0

/* Status bits from vtllib.h */
#define STATUS_Full     0x01
#define STATUS_ImpExp   0x02
#define STATUS_Except   0x04
#define STATUS_Access   0x08

/* Minimal struct definitions matching the project headers */
struct m_info {
	uint32_t last_location;
	char     barcode[16 + 1];
};

struct s_info {
	uint32_t     slot_location;
	uint32_t     last_location;
	void        *drive;   /* struct d_info * — not needed for these tests */
	struct m_info *media;
	uint16_t     asc_ascq;
	uint8_t      status;
	uint8_t      element_type;
};

struct smc_personality_template {
	uint32_t start_drive;
	uint32_t start_picker;
	uint32_t start_map;
	uint32_t start_storage;
};

struct smc_priv {
	int num_drives;
	int num_picker;
	int num_map;
	int num_storage;
	struct smc_personality_template *pm;
};

/* Stub syslog */
#include <stdarg.h>
void syslog(int priority, const char *fmt, ...) { (void)priority; (void)fmt; }
int verbose = 0;
int debug = 0;
char *mhvtl_driver_name = "test";

/* ================================================================
 * Re-implement the static functions from smc.c (pure logic only)
 * ================================================================ */

static int slot_type(struct smc_priv *smc_p, int addr) {
	if ((addr >= (int)smc_p->pm->start_drive) &&
		(addr < (int)smc_p->pm->start_drive + smc_p->num_drives))
		return DATA_TRANSFER;
	if ((addr >= (int)smc_p->pm->start_picker) &&
		(addr < (int)smc_p->pm->start_picker + smc_p->num_picker))
		return MEDIUM_TRANSPORT;
	if ((addr >= (int)smc_p->pm->start_map) &&
		(addr < (int)smc_p->pm->start_map + smc_p->num_map))
		return MAP_ELEMENT;
	if ((addr >= (int)smc_p->pm->start_storage) &&
		(addr < (int)smc_p->pm->start_storage + smc_p->num_storage))
		return STORAGE_ELEMENT;
	return 0;
}

static int slot_number(struct smc_personality_template *pm, struct s_info *sp) {
	switch (sp->element_type) {
	case MEDIUM_TRANSPORT:
		return sp->slot_location - pm->start_picker + 1;
	case STORAGE_ELEMENT:
		return sp->slot_location - pm->start_storage + 1;
	case MAP_ELEMENT:
		return sp->slot_location - pm->start_map + 1;
	case DATA_TRANSFER:
		return sp->slot_location - pm->start_drive + 1;
	}
	return 0;
}

static int slotAccess(struct s_info *s) {
	return s->status & STATUS_Access;
}

static int slotOccupied(struct s_info *s) {
	return s->status & STATUS_Full;
}

static int is_map_slot(struct s_info *s) {
	return s->element_type == MAP_ELEMENT;
}

static void setAccessStatus(struct s_info *s, int flg) {
	if (flg)
		s->status |= STATUS_Access;
	else
		s->status &= ~STATUS_Access;
}

static void setImpExpStatus(struct s_info *s, int flg) {
	if (flg)
		s->status |= STATUS_ImpExp;
	else
		s->status &= ~STATUS_ImpExp;
}

static void setFullStatus(struct s_info *s, int flg) {
	if (flg)
		s->status |= STATUS_Full;
	else
		s->status &= ~STATUS_Full;
}

static void setSlotEmpty(struct s_info *s) { setFullStatus(s, 0); }
static void setSlotFull(struct s_info *s)  { setFullStatus(s, 1); }

static void move_cart(struct s_info *src, struct s_info *dest) {
	dest->media = src->media;
	dest->last_location        = src->slot_location;
	dest->media->last_location = src->slot_location;
	setSlotFull(dest);
	if (is_map_slot(dest))
		setImpExpStatus(dest, ROBOT_ARM);
	src->media         = NULL;
	src->last_location = 0;
	setSlotEmpty(src);
	setAccessStatus(src, 1);
}

/* ================================================================
 * Test fixture: a small library layout
 *
 *   Drives:  addresses 500-503 (4 drives)
 *   Picker:  address 0         (1 picker)
 *   MAP:     addresses 1000-1003 (4 MAP slots)
 *   Storage: addresses 2000-2031 (32 storage slots)
 * ================================================================ */

static struct smc_personality_template test_pm = {
	.start_drive   = 500,
	.start_picker  = 0,
	.start_map     = 1000,
	.start_storage = 2000,
};

static struct smc_priv test_smc = {
	.num_drives  = 4,
	.num_picker  = 1,
	.num_map     = 4,
	.num_storage = 32,
	.pm          = &test_pm,
};

/* ================================================================
 * Tests: slot_type
 * ================================================================ */

void test_slot_type_drive(void) {
	TEST_CHECK(slot_type(&test_smc, 500) == DATA_TRANSFER);
	TEST_CHECK(slot_type(&test_smc, 503) == DATA_TRANSFER);
}

void test_slot_type_picker(void) {
	TEST_CHECK(slot_type(&test_smc, 0) == MEDIUM_TRANSPORT);
}

void test_slot_type_map(void) {
	TEST_CHECK(slot_type(&test_smc, 1000) == MAP_ELEMENT);
	TEST_CHECK(slot_type(&test_smc, 1003) == MAP_ELEMENT);
}

void test_slot_type_storage(void) {
	TEST_CHECK(slot_type(&test_smc, 2000) == STORAGE_ELEMENT);
	TEST_CHECK(slot_type(&test_smc, 2031) == STORAGE_ELEMENT);
}

void test_slot_type_invalid(void) {
	TEST_CHECK(slot_type(&test_smc, 9999) == 0);
	TEST_CHECK(slot_type(&test_smc, 504)  == 0);
	TEST_CHECK(slot_type(&test_smc, 1004) == 0);
}

void test_slot_type_boundary(void) {
	/* One past the end of each range should NOT match */
	TEST_CHECK(slot_type(&test_smc, 504)  != DATA_TRANSFER);
	TEST_CHECK(slot_type(&test_smc, 1)    != MEDIUM_TRANSPORT);
	TEST_CHECK(slot_type(&test_smc, 1004) != MAP_ELEMENT);
	TEST_CHECK(slot_type(&test_smc, 2032) != STORAGE_ELEMENT);
}

/* ================================================================
 * Tests: slot_number
 * ================================================================ */

void test_slot_number_drive(void) {
	struct s_info s = { .slot_location = 500, .element_type = DATA_TRANSFER };
	TEST_CHECK(slot_number(&test_pm, &s) == 1);

	s.slot_location = 503;
	TEST_CHECK(slot_number(&test_pm, &s) == 4);
}

void test_slot_number_storage(void) {
	struct s_info s = { .slot_location = 2000, .element_type = STORAGE_ELEMENT };
	TEST_CHECK(slot_number(&test_pm, &s) == 1);

	s.slot_location = 2031;
	TEST_CHECK(slot_number(&test_pm, &s) == 32);
}

void test_slot_number_map(void) {
	struct s_info s = { .slot_location = 1002, .element_type = MAP_ELEMENT };
	TEST_CHECK(slot_number(&test_pm, &s) == 3);
}

void test_slot_number_picker(void) {
	struct s_info s = { .slot_location = 0, .element_type = MEDIUM_TRANSPORT };
	TEST_CHECK(slot_number(&test_pm, &s) == 1);
}

void test_slot_number_unknown_type(void) {
	struct s_info s = { .slot_location = 0, .element_type = 99 };
	TEST_CHECK(slot_number(&test_pm, &s) == 0);
}

/* ================================================================
 * Tests: status bit operations
 * ================================================================ */

void test_slot_access(void) {
	struct s_info s = { .status = 0 };

	TEST_CHECK(slotAccess(&s) == 0);
	setAccessStatus(&s, 1);
	TEST_CHECK(slotAccess(&s) != 0);
	setAccessStatus(&s, 0);
	TEST_CHECK(slotAccess(&s) == 0);
}

void test_slot_occupied(void) {
	struct s_info s = { .status = 0 };

	TEST_CHECK(slotOccupied(&s) == 0);
	setSlotFull(&s);
	TEST_CHECK(slotOccupied(&s) != 0);
	setSlotEmpty(&s);
	TEST_CHECK(slotOccupied(&s) == 0);
}

void test_imp_exp_status(void) {
	struct s_info s = { .status = 0 };

	TEST_CHECK((s.status & STATUS_ImpExp) == 0);
	setImpExpStatus(&s, 1);
	TEST_CHECK(s.status & STATUS_ImpExp);
	setImpExpStatus(&s, 0);
	TEST_CHECK((s.status & STATUS_ImpExp) == 0);
}

void test_status_bits_independent(void) {
	struct s_info s = { .status = 0 };

	/* Set all, then clear one — others should remain */
	setAccessStatus(&s, 1);
	setFullStatus(&s, 1);
	setImpExpStatus(&s, 1);
	TEST_CHECK(s.status == (STATUS_Access | STATUS_Full | STATUS_ImpExp));

	setFullStatus(&s, 0);
	TEST_CHECK(s.status == (STATUS_Access | STATUS_ImpExp));
	TEST_CHECK(slotAccess(&s) != 0);
	TEST_CHECK(slotOccupied(&s) == 0);
}

void test_is_map_slot(void) {
	struct s_info s = { .element_type = MAP_ELEMENT };
	TEST_CHECK(is_map_slot(&s) == 1);

	s.element_type = STORAGE_ELEMENT;
	TEST_CHECK(is_map_slot(&s) == 0);

	s.element_type = DATA_TRANSFER;
	TEST_CHECK(is_map_slot(&s) == 0);
}

/* ================================================================
 * Tests: move_cart
 * ================================================================ */

void test_move_cart_storage_to_storage(void) {
	struct m_info media = { .last_location = 0 };
	strcpy(media.barcode, "E01001L8");

	struct s_info src = {
		.slot_location = 2000,
		.element_type  = STORAGE_ELEMENT,
		.status        = STATUS_Full | STATUS_Access,
		.media         = &media,
	};
	struct s_info dest = {
		.slot_location = 2010,
		.element_type  = STORAGE_ELEMENT,
		.status        = STATUS_Access,
		.media         = NULL,
	};

	move_cart(&src, &dest);

	/* Dest should have the media */
	TEST_CHECK(dest.media == &media);
	TEST_CHECK(slotOccupied(&dest) != 0);
	TEST_CHECK(dest.last_location == 2000);
	TEST_CHECK(media.last_location == 2000);

	/* Src should be empty */
	TEST_CHECK(src.media == NULL);
	TEST_CHECK(slotOccupied(&src) == 0);
	TEST_CHECK(slotAccess(&src) != 0);
	TEST_CHECK(src.last_location == 0);
}

void test_move_cart_to_map_sets_impexp(void) {
	struct m_info media = { .last_location = 0 };
	strcpy(media.barcode, "CLN101L8");

	struct s_info src = {
		.slot_location = 2005,
		.element_type  = STORAGE_ELEMENT,
		.status        = STATUS_Full,
		.media         = &media,
	};
	struct s_info dest = {
		.slot_location = 1000,
		.element_type  = MAP_ELEMENT,
		.status        = 0,
		.media         = NULL,
	};

	move_cart(&src, &dest);

	/* MAP destination: ImpExp should be cleared (ROBOT_ARM = 0) */
	TEST_CHECK((dest.status & STATUS_ImpExp) == 0);
	/* But slot should be full */
	TEST_CHECK(slotOccupied(&dest) != 0);
}

void test_move_cart_to_storage_no_impexp(void) {
	struct m_info media = { .last_location = 0 };
	strcpy(media.barcode, "E01002L8");

	struct s_info src = {
		.slot_location = 2001,
		.element_type  = STORAGE_ELEMENT,
		.status        = STATUS_Full,
		.media         = &media,
	};
	struct s_info dest = {
		.slot_location = 2020,
		.element_type  = STORAGE_ELEMENT,
		.status        = STATUS_ImpExp, /* pre-set — should NOT be changed */
		.media         = NULL,
	};

	move_cart(&src, &dest);

	/* Non-MAP dest: ImpExp should be left as-is (not explicitly cleared) */
	TEST_CHECK(dest.status & STATUS_ImpExp);
}

TEST_LIST = {
	/* slot_type */
	{ "slot_type_drive",     test_slot_type_drive },
	{ "slot_type_picker",    test_slot_type_picker },
	{ "slot_type_map",       test_slot_type_map },
	{ "slot_type_storage",   test_slot_type_storage },
	{ "slot_type_invalid",   test_slot_type_invalid },
	{ "slot_type_boundary",  test_slot_type_boundary },
	/* slot_number */
	{ "slot_number_drive",   test_slot_number_drive },
	{ "slot_number_storage", test_slot_number_storage },
	{ "slot_number_map",     test_slot_number_map },
	{ "slot_number_picker",  test_slot_number_picker },
	{ "slot_number_unknown", test_slot_number_unknown_type },
	/* Status bits */
	{ "slot_access",         test_slot_access },
	{ "slot_occupied",       test_slot_occupied },
	{ "imp_exp_status",      test_imp_exp_status },
	{ "status_bits_independent", test_status_bits_independent },
	{ "is_map_slot",         test_is_map_slot },
	/* move_cart */
	{ "move_cart_storage_to_storage",  test_move_cart_storage_to_storage },
	{ "move_cart_to_map_impexp",       test_move_cart_to_map_sets_impexp },
	{ "move_cart_to_storage_no_impexp", test_move_cart_to_storage_no_impexp },
	{ NULL, NULL }
};
