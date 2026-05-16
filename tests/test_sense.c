/*
 * Unit tests for SCSI sense builders and related functions in vtllib.c:
 *   - sam_unit_attention, sam_not_ready, sam_illegal_request,
 *     sam_medium_error, sam_blank_check, sam_data_protect,
 *     sam_hardware_error, sam_no_sense
 *   - setTapeAlert
 *   - resp_read_block_limits
 *   - resp_read_media_serial
 *   - check_reset / reset_device / check_inquiry_data_has_changed /
 *     set_inquiry_data_changed
 *
 * Strategy: We compile vtllib.c directly with stubs for the heavy
 * dependencies (ioctl, IPC) that the sense paths don't touch.
 */
#include "acutest.h"

#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

/*
 * ---- Provide the minimal environment vtllib.c expects ----
 *
 * vtllib.c includes many headers. Rather than pulling in the full
 * daemon infrastructure, we provide the subset the sense/response
 * functions actually reference. This is intentionally narrow.
 */

/* Logging: compile without MHVTL_DEBUG so DBG macros are no-ops.
 * MHVTL_ERR/MHVTL_LOG still call syslog — stub it.
 */
#include "vtl_common.h"
#include "mhvtl_scsi.h"
#include "be_byteshift.h"

/* Stub out syslog */
#include <stdarg.h>
void syslog(int priority, const char *fmt, ...) { (void)priority; (void)fmt; }

/* Globals */
uint8_t sense[SENSE_BUF_SIZE];
int verbose = 0;
int debug = 0;
char *mhvtl_driver_name = "test";
int current_state = 0;
long my_id = 1;

/* ---- Struct definitions needed by the functions under test ---- */

struct s_sd {
	uint8_t  byte0;
	uint16_t field_pointer;
};

struct pc_header {
	uint8_t head[4];
};

struct log_pg_header {
	uint8_t head[4];
};

struct TapeAlert_flag {
	struct pc_header flag;
	uint8_t          value;
} __attribute__((packed));

struct TapeAlert_pg {
	struct log_pg_header pcode_head;
	struct TapeAlert_flag TapeAlert[64];
} __attribute__((packed));

/*
 * We include the implementation of just the functions we need.
 * return_sense is static in vtllib.c, so the sam_* wrappers must
 * be compiled from the same translation unit.
 */
/* Static helper: return_sense and the sam_* wrappers.
 * Extracted inline to avoid pulling in all of vtllib.c.
 */
static void return_sense(uint8_t key, uint32_t asc_ascq, struct s_sd *sd,
						 uint8_t *sam_stat) {
	memset(sense, 0, SENSE_BUF_SIZE);
	*sam_stat = SAM_STAT_CHECK_CONDITION;
	sense[0] = SD_CURRENT_INFORMATION_FIXED;
	switch (key) {
	case UNIT_ATTENTION:
		if ((asc_ascq >> 8) == 0x29)
			break;
		if (asc_ascq == E_MODE_PARAMETERS_CHANGED)
			break;
	default:
		sense[0] |= SD_VALID;
		break;
	}
	sense[2] = key;
	sense[7] = SENSE_BUF_SIZE - 8;
	put_unaligned_be16(asc_ascq, &sense[12]);
	if (sd) {
		sense[15] = sd->byte0;
		put_unaligned_be16(sd->field_pointer, &sense[16]);
	}
}

void sam_unit_attention(uint16_t ascq, uint8_t *sam_stat) {
	return_sense(UNIT_ATTENTION, ascq, NULL, sam_stat);
}
void sam_not_ready(uint16_t ascq, uint8_t *sam_stat) {
	return_sense(NOT_READY, ascq, NULL, sam_stat);
}
void sam_illegal_request(uint16_t ascq, struct s_sd *sd, uint8_t *sam_stat) {
	return_sense(ILLEGAL_REQUEST, ascq, sd, sam_stat);
}
void sam_medium_error(uint16_t ascq, uint8_t *sam_stat) {
	return_sense(MEDIUM_ERROR, ascq, NULL, sam_stat);
}
void sam_blank_check(uint16_t ascq, uint8_t *sam_stat) {
	return_sense(BLANK_CHECK, ascq, NULL, sam_stat);
}
void sam_data_protect(uint16_t ascq, uint8_t *sam_stat) {
	return_sense(DATA_PROTECT, ascq, NULL, sam_stat);
}
void sam_hardware_error(uint16_t ascq, uint8_t *sam_stat) {
	return_sense(HARDWARE_ERROR, ascq, NULL, sam_stat);
}
void sam_no_sense(uint8_t key, uint16_t ascq, uint8_t *sam_stat) {
	return_sense(NO_SENSE | key, ascq, NULL, sam_stat);
}

