#!/bin/bash
#
# run_backend_test.sh — Round-trip backend test.
#
# Each cycle: mhvtl integration test → TCMU integration test.
# Proves both backends work and switching is clean.
#
# Usage:
#   sudo ./run_backend_test.sh [cycles]     (default: 2)
#
# Exit 0 if all tests pass, 1 on first failure.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
INTEGRATION_TEST="$REPO_DIR/tests/integration_tape_backup_restore.sh"
CYCLES="${1:-2}"

log() { printf '\n========== [%s] %s ==========\n\n' "$(date +%H:%M:%S)" "$*"; }
fail() { printf '\n[FAIL] %s\n' "$*" >&2; exit 1; }

if [ "$(id -u)" != "0" ]; then
    fail "must run as root"
fi

[ -x "$INTEGRATION_TEST" ] || chmod +x "$INTEGRATION_TEST"
[ -f "$INTEGRATION_TEST" ] || fail "integration test not found: $INTEGRATION_TEST"

PASS=0
TOTAL=0

run_test() {
    local backend="$1"
    TOTAL=$((TOTAL + 1))

    log "TEST $TOTAL: switch to '$backend' + integration test"

    "$SCRIPT_DIR/switch_backend.sh" "$backend"

    sleep 3

    if "$INTEGRATION_TEST"; then
        PASS=$((PASS + 1))
        log "PASSED: $backend backend (test $TOTAL)"
    else
        fail "$backend backend integration test FAILED (test $TOTAL of $((CYCLES * 2)))"
    fi
}

log "Starting $CYCLES round-trip cycle(s)"

for cycle in $(seq 1 "$CYCLES"); do
    log "CYCLE $cycle of $CYCLES"
    run_test mhvtl
    run_test tcmu
done

log "ALL $PASS / $TOTAL TESTS PASSED ($CYCLES cycles)"
