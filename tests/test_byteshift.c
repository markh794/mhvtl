/*
 * Unit tests for include/utils/be_byteshift.h
 *
 * Tests big-endian byte serialization/deserialization for all widths:
 * 16, 24, 32, 48, 64 bit.
 */
#include "acutest.h"
#include "be_byteshift.h"

/* ---- put + get round-trip tests ---- */

void test_be16_round_trip(void) {
	uint8_t buf[2] = {0};

	put_unaligned_be16(0x0000, buf);
	TEST_CHECK(get_unaligned_be16(buf) == 0x0000);

	put_unaligned_be16(0xABCD, buf);
	TEST_CHECK(get_unaligned_be16(buf) == 0xABCD);

	put_unaligned_be16(0xFFFF, buf);
	TEST_CHECK(get_unaligned_be16(buf) == 0xFFFF);

	put_unaligned_be16(0x0102, buf);
	TEST_CHECK(buf[0] == 0x01);
	TEST_CHECK(buf[1] == 0x02);
}

void test_be24_round_trip(void) {
	uint8_t buf[3] = {0};

	put_unaligned_be24(0x000000, buf);
	TEST_CHECK(get_unaligned_be24(buf) == 0x000000);

	put_unaligned_be24(0x123456, buf);
	TEST_CHECK(get_unaligned_be24(buf) == 0x123456);
	TEST_CHECK(buf[0] == 0x12);
	TEST_CHECK(buf[1] == 0x34);
	TEST_CHECK(buf[2] == 0x56);

	put_unaligned_be24(0xFFFFFF, buf);
	TEST_CHECK(get_unaligned_be24(buf) == 0xFFFFFF);
}

void test_be32_round_trip(void) {
	uint8_t buf[4] = {0};

	put_unaligned_be32(0x00000000, buf);
	TEST_CHECK(get_unaligned_be32(buf) == 0x00000000);

	put_unaligned_be32(0xDEADBEEF, buf);
	TEST_CHECK(get_unaligned_be32(buf) == 0xDEADBEEF);
	TEST_CHECK(buf[0] == 0xDE);
	TEST_CHECK(buf[1] == 0xAD);
	TEST_CHECK(buf[2] == 0xBE);
	TEST_CHECK(buf[3] == 0xEF);

	put_unaligned_be32(0xFFFFFFFF, buf);
	TEST_CHECK(get_unaligned_be32(buf) == 0xFFFFFFFF);

	put_unaligned_be32(0x00000001, buf);
	TEST_CHECK(get_unaligned_be32(buf) == 0x00000001);
	TEST_CHECK(buf[0] == 0x00);
	TEST_CHECK(buf[3] == 0x01);
}

void test_be48_round_trip(void) {
	uint8_t buf[8] = {0};

	/* put_unaligned_be48() writes bits [47:16] as a big endian 32 bit
	 * value at buf[0..3] and bits [15:0] at buf[4..5];
	 * get_unaligned_be48() reads them back the same way. Every 48 bit
	 * value therefore round-trips exactly.
	 */
	put_unaligned_be48(0x000000000000ULL, buf);
	TEST_CHECK(get_unaligned_be48(buf) == 0x000000000000ULL);

	/* Bits [31:16] must survive: they were dropped by an earlier
	 * implementation that shifted by 32 instead of 16.
	 */
	put_unaligned_be48(0x0000AABB00DDULL, buf);
	TEST_CHECK(get_unaligned_be48(buf) == 0x0000AABB00DDULL);

	put_unaligned_be48(0xFFFFFFFFFFFFULL, buf);
	TEST_CHECK(get_unaligned_be48(buf) == 0xFFFFFFFFFFFFULL);

	put_unaligned_be48(0x123456789ABCULL, buf);
	TEST_CHECK(get_unaligned_be48(buf) == 0x123456789ABCULL);

	/* ... and land in the byte order SCSI expects, most significant
	 * byte first.
	 */
	put_unaligned_be48(0x123456789ABCULL, buf);
	TEST_CHECK(buf[0] == 0x12 && buf[1] == 0x34 && buf[2] == 0x56 &&
			   buf[3] == 0x78 && buf[4] == 0x9A && buf[5] == 0xBC);
}