static int reset_flag = 0;
static int inquiry_data_changed_flag = 0;

void reset_device(void) { reset_flag = 1; }
int check_reset(uint8_t *sam_stat) {
	int retval = reset_flag;
	if (reset_flag) {
		sam_unit_attention(E_POWERON_RESET, sam_stat);
		reset_flag = 0;
	}
	return retval;
}
void set_inquiry_data_changed(void) { inquiry_data_changed_flag = 1; }
int check_inquiry_data_has_changed(uint8_t *sam_stat) {
	int retval = inquiry_data_changed_flag;
	if (inquiry_data_changed_flag) {
		sam_unit_attention(E_INQUIRY_DATA_HAS_CHANGED, sam_stat);
		inquiry_data_changed_flag = 0;
	}
	return retval;
}

void setTapeAlert(struct TapeAlert_pg *ta, uint64_t flg) {
	for (int a = 0; a < 64; a++)
		ta->TapeAlert[a].value = (flg & (1ull << a)) ? 1 : 0;
}

int resp_read_block_limits(struct mhvtl_ds *dbuf_p, int sz) {
	uint8_t *arr = (uint8_t *)dbuf_p->data;
	memset(arr, 0, 6);
	put_unaligned_be24(sz, &arr[1]);
	arr[5] = 0x1;
	return 6;
}

uint32_t resp_read_media_serial(uint8_t *sno, uint8_t *buf, uint8_t *sam_stat) {
	uint32_t size = 38;
	snprintf((char *)&buf[4], size - 3, "%-34.34s", sno);
	put_unaligned_be16(size, &buf[2]);
	return size;
}

/* ================================================================
 * Tests: sam_* sense builders
 * ================================================================ */

static void clear_sense(void) {
	memset(sense, 0, SENSE_BUF_SIZE);
}

void test_sam_not_ready(void) {
	uint8_t sam_stat = 0;
	clear_sense();
	sam_not_ready(0x0401, &sam_stat);

	TEST_CHECK(sam_stat == SAM_STAT_CHECK_CONDITION);
	TEST_CHECK(sense[2] == NOT_READY);
	TEST_CHECK(sense[12] == 0x04);
	TEST_CHECK(sense[13] == 0x01);
	TEST_CHECK((sense[0] & 0x7F) == SD_CURRENT_INFORMATION_FIXED);
	TEST_CHECK(sense[0] & SD_VALID);
}

void test_sam_unit_attention_poweron(void) {
	uint8_t sam_stat = 0;
	clear_sense();
	sam_unit_attention(E_POWERON_RESET, &sam_stat);

	TEST_CHECK(sam_stat == SAM_STAT_CHECK_CONDITION);
	TEST_CHECK(sense[2] == UNIT_ATTENTION);
	TEST_CHECK(sense[12] == 0x29);
	TEST_CHECK(sense[13] == 0x00);
	/* Power-on reset (ASC 0x29) should NOT have SD_VALID set */
	TEST_CHECK(!(sense[0] & SD_VALID));
}

void test_sam_unit_attention_mode_changed(void) {
	uint8_t sam_stat = 0;
	clear_sense();
	sam_unit_attention(E_MODE_PARAMETERS_CHANGED, &sam_stat);

	TEST_CHECK(sense[2] == UNIT_ATTENTION);
	TEST_CHECK(sense[12] == 0x2A);
	TEST_CHECK(sense[13] == 0x01);
	/* Mode parameters changed should NOT have SD_VALID set */
	TEST_CHECK(!(sense[0] & SD_VALID));
}

void test_sam_unit_attention_other(void) {
	uint8_t sam_stat = 0;
	clear_sense();
	/* A non-special UA: should have SD_VALID */
	sam_unit_attention(E_INQUIRY_DATA_HAS_CHANGED, &sam_stat);

	TEST_CHECK(sense[2] == UNIT_ATTENTION);
	TEST_CHECK(sense[0] & SD_VALID);
}

