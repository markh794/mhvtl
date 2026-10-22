/*
 * print_raw_header() must report encryption metadata without disclosing the
 * logical block encryption key or modifying the stored header.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "acutest.h"
#include "vtlcart.h"
#include "vtllib.h"

char mhvtl_driver_name[] = "test_dump_tape_output";

static void test_encryption_key_is_not_dumped(void) {
	struct blk_header saved = *c_pos;
	struct blk_header expected;
	char			  output[1024];
	char			  key_hex[ENCR_KEY_MAX_LEN * 2 + 1];
	FILE			 *capture;
	int				  saved_stdout;
	size_t			  output_len;

	memset(c_pos, 0, sizeof(*c_pos));
	c_pos->blk_type						   = B_DATA;
	c_pos->blk_flags					   = BLKHDR_FLG_ENCRYPTED;
	c_pos->blk_encryption_info.key_length  = ENCR_KEY_MAX_LEN;
	c_pos->blk_encryption_info.ukad_length = 2;
	c_pos->blk_encryption_info.akad_length = 2;
	for (size_t i = 0; i < ENCR_KEY_MAX_LEN; i++) {
		c_pos->blk_encryption_info.key[i] = (uint8_t)(i + 1);
		snprintf(&key_hex[i * 2], sizeof(key_hex) - (i * 2), "%02x", (unsigned)(i + 1));
	}
	c_pos->blk_encryption_info.ukad[0] = 0xaa;
	c_pos->blk_encryption_info.ukad[1] = 0xbb;
	c_pos->blk_encryption_info.akad[0] = 0xcc;
	c_pos->blk_encryption_info.akad[1] = 0xdd;
	expected						   = *c_pos;

	capture = tmpfile();
	TEST_ASSERT(capture != NULL);
	saved_stdout = dup(fileno(stdout));
	TEST_ASSERT(saved_stdout >= 0);
	fflush(stdout);
	TEST_ASSERT(dup2(fileno(capture), fileno(stdout)) >= 0);

	print_raw_header();

	fflush(stdout);
	TEST_ASSERT(dup2(saved_stdout, fileno(stdout)) >= 0);
	close(saved_stdout);
	TEST_ASSERT(fseek(capture, 0, SEEK_SET) == 0);
	output_len		   = fread(output, 1, sizeof(output) - 1, capture);
	output[output_len] = '\0';
	fclose(capture);

	TEST_CHECK(memcmp(c_pos, &expected, sizeof(*c_pos)) == 0);
	TEST_CHECK(strstr(output, "         Key : (not shown)\n") != NULL);
	TEST_CHECK(strstr(output, "aabb") != NULL);
	TEST_CHECK(strstr(output, "ccdd") != NULL);
	TEST_CHECK(strstr(output, key_hex) == NULL);

	*c_pos = saved;
}

/* A corrupt U-KAD length must not read past the 32-byte ukad[] array */
static void test_corrupt_kad_length_is_clamped(void) {
	struct blk_header saved = *c_pos;
	char			  output[1024];
	FILE			 *capture;
	int				  saved_stdout;
	size_t			  output_len;

	memset(c_pos, 0, sizeof(*c_pos));
	c_pos->blk_type						   = B_DATA;
	c_pos->blk_flags					   = BLKHDR_FLG_ENCRYPTED;
	c_pos->blk_encryption_info.ukad_length = 0xffffffff;
	memset(c_pos->blk_encryption_info.ukad, 0x11, ENCR_KEY_MAX_LEN);
	memset(c_pos->blk_encryption_info.akad, 0x99, ENCR_KEY_MAX_LEN);

	capture = tmpfile();
	TEST_ASSERT(capture != NULL);
	saved_stdout = dup(fileno(stdout));
	TEST_ASSERT(saved_stdout >= 0);
	fflush(stdout);
	TEST_ASSERT(dup2(fileno(capture), fileno(stdout)) >= 0);

	print_raw_header();

	fflush(stdout);
	TEST_ASSERT(dup2(saved_stdout, fileno(stdout)) >= 0);
	close(saved_stdout);
	TEST_ASSERT(fseek(capture, 0, SEEK_SET) == 0);
	output_len		   = fread(output, 1, sizeof(output) - 1, capture);
	output[output_len] = '\0';
	fclose(capture);

	TEST_CHECK(strstr(output, "1111111111111111111111111111111111111111111111111111111111111111\n") != NULL);
	TEST_CHECK(strstr(output, "99") == NULL);

	*c_pos = saved;
}

TEST_LIST = {
	{"encryption key is not dumped", test_encryption_key_is_not_dumped},
	{"corrupt KAD length is clamped", test_corrupt_kad_length_is_clamped},
	{NULL, NULL},
};