void test_be64_round_trip(void) {
	uint8_t buf[8] = {0};

	put_unaligned_be64(0x0000000000000000ULL, buf);
	TEST_CHECK(get_unaligned_be64(buf) == 0x0000000000000000ULL);

	put_unaligned_be64(0x0102030405060708ULL, buf);
	TEST_CHECK(get_unaligned_be64(buf) == 0x0102030405060708ULL);
	TEST_CHECK(buf[0] == 0x01);
	TEST_CHECK(buf[7] == 0x08);

	put_unaligned_be64(0xFFFFFFFFFFFFFFFFULL, buf);
	TEST_CHECK(get_unaligned_be64(buf) == 0xFFFFFFFFFFFFFFFFULL);
}

/* ---- byte-order verification (always big-endian) ---- */

void test_be32_byte_order(void) {
	uint8_t buf[4];

	put_unaligned_be32(0x01020304, buf);
	TEST_CHECK(buf[0] == 0x01);
	TEST_MSG("MSB should be 0x01, got 0x%02x", buf[0]);
	TEST_CHECK(buf[1] == 0x02);
	TEST_CHECK(buf[2] == 0x03);
	TEST_CHECK(buf[3] == 0x04);
	TEST_MSG("LSB should be 0x04, got 0x%02x", buf[3]);
}

void test_be64_byte_order(void) {
	uint8_t buf[8];

	put_unaligned_be64(0x0807060504030201ULL, buf);
	TEST_CHECK(buf[0] == 0x08);
	TEST_CHECK(buf[7] == 0x01);
}

/* ---- single-bit edge cases ---- */

void test_be16_single_bits(void) {
	uint8_t buf[2];
	for (int bit = 0; bit < 16; bit++) {
		uint16_t val = 1u << bit;
		put_unaligned_be16(val, buf);
		TEST_CHECK(get_unaligned_be16(buf) == val);
		TEST_MSG("bit %d: expected 0x%04x, got 0x%04x",
				 bit, val, get_unaligned_be16(buf));
	}
}

void test_be32_single_bits(void) {
	uint8_t buf[4];
	for (int bit = 0; bit < 32; bit++) {
		uint32_t val = 1u << bit;
		put_unaligned_be32(val, buf);
		TEST_CHECK(get_unaligned_be32(buf) == val);
	}
}

void test_be64_single_bits(void) {
	uint8_t buf[8];
	for (int bit = 0; bit < 64; bit++) {
		uint64_t val = 1ULL << bit;
		put_unaligned_be64(val, buf);
		TEST_CHECK(get_unaligned_be64(buf) == val);
	}
}

/* ---- unaligned access simulation ---- */

void test_be32_offset_in_buffer(void) {
	uint8_t buf[16] = {0};

	/* Write at offset 3 to test unaligned access */
	put_unaligned_be32(0xCAFEBABE, &buf[3]);
	TEST_CHECK(get_unaligned_be32(&buf[3]) == 0xCAFEBABE);
	/* Verify surrounding bytes untouched */
	TEST_CHECK(buf[2] == 0x00);
	TEST_CHECK(buf[7] == 0x00);
}

TEST_LIST = {
	{ "be16_round_trip",       test_be16_round_trip },
	{ "be24_round_trip",       test_be24_round_trip },
	{ "be32_round_trip",       test_be32_round_trip },
	{ "be48_round_trip",       test_be48_round_trip },
	{ "be64_round_trip",       test_be64_round_trip },
	{ "be32_byte_order",       test_be32_byte_order },
	{ "be64_byte_order",       test_be64_byte_order },
	{ "be16_single_bits",      test_be16_single_bits },
	{ "be32_single_bits",      test_be32_single_bits },
	{ "be64_single_bits",      test_be64_single_bits },
	{ "be32_offset_in_buffer", test_be32_offset_in_buffer },
	{ NULL, NULL }
};