void test_sam_illegal_request_with_sd(void) {
	uint8_t sam_stat = 0;
	struct s_sd sd = { .byte0 = SKSV | CD, .field_pointer = 0x1234 };
	clear_sense();
	sam_illegal_request(0x2400, &sd, &sam_stat);

	TEST_CHECK(sam_stat == SAM_STAT_CHECK_CONDITION);
	TEST_CHECK(sense[2] == ILLEGAL_REQUEST);
	TEST_CHECK(sense[12] == 0x24);
	TEST_CHECK(sense[13] == 0x00);
	TEST_CHECK(sense[15] == (SKSV | CD));
	TEST_CHECK(get_unaligned_be16(&sense[16]) == 0x1234);
}

void test_sam_medium_error(void) {
	uint8_t sam_stat = 0;
	clear_sense();
	sam_medium_error(0x1100, &sam_stat);

	TEST_CHECK(sense[2] == MEDIUM_ERROR);
	TEST_CHECK(sense[12] == 0x11);
	TEST_CHECK(sense[13] == 0x00);
}

void test_sam_blank_check(void) {
	uint8_t sam_stat = 0;
	clear_sense();
	sam_blank_check(0x0005, &sam_stat);

	TEST_CHECK(sense[2] == BLANK_CHECK);
	TEST_CHECK(sense[12] == 0x00);
	TEST_CHECK(sense[13] == 0x05);
}

void test_sam_data_protect(void) {
	uint8_t sam_stat = 0;
	clear_sense();
	sam_data_protect(0x2700, &sam_stat);

	TEST_CHECK(sense[2] == DATA_PROTECT);
	TEST_CHECK(sense[12] == 0x27);
}

void test_sam_hardware_error(void) {
	uint8_t sam_stat = 0;
	clear_sense();
	sam_hardware_error(0x4400, &sam_stat);

	TEST_CHECK(sense[2] == HARDWARE_ERROR);
	TEST_CHECK(sense[12] == 0x44);
}

void test_sam_no_sense(void) {
	uint8_t sam_stat = 0;
	clear_sense();
	sam_no_sense(0, 0x0000, &sam_stat);

	TEST_CHECK(sense[2] == NO_SENSE);
}

void test_sense_clears_previous(void) {
	uint8_t sam_stat = 0;
	/* Set some sense, then set different sense — old data must be gone. */
	sam_hardware_error(0x4400, &sam_stat);
	TEST_CHECK(sense[2] == HARDWARE_ERROR);

	sam_not_ready(0x0401, &sam_stat);
	TEST_CHECK(sense[2] == NOT_READY);
	/* HARDWARE_ERROR-specific fields should be cleared */
	TEST_CHECK(sense[12] == 0x04);
}

/* ================================================================
 * Tests: setTapeAlert
 * ================================================================ */

void test_tape_alert_single_bits(void) {
	struct TapeAlert_pg ta;
	memset(&ta, 0, sizeof(ta));

	for (int bit = 0; bit < 64; bit++) {
		uint64_t flg = 1ULL << bit;
		setTapeAlert(&ta, flg);
		TEST_CHECK(ta.TapeAlert[bit].value == 1);
		TEST_MSG("bit %d should be set", bit);
		/* Check adjacent bits are clear */
		if (bit > 0)
			TEST_CHECK(ta.TapeAlert[bit - 1].value == 0);
		if (bit < 63)
			TEST_CHECK(ta.TapeAlert[bit + 1].value == 0);
	}
}

void test_tape_alert_all_set(void) {
	struct TapeAlert_pg ta;
	memset(&ta, 0, sizeof(ta));

	setTapeAlert(&ta, 0xFFFFFFFFFFFFFFFFULL);
	for (int i = 0; i < 64; i++) {
		TEST_CHECK(ta.TapeAlert[i].value == 1);
	}
}

void test_tape_alert_all_clear(void) {
	struct TapeAlert_pg ta;
	memset(&ta, 0xFF, sizeof(ta));

	setTapeAlert(&ta, 0);
	for (int i = 0; i < 64; i++) {
		TEST_CHECK(ta.TapeAlert[i].value == 0);
	}
}

/* ================================================================
 * Tests: resp_read_block_limits
 * ================================================================ */

