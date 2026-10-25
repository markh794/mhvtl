/*
 * AES-256-GCM encryption of tape blocks (markh794/mhvtl#304)
 *
 * Each cartridge gets its own keys: HKDF-SHA256 (RFC 5869) of the key the
 * application loaded, salted with 32 random bytes kept in the cartridge MAM.
 * Each block is sealed with a 96-bit IV built from a per-cartridge counter
 * (the deterministic construction of NIST SP 800-38D, 8.2.1). The 32-byte key
 * field of the block header holds nonce || tag || key check value; the key
 * itself is never written to the cartridge.
 *
 * Built without OpenSSL (MHVTL_HAVE_OPENSSL undefined), every function fails
 * and mhvtl_crypto_available() returns 0.
 */
#ifndef _MHVTL_CRYPTO_H_
#define _MHVTL_CRYPTO_H_

#include <stddef.h>
#include <stdint.h>
#include "vtlcart.h"

#define GCM_KEY_LEN	  32
#define GCM_NONCE_LEN 12
#define GCM_FIXED_LEN 4	/* IV fixed field, then a 64-bit counter */
#define GCM_TAG_LEN	  16
#define GCM_KCV_LEN	  4
#define CART_SALT_LEN 32

/* Where the GCM values sit in blk_encryption_info.key[] */
#define GCM_NONCE_OFF 0
#define GCM_TAG_OFF	  (GCM_NONCE_OFF + GCM_NONCE_LEN)
#define GCM_KCV_OFF	  (GCM_TAG_OFF + GCM_TAG_LEN)

/* Authenticated header fields, see gcm_aad() */
#define GCM_AAD_MAX (1 + 9 * 4 + ENCR_KEY_MAX_LEN + GCM_KCV_LEN)

/* The keys of one cartridge, derived from the application key */
struct gcm_keys {
	uint8_t enc[GCM_KEY_LEN]; /* AES-256-GCM key */
	uint8_t kcv[GCM_KEY_LEN]; /* HMAC-SHA256 key of the key check value */
	void   *ctx;			  /* EVP_CIPHER_CTX, set up with enc */
};

int	 mhvtl_crypto_available(void);
int	 gcm_random(uint8_t *buf, size_t len);
void gcm_wipe(void *p, size_t len);

int	 gcm_derive_keys(const uint8_t key[GCM_KEY_LEN],
					 const uint8_t salt[CART_SALT_LEN], struct gcm_keys *k);
void gcm_free_keys(struct gcm_keys *k);

void   gcm_nonce(const uint8_t fixed[GCM_FIXED_LEN], uint64_t counter,
				 uint8_t nonce[GCM_NONCE_LEN]);
int	   gcm_kcv(const struct gcm_keys *k, const uint8_t nonce[GCM_NONCE_LEN],
			   uint8_t kcv[GCM_KCV_LEN]);
int	   gcm_kcv_matches(const struct gcm_keys *k, const struct blk_header *h);
size_t gcm_aad(const struct blk_header *h, uint8_t aad[GCM_AAD_MAX]);

int gcm_seal(struct gcm_keys *k, struct blk_header *h, const uint8_t *in,
			 uint8_t *out, size_t len);
int gcm_open(struct gcm_keys *k, const struct blk_header *h, uint8_t *buf,
			 size_t len);

#endif /* _MHVTL_CRYPTO_H_ */
