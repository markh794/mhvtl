#!/bin/bash
#
# edit_tape must not turn a WORM cartridge back into rewritable media.
# Real LTO/3592/T10000 WORM is one-way (INCITS SSC; the WORM identity lives in
# the factory servo pattern and cartridge memory), so edit_tape refuses to
# change a WORM cartridge's MediumType to data, clean or null. Size, density
# and write-protect edits on WORM media are still allowed, and non-WORM
# cartridges are unaffected.
#
# Uses the locally built mktape/edit_tape against a throwaway config/home, so
# no root or mhvtl install is needed.
#
# Usage: tests/test_edit_tape_worm.sh

set -u

here=$(cd "$(dirname "$0")" && pwd)
usr="$here/../usr"
mktape="$usr/bin/mktape"
edit_tape="$usr/bin/edit_tape"
lib="$usr/libvtlscsi.so"

for b in "$mktape" "$edit_tape"; do
	if [ ! -x "$b" ]; then
		echo "SKIP: $(basename "$b") not built (run make -C usr first)"
		exit 0
	fi
done

# Run a locally built tool against its freshly built libvtlscsi.so.
run() { LD_LIBRARY_PATH="$usr" MHVTL_CONFIG_PATH="$conf" "$@"; }

base=$(mktemp -d /tmp/mhvtl-edit-worm-XXXXXX) || exit 1
conf="$base/conf"
home="$base/home"
mkdir -p "$conf" "$home"
trap 'rm -rf "$base"' EXIT

LIB=77
cat > "$conf/device.conf" <<EOF
VERSION: 5

Library: $LIB CHANNEL: 00 TARGET: 00 LUN: 00
 Vendor identification: STK
 Product identification: L700
 Product revision level: 0108
 Unit serial number: TESTLIB77
 Home directory: $home

EOF

failed=0
fail() { echo "FAIL: $*"; failed=$((failed + 1)); }
pass() { echo "ok: $*"; }

mk() { # barcode density type
	run "$mktape" -C "$conf" -H "$home" -l "$LIB" -m "$1" -s 100 -t "$3" -d "$2" \
		>/dev/null 2>&1
}
et() { # args...; sets $out, returns edit_tape exit code
	out=$(run "$edit_tape" -l "$LIB" -m "$1" "${@:2}" 2>&1)
	return $?
}

WORM=WRM001LW   # LTO-6 WORM
DATA=DAT001L8   # LTO-8 data

if ! mk "$WORM" LTO6 WORM; then echo "FAIL: mktape WORM"; exit 1; fi
if ! mk "$DATA" LTO8 data; then echo "FAIL: mktape data"; exit 1; fi

# 1. WORM -> data is refused, non-zero exit, with a clear message.
if et "$WORM" -t data; then
	fail "edit_tape -t data on WORM should fail (exit $?)"
else
	echo "$out" | grep -qi "refus" || fail "no refusal message: $out"
	pass "WORM -> data refused"
fi

# 2. WORM -> clean and WORM -> null are refused too.
for t in clean null; do
	if et "$WORM" -t "$t"; then fail "WORM -> $t should fail"; else pass "WORM -> $t refused"; fi
done

# 3. The refusal left the MAM untouched: a second WORM -> data still refuses
#    (if the first call had written the MAM, the cartridge would now be data
#    and convert cleanly).
if et "$WORM" -t data; then fail "WORM still convertible after refusal (MAM was changed)"; else pass "MAM unchanged after refusal"; fi

# 4. Setting WORM on a WORM cartridge is a no-op, not a conversion: allowed.
if et "$WORM" -t WORM; then pass "WORM -> WORM allowed"; else fail "WORM -> WORM should succeed: $out"; fi

# 5. Non-type edits on WORM media are still allowed (write-protect here).
if et "$WORM" -w on; then pass "write-protect on WORM allowed"; else fail "write-protect on WORM should succeed: $out"; fi

# 6. A data cartridge is unaffected: -t data still works.
if et "$DATA" -t data; then pass "data -> data still works"; else fail "data -> data should succeed: $out"; fi

echo ""
if [ "$failed" -eq 0 ]; then
	echo "=== test_edit_tape_worm: all checks passed ==="
	exit 0
else
	echo "=== test_edit_tape_worm: $failed check(s) FAILED ==="
	exit 1
fi
