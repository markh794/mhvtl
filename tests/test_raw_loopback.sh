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
MNT=""
TESTBIN="$(mktemp /tmp/test_raw_backend-XXXXXX)"
LOADBIN="$(mktemp /tmp/test_raw_load_tape-XXXXXX)"
COMPBIN="$(mktemp /tmp/test_compression-XXXXXX)"
SELBIN="$(mktemp /tmp/test_compression_select-XXXXXX)"
CTBIN="$(mktemp /tmp/get_cart_type_blk-XXXXXX)"
CTHOME="$(mktemp -d /tmp/mhvtl-raw-home-XXXXXX)"
BARCODE="TST001L8"
cleanup() {
	[ -n "$MNT" ] && { $SUDO umount "$MNT" 2>/dev/null; rmdir "$MNT"; }
	[ -n "$LOOP" ] && $SUDO losetup -d "$LOOP" 2>/dev/null
	rm -f "$IMG" "$TESTBIN" "$LOADBIN" "$COMPBIN" "$SELBIN" "$CTBIN"
	rm -rf "$CTHOME"
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

# A 6-character barcode has no media suffix, so the density must be given.
echo "=== format without a media suffix needs --density ==="
if $SUDO env LD_LIBRARY_PATH="$HERE/usr" ./usr/bin/mhvtl-raw-format "$LOOP" \
	--barcode TST002 >/tmp/raw_loop_density.log 2>&1; then
	fail "mhvtl-raw-format formatted a barcode without a media type"
fi
grep -q -- "--density is required" /tmp/raw_loop_density.log \
	|| fail "expected a --density hint (see /tmp/raw_loop_density.log)"
$SUDO env LD_LIBRARY_PATH="$HERE/usr" ./usr/bin/mhvtl-raw-format "$LOOP" \
	--barcode TST002 --density LTO7 \
	|| fail "mhvtl-raw-format --density failed"

# A density that contradicts the barcode suffix, and more partitions than the
# media allows (LTO-4 has one), are refused before the device is written: the
# TST002 cartridge formatted above must still be there afterwards.
echo "=== contradicting density and too many partitions refused ==="
if $SUDO env LD_LIBRARY_PATH="$HERE/usr" ./usr/bin/mhvtl-raw-format "$LOOP" \
	--barcode TST004L8 --density LTO7 >/tmp/raw_loop_conflict.log 2>&1; then
	fail "mhvtl-raw-format accepted --density LTO7 for an L8 barcode"
fi
if $SUDO env LD_LIBRARY_PATH="$HERE/usr" ./usr/bin/mhvtl-raw-format "$LOOP" \
	--barcode TST005L4 --partition-data 50,50 >/tmp/raw_loop_parts.log 2>&1; then
	fail "mhvtl-raw-format formatted two partitions on LTO-4 media"
fi
$SUDO dd if="$LOOP" bs=1M skip=1 count=1 status=none | grep -aq TST002 \
	|| fail "a refused format overwrote the existing superblock"

# M8 is LTO-7 media in the LTO-8 Type M format, density 5Dh (GA32-0928-07
# table 113, p.142): the suffix alone selects it, LTO8M8 agrees with it, and
# plain LTO-8 contradicts it like any other density that differs from the
# suffix, which leaves the TST002 cartridge in place. test_raw_load_tape then
# checks the MAM the format wrote.
echo "=== M8 barcode formats as LTO-8 Type M ==="
if $SUDO env LD_LIBRARY_PATH="$HERE/usr" ./usr/bin/mhvtl-raw-format "$LOOP" \
	--barcode TST003M8 --density LTO8 >/tmp/raw_loop_m8.log 2>&1; then
	fail "mhvtl-raw-format accepted --density LTO8 for an M8 barcode"
fi
# The "density ... contradicts barcode" reason goes to syslog; stderr only
# carries the EINVAL until markh794/mhvtl#299 prints the reason there too.
grep -q "Invalid argument" /tmp/raw_loop_m8.log \
	|| fail "expected EINVAL (see /tmp/raw_loop_m8.log)"
$SUDO dd if="$LOOP" bs=1M skip=1 count=1 status=none | grep -aq TST002 \
	|| fail "the refused M8 format overwrote the existing superblock"
$SUDO env LD_LIBRARY_PATH="$HERE/usr" ./usr/bin/mhvtl-raw-format "$LOOP" \
	--barcode TST003M8 --density LTO8M8 \
	|| fail "mhvtl-raw-format refused --density LTO8M8 for an M8 barcode"
$SUDO env LD_LIBRARY_PATH="$HERE/usr" ./usr/bin/mhvtl-raw-format "$LOOP" \
	--barcode TST003M8 --partition-data 92,8 --min-record-size 64K \
	|| fail "mhvtl-raw-format refused an M8 barcode"
$SUDO env LD_LIBRARY_PATH="$HERE/usr" "$LOADBIN" "$LOOP" TST003M8 \
	|| fail "test_raw_load_tape on M8 exited $?"

# LH is the LTO-10 WORM label.
echo "=== LTO-10 WORM barcode accepted ==="
$SUDO env LD_LIBRARY_PATH="$HERE/usr" ./usr/bin/mhvtl-raw-format "$LOOP" \
	--barcode TST006LH || fail "mhvtl-raw-format refused an LH barcode"

# vtllibrary types a raw cartridge through <home>/<barcode> -> block device.
# The device now holds the WORM cartridge TST006LH: get_cart_type() must say 4.
echo "=== get_cart_type() on a raw WORM cartridge ==="
gcc -Wall -Wextra -std=gnu99 -D_GNU_SOURCE -D_LARGEFILE64_SOURCE $IFLAGS -o "$CTBIN" tests/get_cart_type_blk.c usr/vtl_cart_type.c \
	-L usr -lvtlscsi >/tmp/raw_loop_cc5.log 2>&1 \
	|| fail "get_cart_type_blk compile failed (see /tmp/raw_loop_cc5.log)"
$SUDO env LD_LIBRARY_PATH="$HERE/usr" "$CTBIN" "$CTHOME" TST006LH "$LOOP" 4 \
	|| fail "get_cart_type() did not report WORM (4) for a raw cartridge"

# mhvtl-raw-format opens the device O_EXCL, so the kernel refuses it with
# EBUSY while the device is in use - here, a mounted filesystem.
echo "=== format refused while the device is mounted ==="
$SUDO mkfs.ext4 -q -F "$LOOP" || fail "mkfs.ext4 failed"
MNT="$(mktemp -d /tmp/mhvtl-raw-mnt-XXXXXX)"
$SUDO mount "$LOOP" "$MNT" || fail "mount failed"
if $SUDO env LD_LIBRARY_PATH="$HERE/usr" ./usr/bin/mhvtl-raw-format "$LOOP" \
	--barcode "$BARCODE" --partition-data 100 >/tmp/raw_loop_busy.log 2>&1; then
	fail "mhvtl-raw-format formatted a mounted device"
fi
grep -q "Device or resource busy" /tmp/raw_loop_busy.log \
	|| fail "expected EBUSY (see /tmp/raw_loop_busy.log)"
$SUDO umount "$MNT" || fail "umount failed"
rmdir "$MNT"
MNT=""

echo "=== LOOPBACK TEST PASSED ==="
