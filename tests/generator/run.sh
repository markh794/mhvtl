#!/bin/bash
#
# Test usr/cmd/mhvtl-device-conf-generator.c against the device.conf the build
# produces (etc/device.conf) and against hand-written edge cases.
#
# Runs unprivileged in a temp dir: the generator is compiled here with
# SYSTEMD_SERVICE_DIR pointing at the built units, and MHVTL_CONFIG_PATH
# redirects the config directory. Needs a prior `make` (libvtlscsi.so, etc/).
#
# Usage: tests/generator/run.sh

set -u

here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../.." && pwd)
etc=$top/etc
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

failed=0
checked=0

ok() { checked=$((checked + 1)); }
fail() { checked=$((checked + 1)); failed=$((failed + 1)); echo "FAIL $*"; }

for f in "$etc/device.conf" "$top/usr/libvtlscsi.so"; do
	[[ -e $f ]] || { echo "missing $f; run make first"; exit 2; }
done

gen=$tmp/gen
gcc -Wall -Wextra -D_GNU_SOURCE -std=gnu99 \
	-I"$top/include" -I"$top/include/utils" -I"$top/include/common" -I"$top/ccan" \
	-DMHVTL_VERSION=\"test\" -DMHVTL_GITDATE=\"test\" -DMHVTL_GITHASH=\"test\" \
	-DMHVTL_HOME_PATH=\"/nonexistent\" -DMHVTL_CONFIG_PATH=\"/nonexistent\" \
	-DSYSTEMD_SERVICE_DIR=\""$etc"\" \
	-o "$gen" "$top/usr/cmd/mhvtl-device-conf-generator.c" \
	-L"$top/usr" -lvtlscsi -Wl,-rpath,"$top/usr" || { echo "generator build failed"; exit 2; }

# run_gen <config-dir> <out-dir>: stdout+stderr in $tmp/log, exit code in $rc
run_gen() {
	mkdir -p "$2"
	MHVTL_CONFIG_PATH=$1 "$gen" "$2" /unused /unused >"$tmp/log" 2>&1
	rc=$?
}

