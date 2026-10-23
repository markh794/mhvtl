#!/bin/bash
#
# test_raw_provision.sh — end-to-end check of mhvtl-raw-provision against a real
# vtllibrary daemon and a real loopback block device.
#
#   1. format a loop device with mhvtl-raw-format (barcode BAY001L8),
#   2. compile + run tests/test_raw_provision (starts a vtllibrary via the test
#      harness, runs the provisioner, asserts the home/<barcode> symlink and the
#      MAP occupancy via the library's "list map" response),
#   3. dedup: point two bays at the same disk and confirm the provisioner loads
#      NEITHER and reports the collision.
#
# Requires root for losetup + opening the device (uses sudo). Run from the repo:
#   sudo tests/test_raw_provision.sh   # or: tests/test_raw_provision.sh (uses sudo)
#
set -u
HERE="$(cd "$(dirname "$0")/.." && pwd)"   # repo root (usr/, include/, tests/)
cd "$HERE"

SUDO=""
[ "$(id -u)" -ne 0 ] && SUDO="sudo"

IMG="$(mktemp /tmp/mhvtl-prov-XXXXXX.img)"
LOOP=""
DRVBIN="$HERE/tests/test_raw_provision"
DEDUP_CONF="$(mktemp /tmp/mhvtl-dedup-XXXXXX.conf)"
BARCODE="BAY001L8"
# Logs go to a directory of our own: fixed names in /tmp left by an earlier
# run as root could not be overwritten. It is kept when the script fails.
LOGDIR="$(mktemp -d /tmp/mhvtl-raw-prov-XXXXXX)"
cleanup() {
	local rc=$?
	[ -n "$LOOP" ] && $SUDO losetup -d "$LOOP" 2>/dev/null
	rm -f "$IMG" "$DEDUP_CONF"
	if [ "$rc" -eq 0 ]; then rm -rf "$LOGDIR"; else echo "logs kept in $LOGDIR" >&2; fi
}
trap cleanup EXIT

fail() { echo "PROVISION TEST FAIL: $*" >&2; exit 1; }

echo "=== build libvtlscsi + tools + daemons ==="
make -C usr libvtlscsi.so bin/mhvtl-raw-format bin/mhvtl-raw-provision \
	bin/vtllibrary bin/vtltape bin/mktape >"$LOGDIR/build.log" 2>&1 \
	|| fail "build failed (see $LOGDIR/build.log)"

echo "=== build tests/test_raw_provision ==="
make -C tests test_raw_provision >"$LOGDIR/cc.log" 2>&1 \
	|| fail "driver build failed (see $LOGDIR/cc.log)"
if grep -q 'warning\|error' "$LOGDIR/cc.log"; then
	echo "compiler diagnostics:"; cat "$LOGDIR/cc.log"
fi

echo "=== create 512 MiB backing image + loopback device ==="
truncate -s 512M "$IMG" || fail "truncate failed"
LOOP="$($SUDO losetup -f --show "$IMG")" || fail "losetup failed"
echo "loop device: $LOOP"

echo "=== format raw cartridge (barcode $BARCODE) ==="
$SUDO env LD_LIBRARY_PATH="$HERE/usr" ./usr/bin/mhvtl-raw-format "$LOOP" \
	--barcode "$BARCODE" --partition-data 92,8 --min-record-size 64K \
	|| fail "mhvtl-raw-format failed"

echo "=== run provisioner integration driver ==="
$SUDO env "$DRVBIN" "$LOOP" "$BARCODE" || fail "test_raw_provision driver failed"

echo "=== dedup: two bays, same disk/barcode -> load neither ==="
cat > "$DEDUP_CONF" <<EOF
# two bays pointing at the same formatted disk -> barcode collision
10 1 $LOOP
11 1 $LOOP
EOF
DEDUP_OUT="$($SUDO env LD_LIBRARY_PATH="$HERE/usr" \
	./usr/bin/mhvtl-raw-provision --config "$DEDUP_CONF" --verbose 2>&1)"
echo "$DEDUP_OUT"
PLACED=$(printf '%s\n' "$DEDUP_OUT" | grep -c '^PLACED ')
SKIPPED=$(printf '%s\n' "$DEDUP_OUT" | grep -c '^SKIPPED ')
[ "$PLACED" -eq 0 ] || fail "dedup: expected 0 PLACED, got $PLACED"
[ "$SKIPPED" -eq 2 ] || fail "dedup: expected 2 SKIPPED, got $SKIPPED"
printf '%s\n' "$DEDUP_OUT" | grep -qi collision \
	|| fail "dedup: expected a collision message"

echo "=== PROVISION TEST PASSED ==="
