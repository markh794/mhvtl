/*
 * Unit tests for the AES-256-GCM block encryption helpers (usr/mhvtl_crypto.c).
 *
 * The expected values come from an independent implementation, Python's
 * cryptography package (HKDF, AESGCM) and hmac/hashlib, fed the same key,
 * salt, header and plain text, with the AAD layout of gcm_aad() rebuilt there:
 *
 *   okm   = HKDF(SHA256, length=64, salt=S, info=b"mhvtl-gcm-v1").derive(K)
 *   kcv   = HMAC-SHA256(okm[32:], b"mhvtl-kcv-v1" + nonce)[:4]
 *   aad   = b"\x01" + pack(">9I", type, flags, partition, number, blk_size,
 *           disk_blk_size, crc, ukad_len, akad_len) + akad + kcv
 *   ct    = AESGCM(okm[:32]).encrypt(nonce, pt, aad)
 */
#include "acutest.h"
#include "mhvtl_crypto.h"
#include <string.h>

static const uint8_t no_fixed[GCM_FIXED_LEN]; /* the vectors use a zero fixed field */

static const uint8_t want_enc[32] = {
	0xb1, 0x89, 0xa8, 0x1b, 0x8c, 0x13, 0x55, 0x0a, 0x52, 0xb3, 0x4d, 0x29, 0xd9, 0xd0, 0x4a, 0x1a,
	0xc0, 0x7f, 0x06, 0x21, 0x14, 0x4e, 0xd0, 0x44, 0xf7, 0xf7, 0xff, 0x90, 0x59, 0x11, 0x65, 0x1f};
static const uint8_t want_kcv_key[32] = {
	0xe9, 0x68, 0x94, 0xb9, 0x9d, 0xcb, 0x33, 0xfb, 0x94, 0x51, 0xbe, 0x4c, 0xd9, 0x4d, 0x16, 0x0d,
	0x33, 0x94, 0xde, 0x52, 0xdc, 0xd5, 0x39, 0xd7, 0xe9, 0x46, 0x44, 0x2e, 0x8d, 0x0e, 0xf4, 0xef};
static const uint8_t want_nonce[12] = {
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05};
static const uint8_t want_kcv[4] = {0x65, 0x1c, 0x28, 0xf6};
static const uint8_t want_ct[37] = {
	0xf5, 0x2b, 0xf2, 0xff, 0x96, 0x4e, 0x4b, 0x36, 0xc9, 0x3d, 0xcd, 0x20, 0x93, 0x7a, 0x2b, 0x83,
	0x57, 0xf8, 0x93, 0x12, 0x2c, 0xa2, 0xd3, 0x01, 0x5e, 0xf8, 0x77, 0x20, 0xb2, 0x34, 0xce, 0xbd,
	0x83, 0x64, 0xe2, 0xc0, 0xee};
static const uint8_t want_tag[16] = {
	0x0e, 0x64, 0x45, 0x44, 0xbc, 0xac, 0xc6, 0x4f, 0xdd, 0x02, 0xa3, 0x79, 0xc3, 0x11, 0x0b, 0xc7};
static const char pt[] = "mhvtl aes-gcm known answer vector..!.";

static void setup(struct gcm_keys *k, struct blk_header *h) {
	uint8_t key[32], salt[32];

	for (int i = 0; i < 32; i++) {
		key[i]	= i;
		salt[i] = 0x40 + i;
	}
	memset(k, 0, sizeof(*k));
	TEST_ASSERT(gcm_derive_keys(key, salt, k) == 0);

	memset(h, 0, sizeof(*h));
	h->blk_type								 = B_DATA;
	h->blk_flags							 = BLKHDR_FLG_ENCRYPTED | BLKHDR_FLG_CRC | BLKHDR_FLG_LZ4_COMPRESSED | 0x40;
	h->partition_id							 = 1;
	h->blk_number							 = 7;
	h->blk_size								 = 100;
	h->disk_blk_size						 = sizeof(pt) - 1;
	h->uncomp_crc							 = 0x12345678;
	h->blk_encryption_info.ukad_length		 = 3;
	h->blk_encryption_info.akad_length		 = 5;
	memcpy(h->blk_encryption_info.akad, "AKAD!", 5);
	gcm_nonce(no_fixed, 0x0102030405ULL, h->blk_encryption_info.key + GCM_NONCE_OFF);
}

