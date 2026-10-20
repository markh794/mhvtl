# Live hot-plug test — mhvtl-bay-monitor (Phase 2 Stage B / US-B3)

The bay monitor reacts to real block-device `add`/`remove` udev events, so it
cannot be exercised by the loopback unit tests (losetup does not emit the
`ID_PATH`-bearing events a physical/virtio-scsi disk does). This is the manual
procedure used to verify it end to end on a VM whose disks can be hot-plugged.

## Environment used

- Proxmox host `root@bigiron.lan`, guest VMID **123** (`bench-debian13`).
- A 500 GB virtio-scsi disk hot-pluggable as `scsi1`
  (`dstore01:123/vm-123-disk-5.raw`), seen in the guest at the stable path
  `/dev/disk/by-path/pci-0000:06:05.0-scsi-0:0:0:1`.
- mhvtl built in `~/mhvtl`, library `10` running (changer `/dev/sg4`,
  4 Import/Export MAP slots), home directory `/opt/mhvtl`.
- libudev-dev + pkg-config installed; `make -C usr` produced
  `usr/bin/mhvtl-bay-monitor`.

## Bay config (single bay, focused)

    # /etc/mhvtl/mhvtl_raw_bays_test.conf
    10 1 /dev/disk/by-path/pci-0000:06:05.0-scsi-0:0:0:1

The disk must already be formatted as a raw cartridge (here: barcode
`RAW099L8`, via `mhvtl-raw-format`).

## Procedure

1. **Clean baseline** — cartridge not in the library, no home symlink,
   disk detached:

       vtlcmd 10 open map; vtlcmd 10 empty map; vtlcmd 10 close map
       rm -f /opt/mhvtl/RAW099L8
       # on the host:
       qm set 123 --delete scsi1

   Confirm MAP slot Empty (`mtx -f /dev/sg4 status`) and no symlink.

2. **Start the monitor** (run from the build tree so it links the freshly
   built libvtlscsi; a transient unit detaches cleanly and logs to the
   journal):

       systemd-run --unit=baymon \
         --setenv=LD_LIBRARY_PATH=$HOME/mhvtl/usr \
         $HOME/mhvtl/usr/bin/mhvtl-bay-monitor -f -v \
         --config /etc/mhvtl/mhvtl_raw_bays_test.conf

   With the disk absent, coldplug places nothing; the journal ends with
   `monitoring 1 bay(s) for raw-disk hot-plug events`.

3. **Hot-add** the disk and watch the monitor auto-place it (no provisioner):

       qm set 123 --scsi1 dstore01:123/vm-123-disk-5.raw,discard=on,ssd=1

   Expected (`journalctl -u baymon`):

       bay .../scsi-0:0:0:1: MVTL cartridge 'RAW099L8' (library 10 MAP 1)
       library 10 reply to 'load map RAW099L8': OK
       handle_add(): placed RAW099L8 (library 10 MAP, bay .../scsi-0:0:0:1)

   Verify `/opt/mhvtl/RAW099L8 -> .../scsi-0:0:0:1` exists and the MAP slot is
   `Full :VolumeTag=RAW099L8`.

   **Format in the bay** (optional): hot-add a blank disk instead. The
   monitor logs nothing for it at add time beyond `not placed: no readable
   MVTL cartridge in bay`. Format it where it sits:

       mhvtl-raw-format /dev/disk/by-path/pci-0000:06:05.0-scsi-0:0:0:1 \
           --barcode RAW099L8

   Closing the device after the format makes udev report a `change`, and the
   monitor probes the empty bay again and places the cartridge, with the same
   `handle_add(): placed RAW099L8` line. A bay that already holds a placed
   cartridge ignores `change`, which the drive causes every time it closes
   the device.

4. **Hot-remove** the disk and watch the monitor eject it (remove uses the
   barcode the monitor stored at placement — the gone device cannot be
   re-probed):

       qm set 123 --delete scsi1

   Expected:

       library 10 reply to 'empty map': OK
       handle_remove(): removed RAW099L8 (library 10 MAP, bay .../scsi-0:0:0:1)

   Verify the `/opt/mhvtl/RAW099L8` symlink is gone and the MAP slot is `Empty`.

5. **Teardown / restore:**

       systemctl stop baymon; systemctl reset-failed baymon
       rm -f /etc/mhvtl/mhvtl_raw_bays_test.conf
       qm set 123 --scsi1 dstore01:123/vm-123-disk-5.raw,discard=on,ssd=1
       mhvtl-raw-provision --config /etc/mhvtl/mhvtl_raw_bays.conf   # re-place all bays

## Notes

- A cartridge the library already holds when the monitor starts, placed by
  the static provisioner or by the monitor before a restart, is taken over:
  coldplug logs `RAW099L8 already in library 10, tracking it for bay ...`, and
  pulling the disk afterwards removes it as in step 4. To check, restart the
  monitor (`systemctl restart baymon` for the transient unit) with the disk
  placed, then hot-remove it.
- A disk whose barcode another bay has already placed is a duplicate and is
  not placed: `bay ... not placed: RAW099L8 is already placed from bay ...`.
- Production deployment uses the installed unit
  `etc/mhvtl-bay-monitor.service` (generated from `.service.in`, started after
  `mhvtl.target`); the build-tree + `systemd-run` form above is only to test an
  uninstalled binary against the uninstalled libvtlscsi.
