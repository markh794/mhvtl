/*
 * AES-256-GCM encryption of tape blocks, see include/mhvtl_crypto.h
 *
 * Copyright (C) 2026 Ciro Iriarte
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <string.h>
#include "mhvtl_crypto.h"

static void put_be32(uint32_t v, uint8_t *p) {
	p[0] = v >> 24;
	p[1] = v >> 16;
	p[2] = v >> 8;
	p[3] = v;
}

/* IV: the 32-bit fixed field, then the 64-bit cartridge counter, big-endian */
void gcm_nonce(const uint8_t fixed[GCM_FIXED_LEN], uint64_t counter,
			   uint8_t nonce[GCM_NONCE_LEN]) {
	memcpy(nonce, fixed, GCM_FIXED_LEN);
	put_be32(counter >> 32, nonce + 4);
	put_be32((uint32_t)counter, nonce + 8);
}

/*
 * The header fields the tag covers, in a fixed big-endian layout: a layout
 * version, then block type, flags (compression included), partition, block
 * number, sizes, plain-text CRC, KAD lengths, the A-KAD and the key check
 * value. Changing any of them fails authentication. On reading, the position
 * is the one the block is read from (read_header()), so a moved block fails.
 * The U-KAD is unauthenticated by definition (SSC), so only its length is in.
 */
size_t gcm_aad(const struct blk_header *h, uint8_t aad[GCM_AAD_MAX]) {
	const struct encryption *e = &h->blk_encryption_info;
	uint32_t				 akad_len = e->akad_length;
	size_t					 n		  = 0;

	if (akad_len > ENCR_KEY_MAX_LEN)
		akad_len = ENCR_KEY_MAX_LEN;

	aad[n++] = 1;
	put_be32(h->blk_type, aad + n);
	n += 4;
	put_be32(h->blk_flags, aad + n);
	n += 4;
	put_be32(h->partition_id, aad + n);
	n += 4;
	put_be32(h->blk_number, aad + n);
	n += 4;
	put_be32(h->blk_size, aad + n);
	n += 4;
	put_be32(h->disk_blk_size, aad + n);
	n += 4;
	put_be32(h->uncomp_crc, aad + n);
	n += 4;
	put_be32(e->ukad_length, aad + n);
	n += 4;
	put_be32(e->akad_length, aad + n);
	n += 4;
	memcpy(aad + n, e->akad, akad_len);
	n += akad_len;
	memcpy(aad + n, e->key + GCM_KCV_OFF, GCM_KCV_LEN);
	n += GCM_KCV_LEN;
	return n;
}

#ifdef MHVTL_HAVE_OPENSSL

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/kdf.h>
#include <openssl/rand.h>

static const char hkdf_info[] = "mhvtl-gcm-v1";
static const char kcv_label[] = "mhvtl-kcv-v1";

int mhvtl_crypto_available(void) {
	return 1;
}

int gcm_random(uint8_t *buf, size_t len) {
	return RAND_bytes(buf, (int)len) == 1 ? 0 : -1;
}

void gcm_wipe(void *p, size_t len) {
	OPENSSL_cleanse(p, len);
}

/* HKDF-SHA256(key, salt, "mhvtl-gcm-v1"), 64 bytes: the GCM key, then the
 * key check value key */
int gcm_derive_keys(const uint8_t key[GCM_KEY_LEN],
					const uint8_t salt[CART_SALT_LEN], struct gcm_keys *k) {
	uint8_t		  okm[2 * GCM_KEY_LEN];
	size_t		  okm_len = sizeof(okm);
	EVP_PKEY_CTX *pctx;
	int			  ok = 0;

	gcm_free_keys(k);

	pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, NULL);
	if (pctx &&
		EVP_PKEY_derive_init(pctx) > 0 &&
		EVP_PKEY_CTX_set_hkdf_md(pctx, EVP_sha256()) > 0 &&
		EVP_PKEY_CTX_set1_hkdf_salt(pctx, salt, CART_SALT_LEN) > 0 &&
		EVP_PKEY_CTX_set1_hkdf_key(pctx, key, GCM_KEY_LEN) > 0 &&
		EVP_PKEY_CTX_add1_hkdf_info(pctx, (const unsigned char *)hkdf_info,
									sizeof(hkdf_info) - 1) > 0 &&
		EVP_PKEY_derive(pctx, okm, &okm_len) > 0 &&
		okm_len == sizeof(okm))
		ok = 1;
	EVP_PKEY_CTX_free(pctx);
	if (ok) {
		memcpy(k->enc, okm, GCM_KEY_LEN);
		memcpy(k->kcv, okm + GCM_KEY_LEN, GCM_KEY_LEN);
		k->ctx = EVP_CIPHER_CTX_new();
		if (!k->ctx ||
			EVP_CipherInit_ex(k->ctx, EVP_aes_256_gcm(), NULL, NULL, NULL, 1) != 1 ||
			EVP_CIPHER_CTX_ctrl(k->ctx, EVP_CTRL_GCM_SET_IVLEN, GCM_NONCE_LEN, NULL) != 1 ||
			EVP_CipherInit_ex(k->ctx, NULL, NULL, k->enc, NULL, 1) != 1)
			ok = 0;
	}
	OPENSSL_cleanse(okm, sizeof(okm));
	if (!ok) {
		gcm_free_keys(k);
		return -1;
	}
	return 0;
}

void gcm_free_keys(struct gcm_keys *k) {
	EVP_CIPHER_CTX_free(k->ctx);
	OPENSSL_cleanse(k, sizeof(*k));
}

