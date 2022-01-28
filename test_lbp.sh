#!/bin/bash

#systemctl stop mhvtl
#systemctl start mhvtl

# Library source slot to move tape to/from
SOURCE_SLOT=1
# Which drive are we testing - /etc/mhvtl/device.conf
DRV_INDEX=18

i=`grep -A6 "^Drive: ${DRV_INDEX} " /etc/mhvtl/device.conf | awk '/Library/ {print $5}'`
# Convert into hex - leading '0' typically means it's an octal value
TARGET_DRIVE=$((16#$i-1))

read -r channel id lun <<< `grep "^Drive: ${DRV_INDEX} " /etc/mhvtl/device.conf | awk '{print $4,$6,$8}'`
#echo "Channel: $channel, id: $id, lun: $lun"

HBA=`lsscsi -H | awk '/mhvtl/ {print $1}' | sed -e 's/\[//g' -e 's/\]//g'`
SG=`lsscsi -g ${HBA} ${channel} ${id} ${lun} | awk '{print $7}'`
ST=`lsscsi -g ${HBA} ${channel} ${id} ${lun} | awk '{print $6}'`

MTX=`lsscsi -g ${HBA} 0 0 0 | awk '{print $7}'`

#echo "HBA: ${HBA}"
#echo "st : ${ST}"
#echo "sg : ${SG}"
#echo "mtx : ${MTX}"

echo "++ Moving tape slot: ${SOURCE_SLOT} to drive: ${TARGET_DRIVE}"
mtx -f ${MTX} load ${SOURCE_SLOT} ${TARGET_DRIVE}
vtlcmd ${DRV_INDEX} verbose
vtlcmd ${DRV_INDEX} verbose

echo

echo "++ Checking status of ${ST}"
mt -f ${ST} status

echo

CRC32C=2
RSCRC=1
LBP_W="40"
LBP_R="80"
LBP_RW="c0"
## Set LBP_W
#sg_wr_mode -p 0x0a,0xf0 -d -c 0a,f0,00,1c,${CRC32C},4,${LBP_W},0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 ${SG}
## Set LBP_R
#sg_wr_mode -p 0x0a,0xf0 -d -c 0a,f0,00,1c,${CRC32C},4,${LBP_R},0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 ${SG}

# Set LBP_R & LBP_W
echo -e "++ Enable LBP CRC32C RW\n"
sg_wr_mode -p 0x0a,0xf0 -d -c 0a,f0,00,1c,${CRC32C},4,${LBP_RW},0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 ${SG}

echo -e "++ Reading 64k + 4 to /tmp/CRC32c.out"
dd if=${ST} of=/tmp/CRC32c.out bs=65540 count=1

echo

echo -e "++ Enable LBP Reed-Solomon CRC RW\n"
sg_wr_mode -p 0x0a,0xf0 -d -c 0a,f0,00,1c,${RSCRC},4,${LBP_RW},0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 ${SG}

echo "++ Reading 64k + 4 to /tmp/RS-CRC.out"
dd if=${ST} of=/tmp/RS-CRC.out bs=65540 count=1

echo

# Turn off LBP
echo -e "++ Turn off LBP\n"
sg_wr_mode -p 0x0a,0xf0 -d -c 0a,f0,00,1c,0,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 ${SG}

echo "++ Reading 64k to /tmp/NO_LBP.out"
dd if=${ST} of=/tmp/NO_LBP.out bs=65540 count=1

echo

echo "Offline drive"
mt -f ${ST} offline

echo "Moving tape from drive: ${TARGET_DRIVE} to slot: ${SOURCE_SLOT}"
mtx -f ${MTX} unload ${SOURCE_SLOT} ${TARGET_DRIVE}
