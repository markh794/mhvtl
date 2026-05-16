#!/bin/bash
#
# Integration test for mhvtl: tape load -> backup -> unload -> load in another
# compatible drive -> restore -> verify md5 checksums.
#
# Intended to run as root inside a VM that has mhvtl installed and running
# (e.g. the vagrant ubuntu-test VM; the repo is mounted at /vagrant_data).
#
# Requirements: mtx, mt (mt-st), tar, md5sum, lsscsi
#
# Usage:
#   sudo ./integration_tape_backup_restore.sh
#
# Environment overrides:
#   CHANGER     - SCSI generic device of the medium changer (default: auto-detect)
#   DRIVE_A_ST  - /dev/stX of the first (backup) drive (default: auto-detect TD8 #1)
#   DRIVE_B_ST  - /dev/stX of the second (restore) drive (default: auto-detect TD8 #2)
#   SRC_SLOT    - storage element slot holding the tape to use (default: first full L8 slot)
#   DRIVE_A_DTE - Data Transfer Element index for drive A (default: 0)
#   DRIVE_B_DTE - Data Transfer Element index for drive B (default: 1)
#   TEST_FILES  - number of random test files to create (default: 5)
#   FILE_SIZE_MB- size of each test file in MB (default: 2)

set -euo pipefail

log()  { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*"; }
fail() { printf '[FAIL] %s\n' "$*" >&2; exit 1; }

if [ "$(id -u)" != "0" ]; then
    fail "must run as root"
fi

for bin in mtx mt tar md5sum lsscsi; do
    command -v "$bin" >/dev/null || fail "missing required tool: $bin"
done

# ---------- Auto-detect devices ----------
CHANGER="${CHANGER:-$(lsscsi -g | awk '/mediumx/ {print $(NF); exit}')}"
[ -n "$CHANGER" ] || fail "no medium changer found; is mhvtl.target running?"
log "medium changer: $CHANGER"

# Pick two IBM ULT3580-TD8 drives (same type => compatible media).
mapfile -t TD8_ST < <(lsscsi -g | awk '/tape .*ULT3580-TD8/ {print $(NF-1)}')
if [ "${#TD8_ST[@]}" -lt 2 ]; then
    fail "need at least two TD8 drives; found ${#TD8_ST[@]}"
fi
DRIVE_A_ST="${DRIVE_A_ST:-${TD8_ST[0]}}"
DRIVE_B_ST="${DRIVE_B_ST:-${TD8_ST[1]}}"
DRIVE_A_NST="/dev/n$(basename "$DRIVE_A_ST")"
DRIVE_B_NST="/dev/n$(basename "$DRIVE_B_ST")"
log "drive A (backup):  $DRIVE_A_ST  ($DRIVE_A_NST)"
log "drive B (restore): $DRIVE_B_ST  ($DRIVE_B_NST)"

DRIVE_A_DTE="${DRIVE_A_DTE:-0}"
DRIVE_B_DTE="${DRIVE_B_DTE:-1}"

# ---------- Figure out a starting slot ----------
STATUS="$(mtx -f "$CHANGER" status)"
echo "$STATUS" | sed -n '1,6p'

if [ -z "${SRC_SLOT:-}" ]; then
    SRC_SLOT="$(echo "$STATUS" \
        | awk '/Storage Element [0-9]+:Full.*L8/ {
                 match($0, /Storage Element ([0-9]+)/, m); print m[1]; exit }')"
fi
[ -n "$SRC_SLOT" ] || fail "could not find an L8 tape in any slot"
log "using tape from slot $SRC_SLOT"

# ---------- Ensure both drives empty at start ----------
unload_if_full() {
    local dte="$1" back_slot="$2"
    if echo "$STATUS" | grep -qE "Data Transfer Element ${dte}:Full"; then
        log "DTE $dte already has a tape; unloading to slot $back_slot"
        mtx -f "$CHANGER" unload "$back_slot" "$dte"
    fi
}
unload_if_full "$DRIVE_A_DTE" "$SRC_SLOT"
# Re-read status after possible unload.
STATUS="$(mtx -f "$CHANGER" status)"
unload_if_full "$DRIVE_B_DTE" "$SRC_SLOT"

