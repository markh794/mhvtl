/*
 * mhvtl_compress.c — streaming block codecs (LZ4, Zstandard).
 *
 * See include/mhvtl_compress.h for the contract. This module has no dependency
 * on the tape daemon's global state, so it is linked into the tape daemon and
 * compiled directly into the round-trip unit test.
 *
 * Copyright (C) 2025 mhVTL modernization
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <stdint.h>
#include <lz4.h>
#include <zstd.h>

#include "vtlcart.h"		/* LZ4, ZSTD comp_type values */
#include "mhvtl_compress.h"

uint32_t mhvtl_lz4_compress_bound(uint32_t src_sz)
{
	return (uint32_t)LZ4_compressBound((int)src_sz);
}

uint32_t mhvtl_zstd_compress_bound(uint32_t src_sz)
{
	return (uint32_t)ZSTD_compressBound((size_t)src_sz);
}

int mhvtl_compress_block(uint8_t comp_type, const uint8_t *src, uint32_t src_sz,
			 uint8_t *dst, uint32_t dst_cap, int level)
{
	uint32_t comp_len;

	switch (comp_type) {
	case LZ4: {
		int z = LZ4_compress_default((const char *)src, (char *)dst,
					     (int)src_sz, (int)dst_cap);
		if (z <= 0)
			return -1;
		comp_len = (uint32_t)z;
		break;
	}
	case ZSTD: {
		int lo = ZSTD_minCLevel();
		int hi = ZSTD_maxCLevel();
		size_t z;

		if (level < lo)
			level = lo;
		if (level > hi)
			level = hi;

		z = ZSTD_compress(dst, dst_cap, src, src_sz, level);
		if (ZSTD_isError(z))
			return -1;
		comp_len = (uint32_t)z;
		break;
	}
	default:
		return -1;
	}

	/* STORE-RAW fallback: if compression did not shrink the block, tell the
	 * caller to store it uncompressed (comp_type none). */
	if (comp_len >= src_sz)
		return 0;

	return (int)comp_len;
}

int mhvtl_decompress_block(uint8_t comp_type, const uint8_t *src,
			   uint32_t comp_sz, uint8_t *dst, uint32_t dst_cap,
			   uint32_t expected)
{
	uint32_t out_len;

	switch (comp_type) {
	case LZ4: {
		int d = LZ4_decompress_safe((const char *)src, (char *)dst,
					    (int)comp_sz, (int)dst_cap);
		if (d < 0)
			return -1;
		out_len = (uint32_t)d;
		break;
	}
	case ZSTD: {
		size_t d = ZSTD_decompress(dst, dst_cap, src, comp_sz);
		if (ZSTD_isError(d))
			return -1;
		out_len = (uint32_t)d;
		break;
	}
	default:
		return -1;
	}

	if (out_len != expected)
		return -1;

	return (int)out_len;
}