static void test_available(void) {
	TEST_CHECK(mhvtl_crypto_available());
}

static void test_derive(void) {
	struct gcm_keys	  k;
	struct blk_header h;

	setup(&k, &h);
	TEST_CHECK(!memcmp(k.enc, want_enc, 32));
	TEST_CHECK(!memcmp(k.kcv, want_kcv_key, 32));
	TEST_CHECK(!memcmp(h.blk_encryption_info.key + GCM_NONCE_OFF, want_nonce, 12));
	gcm_free_keys(&k);
}

static void test_seal_matches_vector(void) {
	struct gcm_keys	  k;
	struct blk_header h;
	uint8_t			  ct[sizeof(pt) - 1];

	setup(&k, &h);
	TEST_ASSERT(gcm_seal(&k, &h, (const uint8_t *)pt, ct, sizeof(ct)) == 0);
	TEST_CHECK(!memcmp(h.blk_encryption_info.key + GCM_KCV_OFF, want_kcv, 4));
	TEST_CHECK(!memcmp(ct, want_ct, sizeof(ct)));
	TEST_CHECK(!memcmp(h.blk_encryption_info.key + GCM_TAG_OFF, want_tag, 16));
	TEST_CHECK(gcm_kcv_matches(&k, &h));
	gcm_free_keys(&k);
}

static void test_open_round_trip(void) {
	struct gcm_keys	  k;
	struct blk_header h;
	uint8_t			  buf[sizeof(pt) - 1];

	setup(&k, &h);
	TEST_ASSERT(gcm_seal(&k, &h, (const uint8_t *)pt, buf, sizeof(buf)) == 0);
	TEST_CHECK(gcm_open(&k, &h, buf, sizeof(buf)) == 0);
	TEST_CHECK(!memcmp(buf, pt, sizeof(buf)));
	gcm_free_keys(&k);
}

/* Seal, change one thing, and expect gcm_open() to refuse it */
static int open_after(void (*tamper)(struct blk_header *, uint8_t *)) {
	struct gcm_keys	  k;
	struct blk_header h;
	uint8_t			  buf[sizeof(pt) - 1];
	int				  rc;

	setup(&k, &h);
	if (gcm_seal(&k, &h, (const uint8_t *)pt, buf, sizeof(buf)))
		return 0;
	tamper(&h, buf);
	rc = gcm_open(&k, &h, buf, sizeof(buf));
	gcm_free_keys(&k);
	return rc;
}

static void flip_data(struct blk_header *h, uint8_t *b) { (void)h; b[10] ^= 1; }
static void flip_tag(struct blk_header *h, uint8_t *b) { (void)b; h->blk_encryption_info.key[GCM_TAG_OFF] ^= 1; }
static void flip_nonce(struct blk_header *h, uint8_t *b) { (void)b; h->blk_encryption_info.key[GCM_NONCE_OFF + 11] ^= 1; }
static void flip_kcv(struct blk_header *h, uint8_t *b) { (void)b; h->blk_encryption_info.key[GCM_KCV_OFF] ^= 1; }
static void flip_flags(struct blk_header *h, uint8_t *b) { (void)b; h->blk_flags ^= BLKHDR_FLG_LZ4_COMPRESSED | BLKHDR_FLG_ZSTD_COMPRESSED; }
static void move_block(struct blk_header *h, uint8_t *b) { (void)b; h->blk_number++; }
static void move_partition(struct blk_header *h, uint8_t *b) { (void)b; h->partition_id = 0; }
static void change_size(struct blk_header *h, uint8_t *b) { (void)b; h->blk_size++; }
static void change_crc(struct blk_header *h, uint8_t *b) { (void)b; h->uncomp_crc ^= 1; }
static void change_akad(struct blk_header *h, uint8_t *b) { (void)b; h->blk_encryption_info.akad[0] ^= 1; }
static void change_akad_len(struct blk_header *h, uint8_t *b) { (void)b; h->blk_encryption_info.akad_length = 4; }
static void change_type(struct blk_header *h, uint8_t *b) { (void)b; h->blk_type = B_FILEMARK; }

