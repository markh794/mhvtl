/*
 * test_compression — LZ4 / ZSTD block-codec round-trip.
 *
 * Drives the REAL codec path (usr/mhvtl_compress.c, the same compress+fallback
 * helper writeBlock_lz4 / writeBlock_zstd call) and stores the result through an
 * actual storage backend, then reads it back and verifies it is bit-identical.
 *
 * For BOTH codecs (LZ4, ZSTD) and BOTH backends it asserts:
 *   (a) a COMPRESSIBLE payload is stored compressed (comp_size < uncompressed)
 *       and reads back bit-identical;
 *   (b) an INCOMPRESSIBLE payload falls back to comp_type=none (helper returns
 *       0, "store raw") and reads back bit-identical.
 *
 * Backends:
 *   - file backend  : a regular file's pread/pwrite byte channel (always run;
 *                     no root required).
 *   - raw backend   : cart_backend_raw against a formatted block device, when
 *                     built with -DRAW_BACKEND_AVAILABLE and given the device
 *                     path as argv[1] (driven by tests/test_raw_loopback.sh).
 *
 * Usage: ./test_compression [block-device]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include "vtlcart.h"		/* LZ4, ZSTD comp_type values */
#include "mhvtl_compress.h"

#ifdef RAW_BACKEND_AVAILABLE
#include "cart_backend_raw.h"
/* libvtlscsi expects each executable to supply this symbol. */
char mhvtl_driver_name[] = "test_compression";
#endif

#define PAYLOAD_SZ 8192

static int fails;
#define CHECK(cond, msg) do { \
	if (cond) { printf("  ok   : %s\n", msg); } \
	else { printf("  FAIL : %s (errno=%s)\n", msg, strerror(errno)); fails++; } \
} while (0)

static const char *codec_name(uint8_t c)
{
	return (c == LZ4) ? "LZ4" : (c == ZSTD) ? "ZSTD" : "?";
}

static uint32_t codec_bound(uint8_t c, uint32_t n)
{
	return (c == LZ4) ? mhvtl_lz4_compress_bound(n)
			  : mhvtl_zstd_compress_bound(n);
}

/* A store callback writes `len` bytes to a backend and reads them back into
 * `out` (capacity >= len). Returns 0 on success, -1 on error. */
typedef int (*store_fn)(void *ctx, const uint8_t *in, uint32_t len, uint8_t *out);

/*
 * One codec round-trip through a backend.
 *   expect_compressible != 0 : assert comp_size < uncompressed (stored compressed)
 *   expect_compressible == 0 : assert fallback to comp_type=none (helper == 0)
 */
static void roundtrip(uint8_t codec, const uint8_t *payload, uint32_t n,
		      int expect_compressible, int level,
		      const char *backend, store_fn store, void *ctx)
{
	char	 msg[128];
	uint32_t cap	 = codec_bound(codec, n);
	uint8_t *dst	 = malloc(cap);
	uint8_t *stored_back = malloc(n + cap);
	uint8_t *out	 = malloc(n);
	int	 comp_len;

	if (!dst || !stored_back || !out) {
		CHECK(0, "malloc");
		goto done;
	}

	comp_len = mhvtl_compress_block(codec, payload, n, dst, cap, level);

	if (expect_compressible) {
		snprintf(msg, sizeof(msg),
			 "%s/%s compressible: stored compressed (comp=%d < uncomp=%u)",
			 backend, codec_name(codec), comp_len, n);
		CHECK(comp_len > 0 && (uint32_t)comp_len < n, msg);
		if (comp_len <= 0)
			goto done;

		/* Store the compressed bytes through the backend, read back,
		 * decompress, and verify bit-identical to the original. */
		if (store(ctx, dst, (uint32_t)comp_len, stored_back) != 0) {
			snprintf(msg, sizeof(msg), "%s/%s store compressed bytes",
				 backend, codec_name(codec));
			CHECK(0, msg);
			goto done;
		}
		int d = mhvtl_decompress_block(codec, stored_back,
					       (uint32_t)comp_len, out, n, n);
		snprintf(msg, sizeof(msg),
			 "%s/%s compressible: reads back bit-identical",
			 backend, codec_name(codec));
		CHECK(d == (int)n && memcmp(out, payload, n) == 0, msg);
	} else {
		snprintf(msg, sizeof(msg),
			 "%s/%s incompressible: comp_type=none fallback (helper=%d)",
			 backend, codec_name(codec), comp_len);
		CHECK(comp_len == 0, msg);

		/* Fallback path stores the raw block (comp_type none). Store the
		 * uncompressed payload through the backend and verify readback. */
		if (store(ctx, payload, n, stored_back) != 0) {
			snprintf(msg, sizeof(msg), "%s/%s store raw bytes",
				 backend, codec_name(codec));
			CHECK(0, msg);
			goto done;
		}
		snprintf(msg, sizeof(msg),
			 "%s/%s incompressible: reads back bit-identical",
			 backend, codec_name(codec));
		CHECK(memcmp(stored_back, payload, n) == 0, msg);
	}

done:
	free(dst);
	free(stored_back);
	free(out);
}

