#!/bin/bash
#
# test_raw_loopback.sh — end-to-end check of the raw block-device backend on a
# real loopback device. Formats a loop device with mhvtl-raw-format, then runs
# tests/test_raw_backend against it (direct cart_backend_raw round-trip).
#
# Requires root for losetup + opening the device (uses sudo). Run from the repo:
#   sudo tests/test_raw_loopback.sh      # or: tests/test_raw_loopback.sh (uses sudo)
#
set -u
HERE="$(cd "$(dirname "$0")/.." && pwd)"   # repo root (usr/, include/, tests/)
cd "$HERE"

SUDO=""
[ "$(id -u)" -ne 0 ] && SUDO="sudo"

IMG="$(mktemp /tmp/mhvtl-raw-XXXXXX.img)"
LOOP=""
TESTBIN="$(mktemp /tmp/test_raw_backend-XXXXXX)"
LOADBIN="$(mktemp /tmp/test_raw_load_tape-XXXXXX)"
COMPBIN="$(mktemp /tmp/test_compression-XXXXXX)"
SELBIN="$(mktemp /tmp/test_compression_select-XXXXXX)"
BARCODE="TEST001L8"
cleanup() {
	[ -n "$LOOP" ] && $SUDO losetup -d "$LOOP" 2>/dev/null
	rm -f "$IMG" "$TESTBIN" "$LOADBIN" "$COMPBIN" "$SELBIN"
}
trap cleanup EXIT

fail() { echo "LOOPBACK TEST FAIL: $*" >&2; exit 1; }

IFLAGS="-I include -I include/utils -I include/common -I ccan"

echo "=== build libvtlscsi + mhvtl-raw-format ==="
make -C usr libvtlscsi.so bin/mhvtl-raw-format >/tmp/raw_loop_build.log 2>&1 \
	|| fail "build failed (see /tmp/raw_loop_build.log)"

echo "=== compile tests/test_raw_backend + tests/test_raw_load_tape ==="
cc -Wall -Wextra -std=gnu99 $IFLAGS \
	tests/test_raw_backend.c -L usr -lvtlscsi -Wl,-rpath,"$HERE/usr" \
	-o "$TESTBIN" >/tmp/raw_loop_cc.log 2>&1 \
	|| fail "test_raw_backend compile failed (see /tmp/raw_loop_cc.log)"
cc -Wall -Wextra -std=gnu99 $IFLAGS \
	tests/test_raw_load_tape.c -L usr -lvtlscsi -Wl,-rpath,"$HERE/usr" \
	-o "$LOADBIN" >/tmp/raw_loop_cc2.log 2>&1 \
	|| fail "test_raw_load_tape compile failed (see /tmp/raw_loop_cc2.log)"

echo "=== compile tests/test_compression (raw backend enabled) ==="
cc -Wall -Wextra -std=gnu99 -DRAW_BACKEND_AVAILABLE $IFLAGS \
	tests/test_compression.c usr/mhvtl_compress.c -L usr -lvtlscsi -llz4 -lzstd \
	-Wl,-rpath,"$HERE/usr" \
	-o "$COMPBIN" >/tmp/raw_loop_cc3.log 2>&1 \
	|| fail "test_compression compile failed (see /tmp/raw_loop_cc3.log)"

echo "=== compile tests/test_compression_select (runtime codec selection) ==="
cc -Wall -Wextra -std=gnu99 $IFLAGS \
	tests/test_compression_select.c usr/mhvtl_compress.c -L usr -lvtlscsi -llz4 -lzstd \
	-Wl,-rpath,"$HERE/usr" \
	-o "$SELBIN" >/tmp/raw_loop_cc4.log 2>&1 \
	|| fail "test_compression_select compile failed (see /tmp/raw_loop_cc4.log)"

echo "=== create 512 MiB backing image + loopback device ==="
truncate -s 512M "$IMG" || fail "truncate failed"
LOOP="$($SUDO losetup -f --show "$IMG")" || fail "losetup failed"
echo "loop device: $LOOP"

echo "=== format (multi-partition 92,8) ==="
$SUDO env LD_LIBRARY_PATH="$HERE/usr" ./usr/bin/mhvtl-raw-format "$LOOP" \
	--barcode "$BARCODE" --partition-data 92,8 --min-record-size 64K \
	|| fail "mhvtl-raw-format failed"

echo "=== run direct raw-backend round-trip ==="
$SUDO env LD_LIBRARY_PATH="$HERE/usr" "$TESTBIN" "$LOOP" \
	|| fail "test_raw_backend exited $?"

echo "=== run LZ4/ZSTD compression round-trip (file + raw backends) ==="
$SUDO env LD_LIBRARY_PATH="$HERE/usr" "$COMPBIN" "$LOOP" \
	|| fail "test_compression exited $?"

# test_raw_backend deliberately writes non-tape byte patterns into the
# partition regions, so reformat to hand test_raw_load_tape clean media.
echo "=== reformat for end-to-end test ==="
$SUDO env LD_LIBRARY_PATH="$HERE/usr" ./usr/bin/mhvtl-raw-format "$LOOP" \
	--barcode "$BARCODE" --partition-data 92,8 --min-record-size 64K \
	|| fail "reformat failed"

echo "=== run load_tape() end-to-end integration ==="
$SUDO env LD_LIBRARY_PATH="$HERE/usr" "$LOADBIN" "$LOOP" "$BARCODE" \
	|| fail "test_raw_load_tape exited $?"

# Hand the selection test clean media (test_raw_load_tape left a data block).
echo "=== reformat for codec-selection write-path test ==="
$SUDO env LD_LIBRARY_PATH="$HERE/usr" ./usr/bin/mhvtl-raw-format "$LOOP" \
	--barcode "$BARCODE" --partition-data 92,8 --min-record-size 64K \
	|| fail "reformat failed"

echo "=== run runtime codec selection + write-path flag check ==="
$SUDO env LD_LIBRARY_PATH="$HERE/usr" "$SELBIN" "$LOOP" "$BARCODE" \
	|| fail "test_compression_select exited $?"

echo "=== LOOPBACK TEST PASSED ==="
