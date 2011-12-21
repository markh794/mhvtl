#!/bin/sh

modprobe mpt2sas
modprobe pl2303

modprobe scst
modprobe scst-vdisk
modprobe scst_tape
modprobe scst_changer

# for iSCSI
modprobe scsi_transport_iscsi
modprobe iscsi-scst
iscsi-scstd
iscsid

# for FC
modprobe qla2x00tgt

modprobe scst_user
modprobe scst_local

/opt/fast/application/fileio/fileio_tgt_gpu -o -e 1 -b 4096 gpu01 gpu -- -o -s $((1000*1024*1024*1024)) $(/opt/fast/scripts/diskmapper.sh /opt/fast/scripts/disks_shelf_2.lst) &

sleep 3

echo "add gpu01 0" > /sys/kernel/scst_tgt/targets/scst_local/scst_local_tgt/luns/mgmt

DEVICE=`lsscsi | grep gpu01 | awk '{print $6;}'`

echo "Formatting GPU Device: $DEVICE"

mkfs.ext4 $DEVICE

mount $DEVICE /mnt

mkdir -p /mnt/mhvtl-data
chown vtl:vtl /mnt/mhvtl-data
ln -s /mnt/mhvtl-data /opt/mhvtl
make_vtl_media vtl

/etc/init.d/mhvtl start

sleep 5

# create SCST config

(
cat << 'EOF'

HANDLER dev_tape {
        DEVICE 7:0:1:0
        DEVICE 7:0:2:0
}

HANDLER dev_changer {
        DEVICE 7:0:0:0
}

TARGET_DRIVER qla2x00t {
        enabled 1

        TARGET 21:00:00:24:ff:05:7b:1a {
                LUN 1 7:0:0:0
                LUN 2 7:0:1:0
                LUN 3 7:0:2:0
                enabled 1
        }
}

EOF
) > /etc/scst.conf

scstadmin -config
