/*
 * bench_compression — ratio + processing-speed comparison across ALL block
 * codecs mhVTL supports (none / LZO / ZLIB / LZ4 / ZSTD), on a synthetic
 * compressible stream and a synthetic uncompressible (random) stream.
 *
 * Each codec is driven through its own library exactly as the tape write path
 * does (lzo1x_1_compress, zlib compress2, LZ4_compress_default, ZSTD_compress),
 * so the numbers reflect real per-block codec cost. For every codec it reports:
 *   - ratio          uncompressed / compressed (1.00 = no shrink)
 *   - comp MB/s      compression throughput over the uncompressed size
 *   - decomp MB/s    decompression throughput (restore path)
 * and verifies each codec round-trips bit-identically before timing.
 *
 * Throughput is over the UNCOMPRESSED size (the MB/s a backup stream sees);
 * numbers are single-threaded, CPU-bound, on an in-cache block, so they isolate
 * codec cost rather than disk I/O. "none" is a memcpy (the raw-store ceiling).
 *
 * Note: mhVTL's LZ4/ZSTD write path adds a store-raw fallback (a block that
 * does not shrink is stored uncompressed, ratio capped at 1.00); LZO/ZLIB have
 * no such fallback and can slightly expand incompressible data. This benchmark
 * reports each codec's raw achieved ratio so the fallback's value is visible.
 *
 * Usage: bench_compression [total_MiB] [block_KiB] [zstd_level]
 *   defaults: 256 MiB, 256 KiB blocks, zstd level 1 (also runs zstd level 3).
 *
 * Copyright (C) 2025 mhVTL modernization — GPL v2 or later.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <lzo/lzo1x.h>
#include <zlib.h>
#include <lz4.h>
#include <zstd.h>

enum codec { C_NONE, C_LZO, C_ZLIB, C_LZ4, C_ZSTD };

/* Fast, deterministic PRNG (xorshift64) for the uncompressible stream. */
static uint64_t rng_state = 0x123456789abcdef0ULL;
static uint64_t xorshift64(void)
{
	uint64_t x = rng_state;
	x ^= x << 13; x ^= x >> 7; x ^= x << 17;
	rng_state = x;
	return x;
}

static void fill_random(uint8_t *buf, size_t len)
{
	size_t i = 0;
	while (i + 8 <= len) {
		uint64_t r = xorshift64();
		memcpy(buf + i, &r, 8);
		i += 8;
	}
	while (i < len)
		buf[i++] = (uint8_t)xorshift64();
}

/*
 * Realistically-compressible data: a fixed 1 KiB pseudo-random template tiled
 * across the block (periodic => compressible), with ~1/12 of the bytes
 * overwritten by fresh noise (breaks matches => a believable ~2-3x ratio, the
 * range real backup data tends to land in).
 */
static void fill_compressible(uint8_t *buf, size_t len)
{
	uint8_t tmpl[1024];
	size_t i;

	fill_random(tmpl, sizeof(tmpl));
	for (i = 0; i < len; i++)
		buf[i] = tmpl[i % sizeof(tmpl)];
	for (i = 0; i < len; i++)
		if ((xorshift64() % 12) == 0)
			buf[i] = (uint8_t)xorshift64();
}

