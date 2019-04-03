#!/bin/bash

if [ $# -ne 1 ]; then
	echo "Usage: $0 initiator_iqn (to add to iSCSI ACL)"
	exit
fi
# Use $1 if defined, otherwise default to this IQN iniator addr
INIT_IQN=${1:-iqn.1994-05.com.redhat:e284d58153b4}

# Reference:
# https://bugs.debian.org/cgi-bin/bugreport.cgi?bug=880576

IQN="iqn.2019-03.com.mhvtl:target"

HBA=`lsscsi -H | awk '/mhvtl/ {print $1}' | sed -e 's/\[//' -e 's/\]//'`

# Extract SCSI h:b:t:l of each dev
SCSI_ADDR=`lsscsi $HBA | awk '{print $1}'`

targetcli /iscsi/ create $IQN

# Setup LIO backing store - Walk each device, extract /dev/sg path
for dev in $SCSI_ADDR
do
 	read -r hba channel id lun <<< `echo $dev | awk -F: '{print $1,$2,$3,$4}' | sed -e 's/\[//' -e 's/\]//g'`

	# Extract the SCSI Passthru device (/dev/sg) of this h:b:t:l
	PASSTHRU=`lsscsi -g $hba $channel $id $lun | awk '{print $7}'`

	echo "hba: $hba, Channel: $channel, SCSI ID: $id, SCSI LUN: $lun - scsi passthru path: $PASSTHRU"
	MHVTL=$(printf "h%db%dt%dl%d" $hba $channel $id $lun)

	TLD="/sys/kernel/config/target/core/pscsi_0/mhVTL${MHVTL}"
	mkdir -p $TLD
	uuidgen > $TLD/wwn/vpd_unit_serial
	echo scsi_host_id=$hba,scsi_channel_id=$channel,scsi_target_id=$id,scsi_lun_id=$lun >  $TLD/control
	echo $PASSTHRU > $TLD/udev_path
	echo 1 > $TLD/enable

	# Map LIO backing store to mhVTL /dev/sgXX path
	targetcli /iscsi/$IQN/tpg1/luns/ create /backstores/pscsi/mhVTL${MHVTL}
#	targetcli ls
#	sleep 2
done

targetcli /iscsi/$IQN/tpg1/acls/ create $INIT_IQN

targetcli ls

# Open up TCP port 3260 through iptables
# CentOS7 anyway..
firewall-cmd --add-port 3260/tcp

