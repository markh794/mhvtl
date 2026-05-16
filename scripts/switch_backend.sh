#!/bin/bash
#
# switch_backend.sh — Switch mhvtl between kernel module and TCMU backends.
#
# Cleanly stops the current backend, verifies pristine state,
# switches to the requested backend, and verifies devices appear.
#
# Usage:
#   sudo ./switch_backend.sh mhvtl
#   sudo ./switch_backend.sh tcmu

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
CONF_PATH="${MHVTL_CONFIG_PATH:-/etc/mhvtl}"

log()  { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*"; }
fail() { printf '[FAIL] %s\n' "$*" >&2; exit 1; }

if [ "$(id -u)" != "0" ]; then
    fail "must run as root"
fi

TARGET="${1:-}"
if [ "$TARGET" != "mhvtl" ] && [ "$TARGET" != "tcmu" ]; then
    echo "Usage: $0 <mhvtl|tcmu>" >&2
    exit 1
fi

# ==== Phase 1: Clean shutdown ====
log "Phase 1: Clean shutdown ..."
"$SCRIPT_DIR/mhvtl_stop_all.sh"

# Verify no mhvtl SCSI devices remain (modules may stay loaded — that's OK)
if lsscsi 2>/dev/null | grep -qE 'tape|mediumx'; then
    log "WARNING: stale SCSI devices, retrying cleanup ..."
    "$SCRIPT_DIR/mhvtl_stop_all.sh"
    if lsscsi 2>/dev/null | grep -qE 'tape|mediumx'; then
        fail "SCSI devices still present after double cleanup"
    fi
fi

# ==== Phase 2: Configure backend ====
log "Phase 2: Setting backend to '$TARGET' ..."
sed -i '/^MHVTL_BACKEND=/d' "$CONF_PATH/mhvtl.conf" 2>/dev/null || true
echo "MHVTL_BACKEND=$TARGET" >> "$CONF_PATH/mhvtl.conf"

if [ "$TARGET" = "tcmu" ]; then
    systemctl enable mhvtl-tcmu-handler.service 2>/dev/null || true
else
    systemctl disable mhvtl-tcmu-handler.service 2>/dev/null || true
fi

# ==== Phase 3: Start ====
log "Phase 3: Starting mhvtl.target ..."
systemctl daemon-reload
systemctl start mhvtl.target

# ==== Phase 4: Wait for devices ====
log "Phase 4: Waiting for devices ..."
for i in $(seq 1 60); do
    TAPE_COUNT="$(lsscsi 2>/dev/null | grep -c 'tape' || true)"
    CHANGER_COUNT="$(lsscsi 2>/dev/null | grep -c 'mediumx' || true)"
    if [ "$TAPE_COUNT" -gt 0 ] && [ "$CHANGER_COUNT" -gt 0 ]; then
        break
    fi
    sleep 1
done

TAPE_COUNT="$(lsscsi 2>/dev/null | grep -c 'tape' || true)"
CHANGER_COUNT="$(lsscsi 2>/dev/null | grep -c 'mediumx' || true)"

if [ "$TAPE_COUNT" -eq 0 ] && [ "$CHANGER_COUNT" -eq 0 ]; then
    fail "No SCSI devices appeared after 60 seconds"
fi

DAEMON_COUNT="$(pgrep -cx 'vtltape|vtllibrary' 2>/dev/null || echo 0)"

log "Backend '$TARGET' active: $TAPE_COUNT tapes, $CHANGER_COUNT changers, $DAEMON_COUNT daemons"
lsscsi -g 2>/dev/null || lsscsi 2>/dev/null
