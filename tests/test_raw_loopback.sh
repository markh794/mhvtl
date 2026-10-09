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
cleanup() {
	[ -n "$LOOP" ] && $SUDO losetup -d "$LOOP" 2>/dev/null
	rm -f "$IMG" "$TESTBIN"
}
trap cleanup EXIT

fail() { echo "LOOPBACK TEST FAIL: $*" >&2; exit 1; }

echo "=== build libvtlscsi + mhvtl-raw-format ==="
make -C usr libvtlscsi.so bin/mhvtl-raw-format >/tmp/raw_loop_build.log 2>&1 \
	|| fail "build failed (see /tmp/raw_loop_build.log)"

echo "=== compile tests/test_raw_backend ==="
cc -Wall -Wextra -std=gnu99 \
	-I include -I include/utils -I include/common -I ccan \
	tests/test_raw_backend.c -L usr -lvtlscsi -Wl,-rpath,"$HERE/usr" \
	-o "$TESTBIN" >/tmp/raw_loop_cc.log 2>&1 \
	|| fail "test compile failed (see /tmp/raw_loop_cc.log)"

echo "=== create 512 MiB backing image + loopback device ==="
truncate -s 512M "$IMG" || fail "truncate failed"
LOOP="$($SUDO losetup -f --show "$IMG")" || fail "losetup failed"
echo "loop device: $LOOP"

echo "=== format (multi-partition 92,8) ==="
$SUDO env LD_LIBRARY_PATH="$HERE/usr" ./usr/bin/mhvtl-raw-format "$LOOP" \
	--barcode TEST001L8 --partition-data 92,8 --min-record-size 64K \
	|| fail "mhvtl-raw-format failed"

echo "=== run direct raw-backend round-trip ==="
$SUDO env LD_LIBRARY_PATH="$HERE/usr" "$TESTBIN" "$LOOP"
rc=$?

if [ $rc -eq 0 ]; then
	echo "=== LOOPBACK TEST PASSED ==="
else
	fail "test_raw_backend exited $rc"
fi