/* First 4 bytes of HMAC-SHA256(kcv key, "mhvtl-kcv-v1" || nonce) */
int gcm_kcv(const struct gcm_keys *k, const uint8_t nonce[GCM_NONCE_LEN],
			uint8_t kcv[GCM_KCV_LEN]) {
	uint8_t		 msg[sizeof(kcv_label) - 1 + GCM_NONCE_LEN];
	uint8_t		 mac[EVP_MAX_MD_SIZE];
	unsigned int mac_len = 0;

	memcpy(msg, kcv_label, sizeof(kcv_label) - 1);
	memcpy(msg + sizeof(kcv_label) - 1, nonce, GCM_NONCE_LEN);
	if (!HMAC(EVP_sha256(), k->kcv, GCM_KEY_LEN, msg, sizeof(msg), mac, &mac_len) ||
		mac_len < GCM_KCV_LEN)
		return -1;
	memcpy(kcv, mac, GCM_KCV_LEN);
	OPENSSL_cleanse(mac, sizeof(mac));
	return 0;
}

/* Does the key check value in the header match these keys? A match is only a
 * hint for choosing the sense code; the tag decides. */
int gcm_kcv_matches(const struct gcm_keys *k, const struct blk_header *h) {
	const uint8_t *key = h->blk_encryption_info.key;
	uint8_t		   kcv[GCM_KCV_LEN];

	if (gcm_kcv(k, key + GCM_NONCE_OFF, kcv))
		return 0;
	return CRYPTO_memcmp(kcv, key + GCM_KCV_OFF, GCM_KCV_LEN) == 0;
}

static int gcm_run(struct gcm_keys *k, const struct blk_header *h, int enc,
				   const uint8_t *in, uint8_t *out, size_t len, uint8_t *tag) {
	const uint8_t *nonce = h->blk_encryption_info.key + GCM_NONCE_OFF;
	uint8_t		   aad[GCM_AAD_MAX];
	size_t		   aad_len = gcm_aad(h, aad);
	int			   n;

	if (len > 0x7fffffff)
		return -1;
	/* the key schedule set up by gcm_derive_keys() is kept */
	if (EVP_CipherInit_ex(k->ctx, NULL, NULL, NULL, nonce, enc) != 1 ||
		EVP_CipherUpdate(k->ctx, NULL, &n, aad, (int)aad_len) != 1 ||
		(len && EVP_CipherUpdate(k->ctx, out, &n, in, (int)len) != 1))
		return -1;
	if (!enc && EVP_CIPHER_CTX_ctrl(k->ctx, EVP_CTRL_GCM_SET_TAG, GCM_TAG_LEN, tag) != 1)
		return -1;
	if (EVP_CipherFinal_ex(k->ctx, out + len, &n) != 1)
		return -1;
	if (enc && EVP_CIPHER_CTX_ctrl(k->ctx, EVP_CTRL_GCM_GET_TAG, GCM_TAG_LEN, tag) != 1)
		return -1;
	return 0;
}

/*
 * Encrypt 'len' bytes of 'in' into 'out'. The nonce must already be in the
 * header; this sets the key check value and the tag.
 */
int gcm_seal(struct gcm_keys *k, struct blk_header *h, const uint8_t *in,
			 uint8_t *out, size_t len) {
	uint8_t *key = h->blk_encryption_info.key;

	if (!k->ctx || gcm_kcv(k, key + GCM_NONCE_OFF, key + GCM_KCV_OFF))
		return -1;
	return gcm_run(k, h, 1, in, out, len, key + GCM_TAG_OFF);
}

/* Verify and decrypt 'len' bytes in place. 0 if authentic, -1 if not. */
int gcm_open(struct gcm_keys *k, const struct blk_header *h, uint8_t *buf,
			 size_t len) {
	uint8_t tag[GCM_TAG_LEN];

	if (!k->ctx)
		return -1;
	memcpy(tag, h->blk_encryption_info.key + GCM_TAG_OFF, GCM_TAG_LEN);
	return gcm_run(k, h, 0, buf, buf, len, tag);
}

#else /* !MHVTL_HAVE_OPENSSL */

int mhvtl_crypto_available(void) {
	return 0;
}

int gcm_random(uint8_t *buf, size_t len) {
	(void)buf;
	(void)len;
	return -1;
}

void gcm_wipe(void *p, size_t len) {
	volatile uint8_t *v = p;

	while (len--)
		*v++ = 0;
}

int gcm_derive_keys(const uint8_t key[GCM_KEY_LEN],
					const uint8_t salt[CART_SALT_LEN], struct gcm_keys *k) {
	(void)key;
	(void)salt;
	gcm_wipe(k, sizeof(*k));
	return -1;
}

void gcm_free_keys(struct gcm_keys *k) {
	gcm_wipe(k, sizeof(*k));
}

int gcm_kcv(const struct gcm_keys *k, const uint8_t nonce[GCM_NONCE_LEN],
			uint8_t kcv[GCM_KCV_LEN]) {
	(void)k;
	(void)nonce;
	(void)kcv;
	return -1;
}

int gcm_kcv_matches(const struct gcm_keys *k, const struct blk_header *h) {
	(void)k;
	(void)h;
	return 0;
}

int gcm_seal(struct gcm_keys *k, struct blk_header *h, const uint8_t *in,
			 uint8_t *out, size_t len) {
	(void)k;
	(void)h;
	(void)in;
	(void)out;
	(void)len;
	return -1;
}

int gcm_open(struct gcm_keys *k, const struct blk_header *h, uint8_t *buf,
			 size_t len) {
	(void)k;
	(void)h;
	(void)buf;
	(void)len;
	return -1;
}

#endif /* MHVTL_HAVE_OPENSSL */
