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

`tests/bench_compression.c` compares **all** supported codecs — none, LZO,
ZLIB, LZ4, and ZSTD (two levels) — on a synthetic compressible stream and a
synthetic uncompressible (random) stream, driving each codec through its own
library exactly as the tape write path does. For every codec it reports the
achieved ratio and both the compression and decompression throughput.

```
make -C tests bench                       # defaults: 256 MiB, 256 KiB blocks, zstd level 1
make -C tests bench BENCH_ARGS="512 64 3" # total_MiB block_KiB zstd_level
```

Throughput is reported over the uncompressed size (the MB/s a backup stream
sees); ratio is uncompressed/stored. Each codec is round-tripped bit-identically
before timing. The figures are single-threaded, CPU-bound, and operate on an
in-cache block, so they isolate codec cost rather than disk I/O; `none` is a
plain `memcpy` (the raw-store ceiling). Numbers are per-core and strongly
data-dependent — treat them as relative, not absolute.

### Compression ratio and processing speed (as tested)

Host: openSUSE Tumbleweed, Linux 6.18 x86_64; 256 MiB in 256 KiB blocks, ZLIB at
its default level 6, ZSTD at levels 1 and 3. `comp` = compression throughput,
`decomp` = decompression (restore) throughput.

**Compressible stream** (≈2-4× reducible, representative of typical backup data):

| codec       | ratio | comp MB/s | decomp MB/s |
|-------------|------:|----------:|------------:|
| none (raw)  |  1.00 |     50536 |       49083 |
| LZO         |  2.15 |       515 |         618 |
| ZLIB lvl6   |  3.70 |        53 |         487 |
| LZ4         |  2.27 |       595 |        3666 |
| ZSTD lvl1   |  3.52 |       417 |        1501 |
| ZSTD lvl3   |  3.82 |       370 |        1354 |

**Uncompressible stream** (random):

| codec       | ratio | comp MB/s | decomp MB/s |
|-------------|------:|----------:|------------:|
| none (raw)  |  1.00 |     56380 |       57548 |
| LZO         |  1.00 |     18392 |       26885 |
| ZLIB lvl6   |  1.00 |        55 |        3754 |
| LZ4         |  1.00 |     19769 |       50368 |
| ZSTD lvl1   |  1.00 |      9267 |       54323 |
| ZSTD lvl3   |  1.00 |      6434 |       55457 |

Observations:
- **ZLIB is the bottleneck the modern codecs replace**: at its default level it
  compresses at only ~53 MB/s regardless of data — ~8-11× slower than LZ4/ZSTD
  for a similar or worse ratio.
- **LZ4** is the throughput choice: fastest compression (~595 MB/s) and very fast
  decompression (~3.7 GB/s), at a modest ~2.3× ratio.
- **ZSTD** is the ratio choice: best ratio (≈3.5-3.8×) at ~370-420 MB/s, still far
  above the legacy ZLIB and decompressing at 1.3-1.5 GB/s. Level 3 buys a little
  ratio for a little speed.
- **LZO** sits between — ~2.15× at ~515 MB/s — and LZ4 beats it on both axes.
- On random data every codec reports ratio 1.00: for LZ4/ZSTD the **store-raw
  fallback** fires (never expands); the codecs also reject fast (many GB/s),
  except ZLIB which pays its full CPU cost even when it cannot compress.
- Against AGENTS.md §10's ≥ 400 MB/s target: LZ4 and ZSTD level 1 clear it on
  compressible data; ZSTD level 3 trades a little throughput for ratio.

The ZSTD path reuses a persistent `ZSTD_CCtx`/`ZSTD_DCtx` (created on first use,
kept for the life of the process; the daemon is single-threaded) instead of the
per-block one-shot `ZSTD_compress`/`ZSTD_decompress`, which allocate and free a
context on every call. The compressed output is byte-identical, so the on-disk
format is unchanged. A controlled same-run A/B measured the benefit at roughly
**+5% at 4-64 KiB blocks and ~+1% at 256 KiB** — the per-block allocation is a
larger fraction of the work for smaller blocks; at tape block sizes the dominant
cost is the codec itself, not the context. It is a free win and avoids per-block
malloc/free churn on the hot path. (Throughput figures above are per-core and
host-load-dependent — treat them as relative, not absolute.)