/* ---- file backend: a regular file's pread/pwrite byte channel ---- */

static int store_file(void *ctx, const uint8_t *in, uint32_t len, uint8_t *out)
{
	int fd = *(int *)ctx;

	if (pwrite(fd, in, len, 0) != (ssize_t)len)
		return -1;
	fsync(fd);
	if (pread(fd, out, len, 0) != (ssize_t)len)
		return -1;
	return 0;
}

#ifdef RAW_BACKEND_AVAILABLE
/* ---- raw backend: cart_backend_raw on a formatted block device ---- */

static int store_raw(void *ctx, const uint8_t *in, uint32_t len, uint8_t *out)
{
	(void)ctx;
	if (cart_backend_raw.pwrite(CART_STREAM_DATA, 0, in, len, 0) != (ssize_t)len)
		return -1;
	cart_backend_raw.sync(CART_STREAM_DATA, 0);
	if (cart_backend_raw.pread(CART_STREAM_DATA, 0, out, len, 0) != (ssize_t)len)
		return -1;
	return 0;
}
#endif

static void run_backend(const char *backend, store_fn store, void *ctx,
			const uint8_t *compressible, const uint8_t *random,
			uint32_t n)
{
	/* ZSTD level 3 (balanced); LZ4 ignores the level. */
	const int level = 3;
	uint8_t codecs[2] = { LZ4, ZSTD };

	for (int i = 0; i < 2; i++) {
		roundtrip(codecs[i], compressible, n, 1, level, backend, store, ctx);
		roundtrip(codecs[i], random, n, 0, level, backend, store, ctx);
	}
}

int main(int argc, char **argv)
{
	uint8_t *compressible = malloc(PAYLOAD_SZ);
	uint8_t *random	      = malloc(PAYLOAD_SZ);

	if (!compressible || !random) {
		fprintf(stderr, "malloc failed\n");
		return 2;
	}

	/* Compressible: low-entropy repeating pattern. */
	for (uint32_t i = 0; i < PAYLOAD_SZ; i++)
		compressible[i] = (uint8_t)('A' + (i % 16 == 0 ? 1 : 0));

	/* Incompressible: random bytes from the kernel CSPRNG. */
	{
		int fd = open("/dev/urandom", O_RDONLY);
		ssize_t got = (fd >= 0) ? read(fd, random, PAYLOAD_SZ) : -1;
		if (fd >= 0)
			close(fd);
		if (got != PAYLOAD_SZ) {
			/* Fallback: still high-entropy enough to not shrink. */
			srand(12345);
			for (uint32_t i = 0; i < PAYLOAD_SZ; i++)
				random[i] = (uint8_t)rand();
		}
	}

	printf("test_compression: PAYLOAD_SZ=%d\n", PAYLOAD_SZ);

	/* File backend (always). */
	{
		char tmpl[] = "/tmp/mhvtl-comp-XXXXXX";
		int fd = mkstemp(tmpl);
		if (fd < 0) {
			CHECK(0, "mkstemp for file backend");
		} else {
			unlink(tmpl);
			run_backend("file", store_file, &fd,
				    compressible, random, PAYLOAD_SZ);
			close(fd);
		}
	}

#ifdef RAW_BACKEND_AVAILABLE
	/* Raw block-device backend (when a device path is supplied). */
	if (argc >= 2) {
		if (raw_open(argv[1]) != 0) {
			CHECK(0, "raw_open");
		} else {
			run_backend("raw", store_raw, NULL,
				    compressible, random, PAYLOAD_SZ);
			raw_close();
		}
	} else {
		printf("  (raw backend skipped: no device argument)\n");
	}
#else
	(void)argc;
	(void)argv;
#endif

	free(compressible);
	free(random);

	printf("%s: %d check(s) failed\n", fails ? "FAILED" : "SUCCESS", fails);
	return fails ? 1 : 0;
}