# ---------- Prepare test data ----------
WORK="$(mktemp -d -t mhvtl-itest.XXXXXX)"
SRC_DIR="$WORK/src"
RESTORE_DIR="$WORK/restored"
mkdir -p "$SRC_DIR" "$RESTORE_DIR"
trap 'rm -rf "$WORK"' EXIT

TEST_FILES="${TEST_FILES:-5}"
FILE_SIZE_MB="${FILE_SIZE_MB:-2}"
log "generating $TEST_FILES random files of ${FILE_SIZE_MB}MB in $SRC_DIR"
for i in $(seq 1 "$TEST_FILES"); do
    dd if=/dev/urandom of="$SRC_DIR/file_${i}.bin" bs=1M count="$FILE_SIZE_MB" status=none
done
( cd "$SRC_DIR" && md5sum ./* ) > "$WORK/src.md5"
log "source checksums:"
sed 's/^/    /' "$WORK/src.md5"

# ---------- Step 1: load tape into drive A ----------
log "STEP 1: load slot $SRC_SLOT -> DTE $DRIVE_A_DTE (drive A)"
mtx -f "$CHANGER" load "$SRC_SLOT" "$DRIVE_A_DTE"

log "waiting for drive A to become ready"
for _ in $(seq 1 30); do
    if mt -f "$DRIVE_A_NST" status >/dev/null 2>&1; then break; fi
    sleep 1
done
mt -f "$DRIVE_A_NST" status >/dev/null || fail "drive A not ready"

# ---------- Step 2: backup test files via tar to tape ----------
log "STEP 2: writing tar archive to $DRIVE_A_NST"
mt -f "$DRIVE_A_NST" rewind
tar -cf "$DRIVE_A_NST" -C "$SRC_DIR" .
mt -f "$DRIVE_A_NST" weof      # write end-of-data marker
mt -f "$DRIVE_A_NST" rewind

# ---------- Step 3: unload from drive A back to source slot ----------
log "STEP 3: unload DTE $DRIVE_A_DTE -> slot $SRC_SLOT"
mt -f "$DRIVE_A_NST" rewind
# Best-effort offline; some drivers return EIO which is harmless here.
mt -f "$DRIVE_A_NST" offline 2>/dev/null || true
mtx -f "$CHANGER" unload "$SRC_SLOT" "$DRIVE_A_DTE"

# ---------- Step 4: load same tape into drive B (other compatible drive) ----------
log "STEP 4: load slot $SRC_SLOT -> DTE $DRIVE_B_DTE (drive B)"
mtx -f "$CHANGER" load "$SRC_SLOT" "$DRIVE_B_DTE"

log "waiting for drive B to become ready"
for _ in $(seq 1 30); do
    if mt -f "$DRIVE_B_NST" status >/dev/null 2>&1; then break; fi
    sleep 1
done
mt -f "$DRIVE_B_NST" status >/dev/null || fail "drive B not ready"

# ---------- Step 5: restore and compare md5 ----------
log "STEP 5: reading tar archive from $DRIVE_B_NST"
mt -f "$DRIVE_B_NST" rewind
tar -xf "$DRIVE_B_NST" -C "$RESTORE_DIR"
mt -f "$DRIVE_B_NST" rewind
mt -f "$DRIVE_B_NST" offline 2>/dev/null || true

log "unloading tape back to slot $SRC_SLOT"
mtx -f "$CHANGER" unload "$SRC_SLOT" "$DRIVE_B_DTE" || true

( cd "$RESTORE_DIR" && md5sum ./* ) > "$WORK/restored.md5"
log "restored checksums:"
sed 's/^/    /' "$WORK/restored.md5"

if diff -u "$WORK/src.md5" "$WORK/restored.md5"; then
    log "SUCCESS: all $TEST_FILES files match after tape round-trip"
    exit 0
else
    fail "md5 mismatch between source and restored files"
fi
