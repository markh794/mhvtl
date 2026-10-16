#!/bin/bash
#
# Check that make_vtl_media seeds each library's media in that library's own
# 'Home directory:' (falling back to MHVTL_HOME_PATH), and that a second run
# leaves existing media alone.
#
# Runs unprivileged against a temp dir, with the make_vtl_media and mktape
# that `make` builds in usr/bin.
#
# Usage: tests/make_vtl_media_home.sh

set -u

here=$(cd "$(dirname "$0")" && pwd)
bin=$here/../usr/bin
script=$bin/make_vtl_media
# mktape finds libvtlscsi.so in the build tree, not an install
export LD_LIBRARY_PATH="$here/../usr${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

for f in "$script" "$bin/mktape"; do
	[[ -x $f ]] || { echo "missing $f; run make first"; exit 2; }
done

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

cfg=$tmp/etc
mkdir -p "$cfg" "$tmp/global" "$tmp/home10" "$tmp/home20"

cat >"$cfg/mhvtl.conf" <<EOF
MHVTL_HOME_PATH=$tmp/global
CAPACITY=8
EOF

# Libraries 10 and 20 have their own homes; 30 has none and uses the global one
cat >"$cfg/device.conf" <<EOF
VERSION: 5

Library: 10 CHANNEL: 00 TARGET: 00 LUN: 00
 Vendor identification: STK
 Product identification: L700
 Unit serial number: LIB10
 Home directory: $tmp/home10
 PERSIST: False

Drive: 11 CHANNEL: 00 TARGET: 01 LUN: 00
 Library ID: 10 Slot: 01
 Vendor identification: IBM
 Product identification: ULT3580-TD5
 Unit serial number: DRV11
 Home directory: $tmp/home10

Library: 20 CHANNEL: 00 TARGET: 08 LUN: 00
 Vendor identification: STK
 Product identification: L700
 Unit serial number: LIB20
 Home directory: $tmp/home20
 PERSIST: False

Library: 30 CHANNEL: 00 TARGET: 09 LUN: 00
 Vendor identification: STK
 Product identification: L700
 Unit serial number: LIB30
 PERSIST: False
EOF

for l in 10:A1 20:B2 30:C3; do
	id=${l%%:*}
	p=${l##*:}
	printf 'VERSION: 2\n\nDrive 1:\n\nPicker 1:\n\nMAP 1:\n\nSlot 1: %s0001L5\nSlot 2: %s0002L5\n' "$p" "$p" \
		>"$cfg/library_contents.$id"
done

failed=0
checked=0
check() { # check <description> <command...>
	local what=$1
	shift
	checked=$((checked + 1))
	if ! "$@"; then
		failed=$((failed + 1))
		echo "FAIL $what"
	fi
}
ls_media() { ls "$1" 2>/dev/null | sort | tr '\n' ' '; }
same() { [[ "$1" == "$2" ]]; }

run() {
	"$script" -C "$cfg" -m "$bin" "$@" >"$tmp/out" 2>&1 || { cat "$tmp/out"; return 1; }
}

check "--help lists --per-library-home" \
	bash -c "'$script' --help | grep -q -- '--per-library-home'"

check "first run" run

check "library 10 media only in its home" same "$(ls_media "$tmp/home10")" "A10001L5 A10002L5 "
check "library 20 media only in its home" same "$(ls_media "$tmp/home20")" "B20001L5 B20002L5 "
check "library 30 media in the global home" same "$(ls_media "$tmp/global")" "C30001L5 C30002L5 "

before=$(cd "$tmp" && find global home10 home20 -type f -printf '%p %s %T@\n' | sort)
check "second run" run
after=$(cd "$tmp" && find global home10 home20 -type f -printf '%p %s %T@\n' | sort)
check "second run leaves existing media untouched" same "$before" "$after"
check "second run reports them as present" grep -q "Directory already exists: $tmp/home10/A10001L5" "$tmp/out"

# The old single-home layout stays available
rm -rf "$tmp/global"/* "$tmp/home10"/* "$tmp/home20"/*
check "--no-per-library-home run" run --no-per-library-home
check "--no-per-library-home keeps everything in the global home" \
	same "$(ls_media "$tmp/global")" "A10001L5 A10002L5 B20001L5 B20002L5 C30001L5 C30002L5 "
check "--no-per-library-home leaves the library homes empty" \
	same "$(ls_media "$tmp/home10")$(ls_media "$tmp/home20")" ""

# -H against a config whose libraries name a home: --no-per-library-home puts
# everything in the -H dir (what the top-level `make tape` staging relies on)
rm -rf "$tmp/global"/* "$tmp/home10"/* "$tmp/home20"/*
mkdir -p "$tmp/staged"
check "-H --no-per-library-home run" run -H "$tmp/staged" --no-per-library-home
check "-H --no-per-library-home puts everything in the -H dir" \
	same "$(ls_media "$tmp/staged")" "A10001L5 A10002L5 B20001L5 B20002L5 C30001L5 C30002L5 "
check "-H --no-per-library-home leaves every other home empty" \
	same "$(ls_media "$tmp/home10")$(ls_media "$tmp/home20")$(ls_media "$tmp/global")" ""

# A Home directory: on a drive stanza is not the library's; the library falls
# back to the global home (same as find_media_home_directory() with
# fix/media-home-lookup)
rm -rf "$tmp/staged" "$tmp/global"/*
cat >"$cfg/device.conf" <<EOF
Library: 10 CHANNEL: 00 TARGET: 00 LUN: 00
 Unit serial number: LIB10

Drive: 11 CHANNEL: 00 TARGET: 01 LUN: 00
 Library ID: 10 Slot: 01
 Home directory: $tmp/home20
EOF
check "drive-stanza run" run
check "a drive's Home directory is not the library's" \
	same "$(ls_media "$tmp/global")|$(ls_media "$tmp/home20")" "A10001L5 A10002L5 |"

echo "make_vtl_media_home: $((checked - failed))/$checked checks passed"
[[ $failed -eq 0 ]]
