/*
 * bench_compression — sequential write-throughput benchmark for the mhVTL
 * block codecs (none / LZ4 / ZSTD), on a synthetic compressible stream and a
 * synthetic uncompressible (random) stream.
 *
 * It drives the SAME shared codec path the tape write uses
 * (mhvtl_compress_block from usr/mhvtl_compress.c), so the numbers reflect the
 * real per-block compress cost plus the store-raw fallback decision. Throughput
 * is reported over the UNCOMPRESSED input size (MB/s a backup stream would see);
 * ratio is uncompressed/stored.
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

#include "vtlcart.h"		/* LZ4, ZSTD comp_type values */
#include "mhvtl_compress.h"

/* Fast, deterministic PRNG (xorshift64) for the uncompressible stream. */
static uint64_t rng_state = 0x123456789abcdef0ULL;
static uint64_t xorshift64(void)
{
	uint64_t x = rng_state;
	x ^= x << 13; x ^= x >> 7; x ^= x << 17;
	rng_state = x;
	return x;
}

/* Fill buf with incompressible (random) bytes. */
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
 * Fill buf with realistically-compressible data: a fixed 1 KiB pseudo-random
 * template tiled across the block (periodic => compressible), with ~1/3 of the
 * bytes overwritten by fresh random noise (breaks matches => a believable
 * ~2-3x ratio, in the range real backup data tends to land, rather than the
 * absurd ratio of repeated text or zeros).
 */
static void fill_compressible(uint8_t *buf, size_t len)
{
	uint8_t tmpl[1024];
	size_t i;

	fill_random(tmpl, sizeof(tmpl));
	for (i = 0; i < len; i++)
		buf[i] = tmpl[i % sizeof(tmpl)];
	for (i = 0; i < len; i++)
		if ((xorshift64() % 12) == 0)	/* perturb ~1/12 of bytes */
			buf[i] = (uint8_t)xorshift64();
}

static double now_sec(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

struct result { double mbps; double ratio; };

/*
 * Compress `blocks` blocks of `blk` bytes from src. comp_type 0 = store-raw
 * baseline (memcpy, the no-compression write path's cost). Returns MB/s over
 * the uncompressed size and the achieved ratio.
 */
static struct result run(uint8_t comp_type, int level, const uint8_t *src,
			 uint32_t blk, uint32_t blocks, uint8_t *dst,
			 uint32_t dst_cap)
{
	struct result r = { 0, 0 };
	uint64_t stored = 0;
	double t0, t1;
	uint32_t i;

	t0 = now_sec();
	for (i = 0; i < blocks; i++) {
		if (comp_type == 0) {
			memcpy(dst, src, blk);	/* raw store */
			stored += blk;
		} else {
			int rc = mhvtl_compress_block(comp_type, src, blk,
						      dst, dst_cap, level);
			if (rc < 0) {
				fprintf(stderr, "compress error (type %u)\n",
					comp_type);
				exit(1);
			}
			/* rc == 0 => store-raw fallback (not smaller). */
			stored += (rc > 0) ? (uint32_t)rc : blk;
		}
	}
	t1 = now_sec();

	{
		double unc_mb = (double)blk * blocks / (1024.0 * 1024.0);
		r.mbps = unc_mb / (t1 - t0 > 0 ? t1 - t0 : 1e-9);
		r.ratio = (double)((uint64_t)blk * blocks) / (double)(stored ? stored : 1);
	}
	return r;
}

static void report(const char *stream, const uint8_t *src, uint32_t blk,
		   uint32_t blocks, uint8_t *dst, uint32_t dst_cap, int zlevel)
{
	struct result none, lz4, zs1, zs3;

	none = run(0, 0, src, blk, blocks, dst, dst_cap);
	lz4  = run(LZ4, 0, src, blk, blocks, dst, dst_cap);
	zs1  = run(ZSTD, zlevel, src, blk, blocks, dst, dst_cap);
	zs3  = run(ZSTD, 3, src, blk, blocks, dst, dst_cap);

	printf("  %-16s %10s %8s\n", stream, "MB/s", "ratio");
	printf("    %-14s %10.0f %8.2f\n", "none (raw)", none.mbps, none.ratio);
	printf("    %-14s %10.0f %8.2f\n", "LZ4",        lz4.mbps,  lz4.ratio);
	printf("    %-9s lvl%d %10.0f %8.2f\n", "ZSTD",  zlevel, zs1.mbps, zs1.ratio);
	printf("    %-9s lvl%d %10.0f %8.2f\n", "ZSTD",  3,      zs3.mbps, zs3.ratio);
}

int main(int argc, char **argv)
{
	uint32_t total_mib = (argc > 1) ? (uint32_t)strtoul(argv[1], NULL, 10) : 256;
	uint32_t blk_kib   = (argc > 2) ? (uint32_t)strtoul(argv[2], NULL, 10) : 256;
	int	 zlevel    = (argc > 3) ? atoi(argv[3]) : 1;
	uint32_t blk       = blk_kib * 1024;
	uint32_t blocks    = (uint32_t)((uint64_t)total_mib * 1024 * 1024 / blk);
	uint8_t *comp_buf, *rand_buf, *dst;
	uint32_t dst_cap;

	if (!blocks)
		blocks = 1;
	dst_cap = mhvtl_zstd_compress_bound(blk);
	if (mhvtl_lz4_compress_bound(blk) > dst_cap)
		dst_cap = mhvtl_lz4_compress_bound(blk);

	comp_buf = malloc(blk);
	rand_buf = malloc(blk);
	dst	 = malloc(dst_cap);
	if (!comp_buf || !rand_buf || !dst) {
		fprintf(stderr, "out of memory\n");
		return 1;
	}
	fill_compressible(comp_buf, blk);
	fill_random(rand_buf, blk);

	printf("mhVTL compression benchmark: %u MiB, %u KiB blocks (%u blocks)\n",
	       total_mib, blk_kib, blocks);
	report("compressible", comp_buf, blk, blocks, dst, dst_cap, zlevel);
	report("uncompressible", rand_buf, blk, blocks, dst, dst_cap, zlevel);

	free(comp_buf);
	free(rand_buf);
	free(dst);
	return 0;
}
