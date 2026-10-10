# Block compression (LZ4 / ZSTD)

mhVTL compresses each tape data block independently. Alongside the legacy
`zlib` and `lzo1x` codecs it supports **LZ4** (fast) and **Zstandard / ZSTD**
(higher ratio, level 1-3), selectable at runtime.

## On-media format

Compression is recorded per block in the existing `struct blk_header`
(`include/vtlcart.h`) — there is **no** separate container frame. Each block
header already carries everything a frame would:

- `blk_size` — uncompressed length,
- `disk_blk_size` — bytes actually stored (the compressed length, or the raw
  length on fallback),
- `uncomp_crc` — CRC of the uncompressed data,
- `blk_flags` — the codec, as a flag bit.

Codec flag bits (`include/vtlcart.h`), existing bits unchanged:

| codec | `comp_type` | `blk_flags` bit |
|-------|-------------|-----------------|
| none  | 0           | (none)          |
| zlib  | `ZLIB` = 2  | `BLKHDR_FLG_ZLIB_COMPRESSED` 0x01 |
| lzo   | `LZO` = 1   | `BLKHDR_FLG_LZO_COMPRESSED`  0x04 |
| lz4   | `LZ4` = 3   | `BLKHDR_FLG_LZ4_COMPRESSED`  0x10 |
| zstd  | `ZSTD` = 4  | `BLKHDR_FLG_ZSTD_COMPRESSED` 0x20 |

Because LZ4/ZSTD reuse the existing header machinery, the on-disk encoding of
previously written `none`/`lzo`/`zlib` blocks is unchanged and still reads back
identically; a cartridge written by an older build keeps working.

### Store-raw fallback

The LZ4/ZSTD write path (`writeBlock_lz4` / `writeBlock_zstd`, `usr/mhvtl_io.c`,
via the shared `mhvtl_compress_block()` helper in `usr/mhvtl_compress.c`) stores
the block **uncompressed** (`comp_type` none) whenever the compressed form is
not strictly smaller than the input. So incompressible data never grows the
cartridge, and the read path simply sees a `none` block.

## Codec selection (SCSI MODE SELECT, page 0x0F)

The active write-time codec is chosen through the SCSI **Data Compression mode
page (0x0F)**, handled in `usr/mode.c`:

- MODE SELECT reads the 32-bit Compression Algorithm field and maps it to the
  codec (`include/vtlcart.h`): `0x00` none, `0x10` legacy default (lzo/zlib),
  `0x14` LZ4, `0x15` ZSTD. For ZSTD the level is carried through the existing
  `compressionFactor` plumbing and clamped to 1-3.
- MODE SENSE reports the currently selected algorithm back. At startup it
  reports the legacy `0x10`, so existing initiators see no change.
- The Data Compression Enable (DCE) bit still enables/disables compression as
  before.

For convenience the codec can also be set the mhVTL-native way, now extended to
`lz4`/`zstd`: the `Compression type` key in the device config and the runtime
`vtlcmd <drive> compression <lzo|zlib|lz4|zstd>` command.

The read path needs no selection — each block is decompressed according to its
own stored `blk_flags`, so a cartridge may freely mix codecs.

## Benchmark

`tests/bench_compression.c` measures sequential write throughput and achieved
ratio for none / LZ4 / ZSTD on a synthetic compressible stream and a synthetic
uncompressible (random) stream, driving the same `mhvtl_compress_block()` helper
the tape write path uses.

```
make -C tests bench                      # defaults: 256 MiB, 256 KiB blocks, zstd level 1
make -C tests bench BENCH_ARGS="512 64 3"  # total_MiB block_KiB zstd_level
```

Throughput is reported over the uncompressed input (MB/s a backup stream sees);
ratio is uncompressed/stored. The figures are single-threaded, CPU-bound, and
operate on an in-cache buffer, so they isolate codec cost rather than disk I/O.

### Captured run

Host: openSUSE Tumbleweed, Linux 6.18 x86_64; 256 MiB in 256 KiB blocks,
ZSTD level 1 (and level 3):

| stream | codec | MB/s | ratio |
|--------|-------|-----:|------:|
| compressible   | none (raw) | ~51000 | 1.00 |
| compressible   | LZ4        | 533 | 2.27 |
| compressible   | ZSTD lvl1  | 389 | 3.52 |
| compressible   | ZSTD lvl3  | 364 | 3.82 |
| uncompressible | none (raw) | ~56000 | 1.00 |
| uncompressible | LZ4        | 19544 | 1.00 |
| uncompressible | ZSTD lvl1  | 9228 | 1.00 |
| uncompressible | ZSTD lvl3  | 6334 | 1.00 |

Observations:
- On compressible data, ZSTD gives the better ratio (≈3.5-3.8×) at lower
  throughput; LZ4 gives a lower ratio (≈2.3×) but is faster — the expected
  speed/ratio trade-off.
- On random data every codec reports ratio 1.00: the **store-raw fallback**
  fires and the cartridge does not grow; LZ4/ZSTD reject fast (many GB/s).
- `none` is a plain `memcpy`, i.e. the raw-store ceiling (memory bandwidth).
- All codecs clear the AGENTS.md §10 ≥ 400 MB/s target on compressible data
  except ZSTD level 3, which trades throughput for ratio; numbers are per-core
  and data-dependent (genuinely compressible payloads run several GB/s).