static void test_tamper_detected(void) {
	TEST_CHECK_(open_after(flip_data) == -1, "ciphertext byte");
	TEST_CHECK_(open_after(flip_tag) == -1, "tag");
	TEST_CHECK_(open_after(flip_nonce) == -1, "nonce");
	TEST_CHECK_(open_after(flip_kcv) == -1, "key check value");
	TEST_CHECK_(open_after(flip_flags) == -1, "compression flags");
	TEST_CHECK_(open_after(move_block) == -1, "block number");
	TEST_CHECK_(open_after(move_partition) == -1, "partition");
	TEST_CHECK_(open_after(change_size) == -1, "block size");
	TEST_CHECK_(open_after(change_crc) == -1, "plain-text CRC");
	TEST_CHECK_(open_after(change_akad) == -1, "A-KAD");
	TEST_CHECK_(open_after(change_akad_len) == -1, "A-KAD length");
	TEST_CHECK_(open_after(change_type) == -1, "block type");
}

/* Another application key: the key check value no longer matches */
static void test_wrong_key(void) {
	struct gcm_keys	  k, other;
	struct blk_header h;
	uint8_t			  buf[sizeof(pt) - 1], key[32], salt[32];

	setup(&k, &h);
	TEST_ASSERT(gcm_seal(&k, &h, (const uint8_t *)pt, buf, sizeof(buf)) == 0);
	memset(key, 0xaa, sizeof(key));
	for (int i = 0; i < 32; i++)
		salt[i] = 0x40 + i;
	memset(&other, 0, sizeof(other));
	TEST_ASSERT(gcm_derive_keys(key, salt, &other) == 0);
	TEST_CHECK(!gcm_kcv_matches(&other, &h));
	TEST_CHECK(gcm_open(&other, &h, buf, sizeof(buf)) == -1);
	gcm_free_keys(&k);
	gcm_free_keys(&other);
}

/* The same application key on another cartridge (other salt): other keys,
 * so the key check value does not show that the two share a key */
static void test_salt_separates_cartridges(void) {
	struct gcm_keys k1, k2;
	uint8_t			key[32], s1[32], s2[32], kcv1[4], kcv2[4], nonce[12];

	memset(key, 7, sizeof(key));
	memset(s1, 1, sizeof(s1));
	memset(s2, 2, sizeof(s2));
	memset(&k1, 0, sizeof(k1));
	memset(&k2, 0, sizeof(k2));
	TEST_ASSERT(gcm_derive_keys(key, s1, &k1) == 0);
	TEST_ASSERT(gcm_derive_keys(key, s2, &k2) == 0);
	TEST_CHECK(memcmp(k1.enc, k2.enc, 32) != 0);
	gcm_nonce(no_fixed, 0, nonce);
	TEST_ASSERT(gcm_kcv(&k1, nonce, kcv1) == 0 && gcm_kcv(&k2, nonce, kcv2) == 0);
	TEST_CHECK(memcmp(kcv1, kcv2, 4) != 0);
	gcm_free_keys(&k1);
	gcm_free_keys(&k2);
}

static void test_empty_block(void) {
	struct gcm_keys	  k;
	struct blk_header h;
	uint8_t			  b[1];

	setup(&k, &h);
	h.disk_blk_size = 0;
	TEST_ASSERT(gcm_seal(&k, &h, b, b, 0) == 0);
	TEST_CHECK(gcm_open(&k, &h, b, 0) == 0);
	h.blk_encryption_info.key[GCM_TAG_OFF] ^= 1;
	TEST_CHECK(gcm_open(&k, &h, b, 0) == -1);
	gcm_free_keys(&k);
}

TEST_LIST = {
	{"available", test_available},
	{"derive", test_derive},
	{"seal_matches_vector", test_seal_matches_vector},
	{"open_round_trip", test_open_round_trip},
	{"tamper_detected", test_tamper_detected},
	{"wrong_key", test_wrong_key},
	{"salt_separates_cartridges", test_salt_separates_cartridges},
	{"empty_block", test_empty_block},
	{NULL, NULL}};
