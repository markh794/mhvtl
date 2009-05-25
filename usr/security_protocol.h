
/*
 * Security Protocol IN/OUT
 *
 * Based on spc4r14 / ssc3r04a
 *
 */

/*
 * Security Algorithm Code
 */
#define HMAC_KDF_SHA1		0x0020002
#define HMAC_KDF_SHA256		0x0020005
#define HMAC_KDF_SHA384		0x0020006
#define HMAC_KDF_SHA512		0x0020007
#define KDF_AES_128_XCBC	0x0020004


/*
 * Security Association
 *	spc4r14 5.13.2
 */
struct sa {
	uint32_t ac_sai;
	uint32_t ds_sai;
	uint32_t timeout;

	uint64_t ac_sqn;
	uint64_t ds_sqn;

	uint8_t ac_nonce[64];
	uint8_t ds_nonce[64];

	uint8_t key_seed[64];

	uint32_t kdf_id;

	uint8_t keymat[1024];

	uint16_t usage_type;
	uint8_t usage_data[1024];
	uint8_t mgmt_data[1024];
};