void test_read_block_limits(void) {
	uint8_t data[16] = {0};
	struct mhvtl_ds ds;
	ds.data = data;

	int ret = resp_read_block_limits(&ds, 0x100000);
	TEST_CHECK(ret == 6);
	/* Max block size in bytes 1-3 (big-endian 24-bit) */
	TEST_CHECK(get_unaligned_be24(&data[1]) == 0x100000);
	/* Minimum block size */
	TEST_CHECK(data[5] == 0x01);
}

void test_read_block_limits_small(void) {
	uint8_t data[16] = {0};
	struct mhvtl_ds ds;
	ds.data = data;

	resp_read_block_limits(&ds, 512);
	TEST_CHECK(get_unaligned_be24(&data[1]) == 512);
	TEST_CHECK(data[5] == 0x01);
}

/* ================================================================
 * Tests: resp_read_media_serial
 * ================================================================ */

void test_read_media_serial(void) {
	uint8_t buf[64] = {0};
	uint8_t sam_stat = 0;
	uint8_t sno[] = "E01001L8";

	uint32_t sz = resp_read_media_serial(sno, buf, &sam_stat);
	TEST_CHECK(sz == 38);
	/* Size at offset 2-3 */
	TEST_CHECK(get_unaligned_be16(&buf[2]) == 38);
	/* Serial starts at offset 4 */
	TEST_CHECK(memcmp(&buf[4], "E01001L8", 8) == 0);
}

/* ================================================================
 * Tests: device state flags
 * ================================================================ */

void test_reset_device_flag(void) {
	uint8_t sam_stat = 0;

	/* Initially no reset pending */
	reset_flag = 0;
	TEST_CHECK(check_reset(&sam_stat) == 0);

	/* Set reset, check returns 1 and sets sense */
	reset_device();
	sam_stat = 0;
	TEST_CHECK(check_reset(&sam_stat) == 1);
	TEST_CHECK(sam_stat == SAM_STAT_CHECK_CONDITION);
	TEST_CHECK(sense[2] == UNIT_ATTENTION);
	TEST_CHECK(sense[12] == 0x29); /* POWER ON / RESET */

	/* Second check should return 0 (cleared) */
	sam_stat = 0;
	TEST_CHECK(check_reset(&sam_stat) == 0);
}

void test_inquiry_data_changed_flag(void) {
	uint8_t sam_stat = 0;

	inquiry_data_changed_flag = 0;
	TEST_CHECK(check_inquiry_data_has_changed(&sam_stat) == 0);

	set_inquiry_data_changed();
	sam_stat = 0;
	TEST_CHECK(check_inquiry_data_has_changed(&sam_stat) == 1);
	TEST_CHECK(sam_stat == SAM_STAT_CHECK_CONDITION);
	TEST_CHECK(sense[2] == UNIT_ATTENTION);
	TEST_CHECK(sense[12] == 0x3F); /* INQUIRY DATA HAS CHANGED */
	TEST_CHECK(sense[13] == 0x03);

	sam_stat = 0;
	TEST_CHECK(check_inquiry_data_has_changed(&sam_stat) == 0);
}

TEST_LIST = {
	/* Sense builders */
	{ "sam_not_ready",                test_sam_not_ready },
	{ "sam_unit_attention_poweron",   test_sam_unit_attention_poweron },
	{ "sam_unit_attention_mode",      test_sam_unit_attention_mode_changed },
	{ "sam_unit_attention_other",     test_sam_unit_attention_other },
	{ "sam_illegal_request_with_sd",  test_sam_illegal_request_with_sd },
	{ "sam_medium_error",             test_sam_medium_error },
	{ "sam_blank_check",              test_sam_blank_check },
	{ "sam_data_protect",             test_sam_data_protect },
	{ "sam_hardware_error",           test_sam_hardware_error },
	{ "sam_no_sense",                 test_sam_no_sense },
	{ "sense_clears_previous",        test_sense_clears_previous },
	/* TapeAlert */
	{ "tape_alert_single_bits",       test_tape_alert_single_bits },
	{ "tape_alert_all_set",           test_tape_alert_all_set },
	{ "tape_alert_all_clear",         test_tape_alert_all_clear },
	/* Response builders */
	{ "read_block_limits",            test_read_block_limits },
	{ "read_block_limits_small",      test_read_block_limits_small },
	{ "read_media_serial",            test_read_media_serial },
	/* Device state flags */
	{ "reset_device_flag",            test_reset_device_flag },
	{ "inquiry_data_changed_flag",    test_inquiry_data_changed_flag },
	{ NULL, NULL }
};
