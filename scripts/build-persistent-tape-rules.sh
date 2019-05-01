#!/bin/bash

# To 'view' what are valid fields we can use - Pick a tape drive you are interested in. using /dev/st0 here.

#  # udevadm info --name=/dev/st0 --query=all
#  P: /devices/pseudo_9/adapter0/host4/target4:0:1/4:0:1:0/scsi_tape/st0
#  N: st0
#  S: tape/by-id/scsi-350223344ab000100
#  E: DEVLINKS=/dev/tape/by-id/scsi-350223344ab000100
#  E: DEVNAME=/dev/st0
#  E: DEVPATH=/devices/pseudo_9/adapter0/host4/target4:0:1/4:0:1:0/scsi_tape/st0
#  E: ID_BUS=scsi
#  E: ID_MODEL=ULT3580-TD5
#  E: ID_MODEL_ENC=ULT3580-TD5\x20\x20\x20\x20\x20
#  E: ID_REVISION=0105
#  E: ID_SCSI=1
#  E: ID_SCSI_SERIAL=XYZZY_A1
#  E: ID_SERIAL=350223344ab000100
#  E: ID_SERIAL_SHORT=50223344ab000100
#  E: ID_TYPE=tape
#  E: ID_VENDOR=IBM
#  E: ID_VENDOR_ENC=IBM\x20\x20\x20\x20\x20
#  E: ID_WWN=0x50223344ab000100
#  E: ID_WWN_WITH_EXTENSION=0x50223344ab000100
#  E: MAJOR=9
#  E: MINOR=0
#  E: SUBSYSTEM=scsi_tape
#  E: USEC_INITIALIZED=421748
#  E: nodmraid=1
#
# Anything beginning with 'E' are environmental and can be used by/for 'ENV{}'

for SN in `grep ^Drive -A6 /etc/mhvtl/device.conf | awk '/NAA:/ {print $2}' | sed -e "s/://g" -e "s/^[13]0/50/g"`
do
	echo "ACTION==\"add|change\", KERNEL==\"nst*\", ENV{SUBSYSTEM}==\"scsi_tape\", ENV{ID_SERIAL_SHORT}==\"$SN\", SYMLINK+=\"tape/by-path/%E{ID_VENDOR}-%E{ID_MODEL}-%E{ID_SCSI_SERIAL}-nst\", MODE=\"0666\""
done