# links <out-dir>: sorted "name -> target" lines of the generated wants dir
links() {
	local l
	for l in "$1"/mhvtl.target.wants/*; do
		[[ -L $l ]] && echo "$(basename "$l") -> $(readlink "$l")"
	done | sort
}

# want_from_conf <device.conf>: the units a hand-enabled install would carry,
# one "name -> template" line per Library:/Drive: entry
want_from_conf() {
	tr -d '\r' <"$1" | awk -v d="$etc" '
		/^Library: *[0-9]+/ { n = $2 + 0; if (n > 0) print "vtllibrary@" n ".service -> " d "/vtllibrary@.service" }
		/^Drive: *[0-9]+/   { n = $2 + 0; if (n > 0) print "vtltape@" n ".service -> " d "/vtltape@.service" }
	' | sort
}

mkconf() { # mkconf <name>; body on stdin
	mkdir -p "$tmp/$1"
	cat >"$tmp/$1/device.conf"
	: >"$tmp/$1/mhvtl.conf"
}

# (a) shipped device.conf: generated set equals the awk-derived set
mkdir -p "$tmp/shipped"
cp "$etc/device.conf" "$tmp/shipped/device.conf"
: >"$tmp/shipped/mhvtl.conf"
run_gen "$tmp/shipped" "$tmp/shipped.out"
want=$(want_from_conf "$etc/device.conf")
got=$(links "$tmp/shipped.out")
if [[ $rc -eq 0 && -n $want && "$got" == "$want" ]]; then ok
else fail "(a) shipped device.conf rc=$rc"; diff <(echo "$want") <(echo "$got"); fi

# (b) systemd-analyze verify over the built units plus the generated dir
if ! command -v systemd-analyze >/dev/null; then
	echo "SKIP (b),(c): systemd-analyze not available"
else
	# verify: <unit-path> <unit>...; the commands are not installed and man
	# pages are not browsable here, so those two notices are expected.
	verify() {
		local path=$1
		shift
		SYSTEMD_UNIT_PATH=$path systemd-analyze verify --recursive-errors=no "$@" 2>&1 |
			grep -v -e 'is not executable' -e "(man)' failed" -e "Can't show"
	}
	units=$tmp/units
	mkdir -p "$units"
	cp "$etc"/*.service "$etc/mhvtl.target" "$units/"
	out=$(verify "$tmp/shipped.out:$units" "$units/mhvtl.target" "$tmp"/shipped.out/mhvtl.target.wants/*.service)
	if [[ -z $out ]]; then ok; else fail "(b) systemd-analyze verify"; echo "$out"; fi

	# (c) hand-enabled links in /etc/systemd/system/mhvtl.target.wants coexist
	fake=$tmp/etc-systemd-system
	mkdir -p "$fake/mhvtl.target.wants"
	ln -s "$etc/vtllibrary@.service" "$fake/mhvtl.target.wants/vtllibrary@10.service"
	ln -s "$etc/vtllibrary@.service" "$fake/mhvtl.target.wants/vtllibrary@99.service"
	before=$(links "$tmp/shipped.out")
	run_gen "$tmp/shipped" "$tmp/coexist.out"
	merged=$(ls "$fake/mhvtl.target.wants" "$tmp/coexist.out/mhvtl.target.wants" | grep '\.service$' | sort -u | wc -l)
	out=$(verify "$tmp/coexist.out:$fake:$units" "$units/mhvtl.target" \
		"$fake"/mhvtl.target.wants/*.service "$tmp"/coexist.out/mhvtl.target.wants/*.service)
	# the generator wrote only into its own dir, leaving the hand-made links alone
	if [[ $rc -eq 0 && -z $out && "$(links "$tmp/coexist.out")" == "$before" &&
		$(ls "$fake/mhvtl.target.wants" | wc -l) -eq 2 && $merged -eq 11 ]]; then ok
	else fail "(c) coexistence rc=$rc merged=$merged"; echo "$out"; fi
fi

# (d) edge cases
# comments, indented continuation lines and a headerless Drive:
mkconf edge1 <<'EOF'
# Library: 77 CHANNEL: 00 TARGET: 00 LUN: 00
 Library: 78 indented, part of a record

Drive: 5 CHANNEL: 00 TARGET: 01 LUN: 00
Library: 10 CHANNEL: 00 TARGET: 00 LUN: 00
Drive: 11 CHANNEL: 00 TARGET: 01 LUN: 00
EOF
run_gen "$tmp/edge1" "$tmp/edge1.out"
want=$(printf '%s\n' "vtllibrary@10.service -> $etc/vtllibrary@.service" \
	"vtltape@11.service -> $etc/vtltape@.service" "vtltape@5.service -> $etc/vtltape@.service" | sort)
if [[ $rc -eq 0 && "$(links "$tmp/edge1.out")" == "$want" ]]; then ok
else fail "(d) comments/headerless Drive rc=$rc"; links "$tmp/edge1.out"; fi

# CRLF line endings
mkconf edge2 < <(printf 'Library: 10 CHANNEL: 00 TARGET: 00 LUN: 00\r\n\r\nDrive: 11 CHANNEL: 00 TARGET: 01 LUN: 00\r\nDrive: 12 CHANNEL: 00 TARGET: 02 LUN: 00\r\n')
run_gen "$tmp/edge2" "$tmp/edge2.out"
if [[ $rc -eq 0 && "$(links "$tmp/edge2.out" | cut -d' ' -f1 | tr '\n' ' ')" == "vtllibrary@10.service vtltape@11.service vtltape@12.service " ]]; then ok
else fail "(d) CRLF rc=$rc"; links "$tmp/edge2.out"; fi

# missing device.conf: nothing generated, non-zero exit
mkdir -p "$tmp/empty"
: >"$tmp/empty/mhvtl.conf"
run_gen "$tmp/empty" "$tmp/missing.out"
if [[ $rc -ne 0 && ! -e $tmp/missing.out/mhvtl.target.wants ]]; then ok
else fail "(d) missing device.conf rc=$rc"; fi

# zero libraries: success, empty wants dir
mkconf zero <<'EOF'
# nothing configured
EOF
run_gen "$tmp/zero" "$tmp/zero.out"
if [[ $rc -eq 0 && -d $tmp/zero.out/mhvtl.target.wants && -z "$(links "$tmp/zero.out")" ]]; then ok
else fail "(d) zero libraries rc=$rc"; fi

# id <= 0 is logged and skipped; entries after it are still generated
mkconf badid <<'EOF'
Library: 0 CHANNEL: 00 TARGET: 00 LUN: 00
Library: -3 CHANNEL: 00 TARGET: 00 LUN: 00
Drive: 0 CHANNEL: 00 TARGET: 01 LUN: 00
Library: 10 CHANNEL: 00 TARGET: 00 LUN: 00
Drive: 11 CHANNEL: 00 TARGET: 01 LUN: 00
EOF
run_gen "$tmp/badid" "$tmp/badid.out"
if [[ $rc -eq 0 && "$(links "$tmp/badid.out" | cut -d' ' -f1 | tr '\n' ' ')" == "vtllibrary@10.service vtltape@11.service " &&
	$(grep -c 'ignoring' "$tmp/log") -eq 3 ]]; then ok
else fail "(d) id <= 0 rc=$rc"; links "$tmp/badid.out"; cat "$tmp/log"; fi

echo "generator: $((checked - failed))/$checked checks passed"
[[ $failed -eq 0 ]]
