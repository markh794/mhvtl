/*
 * Unit tests for CRC functions:
 *   - ccan/ccan/crc32c/crc32c.c  (CRC-32C / iSCSI)
 *   - usr/utils/reed-solomon.c   (Reed-Solomon LBP CRC per ECMA-319)
 */
#include "acutest.h"

#include <string.h>
#include <stdint.h>

/* CRC-32C: use the project's usr/utils/crc32c.c (Mark Adler's
 * standalone implementation), not the ccan version which has
 * unvendored dependencies. Linked via Makefile.
 */
extern uint32_t crc32c(uint32_t crc, void const *buf, size_t len);

/* Reed-Solomon: linked via Makefile. */
extern uint32_t GenerateRSCRC(uint32_t crc, uint32_t cnt, const void *start);
extern uint32_t BlockProtectRSCRC(uint8_t *blkbuf, uint32_t blklen, int32_t bigendian);
extern uint32_t BlockVerifyRSCRC(const uint8_t *blkbuf, uint32_t blklen, int32_t bigendian);

/* ================================================================
 * CRC-32C tests — vectors from RFC 3720 Appendix B.4
 * ================================================================ */

/*
 * NOTE: This crc32c implementation (Mark Adler's usr/utils/crc32c.c)
 * returns the CRC in the host's native byte order. On little-endian
 * x86 this is byte-swapped relative to the "canonical" RFC 3720
 * notation. The expected values below match the actual output.
 */
void test_crc32c_zeros(void) {
	uint8_t data[32];
	memset(data, 0, sizeof(data));
	uint32_t crc = crc32c(0, data, sizeof(data));
	TEST_CHECK(crc == 0x8A9136AA);
	TEST_MSG("expected 0x8A9136AA, got 0x%08X", crc);
}

void test_crc32c_ones(void) {
	uint8_t data[32];
	memset(data, 0xFF, sizeof(data));
	uint32_t crc = crc32c(0, data, sizeof(data));
	TEST_CHECK(crc == 0x62A8AB43);
	TEST_MSG("expected 0x62A8AB43, got 0x%08X", crc);
}

void test_crc32c_incrementing(void) {
	uint8_t data[32];
	for (int i = 0; i < 32; i++)
		data[i] = i;
	uint32_t crc = crc32c(0, data, sizeof(data));
	TEST_CHECK(crc == 0x46DD794E);
	TEST_MSG("expected 0x46DD794E, got 0x%08X", crc);
}

void test_crc32c_decrementing(void) {
	uint8_t data[32];
	for (int i = 0; i < 32; i++)
		data[i] = 31 - i;
	uint32_t crc = crc32c(0, data, sizeof(data));
	TEST_CHECK(crc == 0x113FDB5C);
	TEST_MSG("expected 0x113FDB5C, got 0x%08X", crc);
}

void test_crc32c_iscsi_read(void) {
	uint8_t data[48] = {
		0x01, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00,
		0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x18,
		0x28, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	};
	uint32_t crc = crc32c(0, data, sizeof(data));
	TEST_CHECK(crc == 0xD9963A56);
	TEST_MSG("expected 0xD9963A56, got 0x%08X", crc);
}

void test_crc32c_empty(void) {
	uint32_t crc = crc32c(0, NULL, 0);
	TEST_CHECK(crc == 0);
	TEST_MSG("CRC of empty data should be 0 (seed), got 0x%08X", crc);
}

void test_crc32c_single_byte(void) {
	uint8_t data = 0x61; /* 'a' */
	uint32_t crc = crc32c(0, &data, 1);
	/* Verify it's deterministic */
	uint32_t crc2 = crc32c(0, &data, 1);
	TEST_CHECK(crc == crc2);
	TEST_CHECK(crc != 0); /* should produce a non-zero CRC */
}

void test_crc32c_incremental(void) {
	/* CRC computed in two chunks must match single-pass CRC */
	uint8_t data[16];
	for (int i = 0; i < 16; i++)
		data[i] = i * 7;

	uint32_t full = crc32c(0, data, 16);
	uint32_t partial = crc32c(0, data, 8);
	partial = crc32c(partial, data + 8, 8);
	TEST_CHECK(full == partial);
	TEST_MSG("incremental: 0x%08X != single: 0x%08X", partial, full);
}

/* ================================================================
 * Reed-Solomon LBP CRC tests
 * ================================================================ */

void test_rs_generate_deterministic(void) {
	uint8_t data[] = {0x01, 0x02, 0x03, 0x04, 0x05};
	uint32_t crc1 = GenerateRSCRC(0, sizeof(data), data);
	uint32_t crc2 = GenerateRSCRC(0, sizeof(data), data);
	TEST_CHECK(crc1 == crc2);
	TEST_CHECK(crc1 != 0);
}

void test_rs_generate_zeros(void) {
	uint8_t data[8] = {0};
	uint32_t crc = GenerateRSCRC(0, sizeof(data), data);
	/* CRC of all zeros with zero seed should be zero (per the polynomial). */
	TEST_CHECK(crc == 0);
}

