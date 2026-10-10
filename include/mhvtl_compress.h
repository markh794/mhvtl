/*
 * mhvtl_compress.h — streaming block codecs (LZ4, Zstandard).
 *
 * A thin, backend-agnostic wrapper around liblz4 / libzstd that performs the
 * per-block compress / decompress plus the "store raw when it does not shrink"
 * fallback decision. It is shared verbatim between the tape daemon's write path
 * (writeBlock_lz4 / writeBlock_zstd in usr/mhvtl_io.c) and the round-trip unit
 * test, so the test drives the real code path rather than a re-implementation.
 *
 * Copyright (C) 2025 mhVTL modernization
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef _MHVTL_COMPRESS_H_
#define _MHVTL_COMPRESS_H_

#include <stdint.h>

/* comp_type values (LZ4, ZSTD) are defined in vtlcart.h. */

/* Worst-case compressed size for a src_sz input, per codec. Callers size the
 * destination buffer with these before calling mhvtl_compress_block(). */
uint32_t mhvtl_lz4_compress_bound(uint32_t src_sz);
uint32_t mhvtl_zstd_compress_bound(uint32_t src_sz);

/*
 * Compress src_sz bytes from src into dst (capacity dst_cap) using comp_type
 * (LZ4 or ZSTD). `level` is the compression level; it is used by ZSTD (clamped
 * to the library's valid range) and ignored by LZ4.
 *
 * Returns:
 *   > 0  the compressed length to store; the block shrank and dst holds the
 *        compressed form (caller records comp_type LZ4/ZSTD).
 *   = 0  STORE-RAW fallback: the compressed form was not smaller than the
 *        input, so the caller must store the block uncompressed (comp_type
 *        none). dst contents are undefined.
 *   < 0  hard compression/codec error.
 */
int mhvtl_compress_block(uint8_t comp_type, const uint8_t *src, uint32_t src_sz,
			 uint8_t *dst, uint32_t dst_cap, int level);

/*
 * Decompress comp_sz bytes from src into dst (capacity dst_cap) using comp_type
 * (LZ4 or ZSTD). `expected` is the uncompressed size recorded in the block
 * header; a decoded length that differs is treated as corruption.
 *
 * Returns the decompressed length (== expected) on success, or < 0 on a decode
 * error or size mismatch.
 */
int mhvtl_decompress_block(uint8_t comp_type, const uint8_t *src,
			   uint32_t comp_sz, uint8_t *dst, uint32_t dst_cap,
			   uint32_t expected);

#endif /* _MHVTL_COMPRESS_H_ */