static double now_sec(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* LZO scratch memory (fixed size, single-threaded — same as the tape daemon). */
static lzo_align_t lzo_wrkmem[((LZO1X_1_MEM_COMPRESS) + (sizeof(lzo_align_t) - 1)) /
			      sizeof(lzo_align_t)];

/* Persistent ZSTD contexts, reused per block — mirrors the tape write path
 * (usr/mhvtl_compress.c) so the benchmark reflects real mhVTL ZSTD throughput. */
static ZSTD_CCtx *zstd_cctx;
static ZSTD_DCtx *zstd_dctx;

/* Compress one block. Returns compressed length, or 0 on codec error. */
static uint32_t compress_one(enum codec c, int level, const uint8_t *src,
			     uint32_t blk, uint8_t *dst, uint32_t dst_cap)
{
	switch (c) {
	case C_NONE:
		memcpy(dst, src, blk);
		return blk;
	case C_LZO: {
		lzo_uint n = dst_cap;
		if (lzo1x_1_compress(src, blk, dst, &n, lzo_wrkmem) != LZO_E_OK)
			return 0;
		return (uint32_t)n;
	}
	case C_ZLIB: {
		uLongf n = dst_cap;
		if (compress2(dst, &n, src, blk, level) != Z_OK)
			return 0;
		return (uint32_t)n;
	}
	case C_LZ4: {
		int n = LZ4_compress_default((const char *)src, (char *)dst,
					     (int)blk, (int)dst_cap);
		return n > 0 ? (uint32_t)n : 0;
	}
	case C_ZSTD: {
		size_t n = ZSTD_compressCCtx(zstd_cctx, dst, dst_cap, src, blk,
					     level);
		return ZSTD_isError(n) ? 0 : (uint32_t)n;
	}
	}
	return 0;
}

/* Decompress one block. Returns decompressed length, or 0 on error. */
static uint32_t decompress_one(enum codec c, const uint8_t *src, uint32_t comp,
			       uint8_t *dst, uint32_t dst_cap)
{
	switch (c) {
	case C_NONE:
		memcpy(dst, src, comp);
		return comp;
	case C_LZO: {
		lzo_uint n = dst_cap;
		if (lzo1x_decompress_safe(src, comp, dst, &n, NULL) != LZO_E_OK)
			return 0;
		return (uint32_t)n;
	}
	case C_ZLIB: {
		uLongf n = dst_cap;
		if (uncompress(dst, &n, src, comp) != Z_OK)
			return 0;
		return (uint32_t)n;
	}
	case C_LZ4: {
		int n = LZ4_decompress_safe((const char *)src, (char *)dst,
					    (int)comp, (int)dst_cap);
		return n > 0 ? (uint32_t)n : 0;
	}
	case C_ZSTD: {
		size_t n = ZSTD_decompressDCtx(zstd_dctx, dst, dst_cap, src,
					       comp);
		return ZSTD_isError(n) ? 0 : (uint32_t)n;
	}
	}
	return 0;
}

static void row(const char *label, enum codec c, int level, const uint8_t *src,
		uint32_t blk, uint32_t blocks, uint8_t *dst, uint32_t dst_cap,
		uint8_t *chk)
{
	double t0, t1, unc_mb;
	double comp_mbps, decomp_mbps, ratio;
	uint32_t comp_len, i;

	/* One compression up front for the ratio + a round-trip correctness
	 * check, so a broken codec is caught before its numbers are reported. */
	comp_len = compress_one(c, level, src, blk, dst, dst_cap);
	if (!comp_len) {
		printf("    %-14s %10s %10s %8s\n", label, "ERR", "ERR", "-");
		return;
	}
	if (decompress_one(c, dst, comp_len, chk, blk) != blk ||
	    memcmp(chk, src, blk) != 0) {
		printf("    %-14s %10s %10s %8s\n", label, "BADRT", "BADRT", "-");
		return;
	}

	unc_mb = (double)blk * blocks / (1024.0 * 1024.0);

	t0 = now_sec();
	for (i = 0; i < blocks; i++)
		compress_one(c, level, src, blk, dst, dst_cap);
	t1 = now_sec();
	comp_mbps = unc_mb / (t1 - t0 > 0 ? t1 - t0 : 1e-9);

	t0 = now_sec();
	for (i = 0; i < blocks; i++)
		decompress_one(c, dst, comp_len, chk, blk);
	t1 = now_sec();
	decomp_mbps = unc_mb / (t1 - t0 > 0 ? t1 - t0 : 1e-9);

	ratio = (double)blk / (double)(comp_len ? comp_len : 1);
	printf("    %-14s %10.0f %10.0f %8.2f\n", label, comp_mbps, decomp_mbps,
	       ratio);
}

static void report(const char *stream, const uint8_t *src, uint32_t blk,
		   uint32_t blocks, uint8_t *dst, uint32_t dst_cap,
		   uint8_t *chk, int zlevel)
{
	char zl1[16], zl3[16];

	snprintf(zl1, sizeof(zl1), "ZSTD lvl%d", zlevel);
	snprintf(zl3, sizeof(zl3), "ZSTD lvl%d", 3);

	printf("  %-14s %10s %10s %8s\n", stream, "comp MB/s", "dec MB/s", "ratio");
	row("none (raw)", C_NONE, 0, src, blk, blocks, dst, dst_cap, chk);
	row("LZO",        C_LZO,  0, src, blk, blocks, dst, dst_cap, chk);
	row("ZLIB lvl6",  C_ZLIB, 6, src, blk, blocks, dst, dst_cap, chk);
	row("LZ4",        C_LZ4,  0, src, blk, blocks, dst, dst_cap, chk);
	row(zl1,          C_ZSTD, zlevel, src, blk, blocks, dst, dst_cap, chk);
	row(zl3,          C_ZSTD, 3, src, blk, blocks, dst, dst_cap, chk);
}

int main(int argc, char **argv)
{
	uint32_t total_mib = (argc > 1) ? (uint32_t)strtoul(argv[1], NULL, 10) : 256;
	uint32_t blk_kib   = (argc > 2) ? (uint32_t)strtoul(argv[2], NULL, 10) : 256;
	int	 zlevel    = (argc > 3) ? atoi(argv[3]) : 1;
	uint32_t blk       = blk_kib * 1024;
	uint32_t blocks    = (uint32_t)((uint64_t)total_mib * 1024 * 1024 / blk);
	uint8_t *comp_buf, *rand_buf, *dst, *chk;
	uint32_t dst_cap;

	if (lzo_init() != LZO_E_OK) {
		fprintf(stderr, "lzo_init failed\n");
		return 1;
	}
	zstd_cctx = ZSTD_createCCtx();
	zstd_dctx = ZSTD_createDCtx();
	if (!zstd_cctx || !zstd_dctx) {
		fprintf(stderr, "ZSTD context alloc failed\n");
		return 1;
	}
	if (!blocks)
		blocks = 1;

	/* Worst-case destination across all codecs (ZLIB/LZO can expand). */
	dst_cap = (uint32_t)compressBound(blk);
	if ((uint32_t)LZ4_compressBound((int)blk) > dst_cap)
		dst_cap = (uint32_t)LZ4_compressBound((int)blk);
	if ((uint32_t)ZSTD_compressBound(blk) > dst_cap)
		dst_cap = (uint32_t)ZSTD_compressBound(blk);
	dst_cap += blk;		/* headroom for LZO worst case */

	comp_buf = malloc(blk);
	rand_buf = malloc(blk);
	dst	 = malloc(dst_cap);
	chk	 = malloc(blk);
	if (!comp_buf || !rand_buf || !dst || !chk) {
		fprintf(stderr, "out of memory\n");
		return 1;
	}
	fill_compressible(comp_buf, blk);
	fill_random(rand_buf, blk);

	printf("mhVTL compression benchmark: %u MiB, %u KiB blocks (%u blocks)\n",
	       total_mib, blk_kib, blocks);
	report("compressible", comp_buf, blk, blocks, dst, dst_cap, chk, zlevel);
	report("uncompressible", rand_buf, blk, blocks, dst, dst_cap, chk, zlevel);

	free(comp_buf);
	free(rand_buf);
	free(dst);
	free(chk);
	ZSTD_freeCCtx(zstd_cctx);
	ZSTD_freeDCtx(zstd_dctx);
	return 0;
}