void test_rs_generate_different_data(void) {
	uint8_t data1[] = {0x01, 0x02, 0x03, 0x04};
	uint8_t data2[] = {0x04, 0x03, 0x02, 0x01};
	uint32_t crc1 = GenerateRSCRC(0, sizeof(data1), data1);
	uint32_t crc2 = GenerateRSCRC(0, sizeof(data2), data2);
	TEST_CHECK(crc1 != crc2);
	TEST_MSG("different data should produce different CRCs");
}

void test_rs_protect_verify_round_trip_be(void) {
	/* Protect block in big-endian, then verify — should succeed. */
	uint8_t buf[128];
	memset(buf, 0, sizeof(buf));
	for (int i = 0; i < 64; i++)
		buf[i] = (uint8_t)(i * 3 + 7);

	uint32_t protected_len = BlockProtectRSCRC(buf, 64, 1);
	TEST_CHECK(protected_len == 68);
	TEST_MSG("expected 68, got %u", protected_len);

	uint32_t verified_len = BlockVerifyRSCRC(buf, 68, 1);
	TEST_CHECK(verified_len == 64);
	TEST_MSG("verify failed: expected 64, got %u", verified_len);
}

void test_rs_protect_verify_round_trip_le(void) {
	/* Same but little-endian CRC order. */
	uint8_t buf[128];
	memset(buf, 0, sizeof(buf));
	for (int i = 0; i < 64; i++)
		buf[i] = (uint8_t)(i * 5 + 13);

	uint32_t protected_len = BlockProtectRSCRC(buf, 64, 0);
	TEST_CHECK(protected_len == 68);

	uint32_t verified_len = BlockVerifyRSCRC(buf, 68, 0);
	TEST_CHECK(verified_len == 64);
	TEST_MSG("LE verify failed: expected 64, got %u", verified_len);
}

void test_rs_verify_corrupt_data(void) {
	/* Protect, flip a bit, verify should fail. */
	uint8_t buf[128];
	memset(buf, 0, sizeof(buf));
	for (int i = 0; i < 32; i++)
		buf[i] = (uint8_t)(i + 1);

	BlockProtectRSCRC(buf, 32, 1);

	buf[10] ^= 0x01; /* corrupt one bit */

	uint32_t verified_len = BlockVerifyRSCRC(buf, 36, 1);
	TEST_CHECK(verified_len == 0);
	TEST_MSG("corrupted block should fail verification");
}

void test_rs_verify_corrupt_crc(void) {
	/* Protect, corrupt the CRC bytes, verify should fail. */
	uint8_t buf[128];
	memset(buf, 0, sizeof(buf));
	for (int i = 0; i < 16; i++)
		buf[i] = 0xAA;

	BlockProtectRSCRC(buf, 16, 1);
	buf[18] ^= 0xFF; /* flip CRC byte */

	uint32_t verified_len = BlockVerifyRSCRC(buf, 20, 1);
	TEST_CHECK(verified_len == 0);
}

void test_rs_protect_zero_length(void) {
	uint8_t buf[8] = {0};
	uint32_t len = BlockProtectRSCRC(buf, 0, 1);
	TEST_CHECK(len == 0);
	TEST_MSG("zero-length block should return 0");
}

void test_rs_verify_too_small(void) {
	uint8_t buf[4] = {0x01, 0x02, 0x03, 0x04};
	/* Block of 4 bytes = all CRC, no data — should fail. */
	uint32_t len = BlockVerifyRSCRC(buf, 4, 1);
	TEST_CHECK(len == 0);

	/* Block of 3 — too small even for CRC. */
	len = BlockVerifyRSCRC(buf, 3, 1);
	TEST_CHECK(len == 0);
}

TEST_LIST = {
	/* CRC-32C */
	{ "crc32c_zeros",         test_crc32c_zeros },
	{ "crc32c_ones",          test_crc32c_ones },
	{ "crc32c_incrementing",  test_crc32c_incrementing },
	{ "crc32c_decrementing",  test_crc32c_decrementing },
	{ "crc32c_iscsi_read",    test_crc32c_iscsi_read },
	{ "crc32c_empty",         test_crc32c_empty },
	{ "crc32c_single_byte",   test_crc32c_single_byte },
	{ "crc32c_incremental",   test_crc32c_incremental },
	/* Reed-Solomon */
	{ "rs_generate_deterministic",     test_rs_generate_deterministic },
	{ "rs_generate_zeros",             test_rs_generate_zeros },
	{ "rs_generate_different_data",    test_rs_generate_different_data },
	{ "rs_protect_verify_be",          test_rs_protect_verify_round_trip_be },
	{ "rs_protect_verify_le",          test_rs_protect_verify_round_trip_le },
	{ "rs_verify_corrupt_data",        test_rs_verify_corrupt_data },
	{ "rs_verify_corrupt_crc",         test_rs_verify_corrupt_crc },
	{ "rs_protect_zero_length",        test_rs_protect_zero_length },
	{ "rs_verify_too_small",           test_rs_verify_too_small },
	{ NULL, NULL }
};
