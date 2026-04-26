#!/bin/bash
#
# mhvtl_stop_all.sh — Clean shutdown of all mhvtl state.
#
# Shutdown order matters:
#   1. Stop tape/library daemons (they close socket + ioctl connections)
#   2. Stop TCMU handler (cleans configfs — UIO fds released by step 1)
#   3. Stop setup daemon
#   4. Remove any remaining SCSI devices
#   5. rmmod (all references released)
#
# Usage: sudo ./mhvtl_stop_all.sh

set -euo pipefail

log() { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*"; }

if [ "$(id -u)" != "0" ]; then
    echo "error: must run as root" >&2
    exit 1
fi

# ==== Step 1: Stop tape/library daemons ====
# systemctl stop triggers ExecStop (vtlcmd exit) for each daemon.
# The daemon's clean exit path: vtlcmd exit → processMessageQ →
# VTL_REMOVE_LU (removes SCSI device) → close char dev → exit.
# This releases ALL module references. Give it enough time.
log "Step 1: Stopping tape/library daemons ..."
systemctl stop mhvtl.target 2>/dev/null || true
sleep 5

# Kill any survivors (daemons that didn't respond to vtlcmd exit)
PIDS="$(pgrep -x 'vtltape|vtllibrary' 2>/dev/null || true)"
if [ -n "$PIDS" ]; then
    log "  SIGTERM: $PIDS"
    kill $PIDS 2>/dev/null || true
    sleep 2
    PIDS="$(pgrep -x 'vtltape|vtllibrary' 2>/dev/null || true)"
    if [ -n "$PIDS" ]; then
        log "  SIGKILL: $PIDS"
        kill -9 $PIDS 2>/dev/null || true
        sleep 1
    fi
fi

# ==== Step 2: Stop TCMU handler (cleans its own configfs) ====
log "Step 2: Stopping TCMU handler ..."
systemctl stop mhvtl-tcmu-handler 2>/dev/null || true
sleep 2

PIDS="$(pgrep -x mhvtl_tcmu_handler 2>/dev/null || true)"
if [ -n "$PIDS" ]; then
    log "  SIGTERM handler: $PIDS"
    kill $PIDS 2>/dev/null || true
    sleep 2
    PIDS="$(pgrep -x mhvtl_tcmu_handler 2>/dev/null || true)"
    [ -n "$PIDS" ] && kill -9 $PIDS 2>/dev/null || true
fi

# ==== Step 3: Stop remaining services ====
log "Step 3: Stopping remaining services ..."
systemctl stop mhvtl-load-modules 2>/dev/null || true
systemctl stop mhvtl.target 2>/dev/null || true

# ==== Step 4: Remove remaining SCSI devices ====
log "Step 4: Removing SCSI devices ..."
for host_dir in /sys/class/scsi_host/host*; do
    [ -d "$host_dir" ] || continue
    proc="$(cat "$host_dir/proc_name" 2>/dev/null || true)"
    case "$proc" in
        mhvtl|tcm_loopback)
            host_num="$(basename "$host_dir" | sed 's/host//')"
            for dev in /sys/class/scsi_device/${host_num}:*; do
                [ -f "$dev/device/delete" ] || continue
                echo 1 > "$dev/device/delete" 2>/dev/null || true
            done
            ;;
    esac
done

# ==== Step 5: Clean leftover configfs (safety net) ====
CFGFS="/sys/kernel/config/target"
if [ -d "$CFGFS/core/user_0" ] && ls "$CFGFS/core/user_0"/ 2>/dev/null | grep -qv '^hba'; then
    log "Step 5: Cleaning leftover configfs ..."
    # LUN symlinks
    find "$CFGFS/loopback" -type l -delete 2>/dev/null || true
    find "$CFGFS/loopback" -mindepth 4 -maxdepth 4 -type d -name 'lun_*' \
        -exec rmdir {} \; 2>/dev/null || true
    for tpgt in "$CFGFS"/loopback/*/tpgt_*; do
        [ -d "$tpgt" ] || continue
        rmdir "$tpgt" 2>/dev/null || true
    done
    for tgt in "$CFGFS"/loopback/naa.*; do
        [ -d "$tgt" ] || continue
        rmdir "$tgt" 2>/dev/null || true
    done
    # Backstores
    for bs in "$CFGFS"/core/user_0/*/enable; do
        [ -f "$bs" ] || continue
        echo 0 > "$bs" 2>/dev/null || true
    done
    for bs in "$CFGFS"/core/user_0/*/; do
        [ -d "$bs" ] || continue
        rmdir "$bs" 2>/dev/null || true
    done
    rmdir "$CFGFS/core/user_0" 2>/dev/null || true
fi

# ==== Step 6: Unload modules ====
log "Step 6: Unloading modules ..."
# Remove LIO fabric dirs to release module references
rmdir /sys/kernel/config/target/loopback 2>/dev/null || true
sleep 1
rmmod tcm_loop 2>/dev/null || true
rmmod target_core_user 2>/dev/null || true
rmmod mhvtl 2>/dev/null || true

# ==== Step 7: Clean files ====
# Remove stale char device nodes — on reload the module may get
# a different major number, making old nodes point to nothing.
rm -f /dev/mhvtl* 2>/dev/null || true
rm -f /var/lock/mhvtl/mhvtl* 2>/dev/null || true
rm -f /var/run/mhvtl/tcmu_ready.* /var/run/mhvtl/handler_ready.* 2>/dev/null || true
rm -f /var/run/mhvtl/tcmu_handler.sock 2>/dev/null || true

systemctl reset-failed 'vtltape@*' 'vtllibrary@*' \
    'mhvtl-tcmu-handler' 'mhvtl-load-modules' 2>/dev/null || true

# ==== Verify ====
CLEAN=true
pgrep -x 'vtltape|vtllibrary|mhvtl_tcmu_handler' >/dev/null 2>&1 && CLEAN=false
lsscsi 2>/dev/null | grep -qE 'tape|mediumx' && CLEAN=false
lsmod 2>/dev/null | grep -qE '^mhvtl |^tcm_loop ' && CLEAN=false

if $CLEAN; then
    log "Clean shutdown — system is pristine"
else
    log "Shutdown complete (warnings):"
    pgrep -ax 'vtltape|vtllibrary|mhvtl_tcmu_handler' 2>/dev/null && log "  - processes remain"
    lsscsi 2>/dev/null | grep -E 'tape|mediumx' && log "  - SCSI devices remain"
    lsmod 2>/dev/null | grep -E '^mhvtl |^tcm_loop ' && log "  - modules remain"
fi
