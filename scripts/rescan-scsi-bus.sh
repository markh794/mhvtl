#!/bin/bash
# Skript to rescan SCSI bus, using the
# scsi add-single-device mechanism
# (c) 1998--2008 Kurt Garloff <kurt@garloff.de>, GNU GPL v2 or later
# (c) 2006--2008 Hannes Reinecke, GNU GPL v2 or later
# $Id: rescan-scsi-bus.sh,v 1.48 2010/08/10 19:32:22 garloff Exp $

setcolor ()
{
  red="\e[0;31m"
  green="\e[0;32m"
  yellow="\e[0;33m"
  bold="\e[0;1m"
  norm="\e[0;0m"
}

unsetcolor ()
{
  red=""; green=""
  yellow=""; norm=""
}

# Output some text and return cursor to previous position
# (only works for simple strings)
# Stores length of string in LN and returns it
print_and_scroll_back ()
{
  STRG="$1"
  LN=${#STRG}
  BK=""
  declare -i cntr=0
  while test $cntr -lt $LN; do BK="$BK\e[D"; let cntr+=1; done
  echo -en "$STRG$BK"
  return $LN
}

# Overwrite a text of length $1 (fallback to $LN) with whitespace
white_out ()
{
  BK=""; WH=""
  if test -n "$1"; then LN=$1; fi
  declare -i cntr=0
  while test $cntr -lt $LN; do BK="$BK\e[D"; WH="$WH "; let cntr+=1; done
  echo -en "$WH$BK"
}

# Return hosts. sysfs must be mounted
findhosts_26 ()
{
  hosts=
  for hostdir in /sys/class/scsi_host/host*; do
    hostno=${hostdir#/sys/class/scsi_host/host}
    if [ -f $hostdir/isp_name ] ; then
      hostname="qla2xxx"
    elif [ -f $hostdir/lpfc_drvr_version ] ; then
      hostname="lpfc"
    else
      hostname=`cat $hostdir/proc_name`
    fi
    hosts="$hosts $hostno"
    echo "Host adapter $hostno ($hostname) found."
  done
  if [ -z "$hosts" ] ; then
    echo "No SCSI host adapters found in sysfs"
    exit 1;
  fi
  hosts=`echo $hosts | sed 's/ /\n/g' | sort -n`
}

# Return hosts. /proc/scsi/HOSTADAPTER/? must exist
findhosts ()
{
  hosts=
  for driverdir in /proc/scsi/*; do
    driver=${driverdir#/proc/scsi/}
    if test $driver = scsi -o $driver = sg -o $driver = dummy -o $driver = device_info; then continue; fi
    for hostdir in $driverdir/*; do
      name=${hostdir#/proc/scsi/*/}
      if test $name = add_map -o $name = map -o $name = mod_parm; then continue; fi
      num=$name
      driverinfo=$driver
      if test -r $hostdir/status; then
	num=$(printf '%d\n' `sed -n 's/SCSI host number://p' $hostdir/status`)
	driverinfo="$driver:$name"
      fi
      hosts="$hosts $num"
      echo "Host adapter $num ($driverinfo) found."
    done
  done
}

printtype ()
{
  local type=$1

  case "$type" in
    0) echo "Direct-Access    " ;;
    1) echo "Sequential-Access" ;;
    2) echo "Printer          " ;;
    3) echo "Processor        " ;;
    4) echo "WORM             " ;;
    5) echo "CD-ROM           " ;;
    6) echo "Scanner          " ;;
    7) echo "Optical Device   " ;;
    8) echo "Medium Changer   " ;;
    9) echo "Communications   " ;;
    10) echo "Unknown          " ;;
    11) echo "Unknown          " ;;
    12) echo "RAID             " ;;
    13) echo "Enclosure        " ;;
    14) echo "Direct-Access-RBC" ;;
    *) echo "Unknown          " ;;
  esac
}

print02i()
{
    if [ "$1" = "*" ] ; then
        echo "00"
    else
        printf "%02i" "$1"
    fi
}

# Get /proc/scsi/scsi info for device $host:$channel:$id:$lun
# Optional parameter: Number of lines after first (default = 2),
# result in SCSISTR, return code 1 means empty.
procscsiscsi ()
{
  if test -z "$1"; then LN=2; else LN=$1; fi
  CHANNEL=`print02i "$channel"`
  ID=`print02i "$id"`
  LUN=`print02i "$lun"`
  if [ -d /sys/class/scsi_device ]; then
    SCSIPATH="/sys/class/scsi_device/${host}:${channel}:${id}:${lun}"
    if [ -d  "$SCSIPATH" ] ; then
      SCSISTR="Host: scsi${host} Channel: $CHANNEL Id: $ID Lun: $LUN"
      if [ "$LN" -gt 0 ] ; then
	IVEND=$(cat ${SCSIPATH}/device/vendor)
	IPROD=$(cat ${SCSIPATH}/device/model)
	IPREV=$(cat ${SCSIPATH}/device/rev)
	SCSIDEV=$(printf '  Vendor: %-08s Model: %-16s Rev: %-4s' "$IVEND" "$IPROD" "$IPREV")
	SCSISTR="$SCSISTR
$SCSIDEV"
      fi
      if [ "$LN" -gt 1 ] ; then
	ILVL=$(cat ${SCSIPATH}/device/scsi_level)
	type=$(cat ${SCSIPATH}/device/type)
	ITYPE=$(printtype $type)
	SCSITMP=$(printf '  Type:   %-16s                ANSI SCSI revision: %02d' "$ITYPE" "$((ILVL - 1))")
	SCSISTR="$SCSISTR
$SCSITMP"
      fi
    else
      return 1
    fi
  else
    grepstr="scsi$host Channel: $CHANNEL Id: $ID Lun: $LUN"
    SCSISTR=`cat /proc/scsi/scsi | grep -A$LN -e"$grepstr"`
  fi
  if test -z "$SCSISTR"; then return 1; else return 0; fi
}

# Find sg device with 2.6 sysfs support
sgdevice26 ()
{
  if test -e /sys/class/scsi_device/$host\:$channel\:$id\:$lun/device/generic; then
    SGDEV=`readlink /sys/class/scsi_device/$host\:$channel\:$id\:$lun/device/generic`
    SGDEV=`basename $SGDEV`
  else
    for SGDEV in /sys/class/scsi_generic/sg*; do
      DEV=`readlink $SGDEV/device`
      if test "${DEV##*/}" = "$host:$channel:$id:$lun"; then
	SGDEV=`basename $SGDEV`; return
      fi
    done
    SGDEV=""
  fi
}

# Find sg device with 2.4 report-devs extensions
sgdevice24 ()
{
  if procscsiscsi 3; then
    SGDEV=`echo "$SCSISTR" | grep 'Attached drivers:' | sed 's/^ *Attached drivers: \(sg[0-9]*\).*/\1/'`
  fi
}

# Find sg device that belongs to SCSI device $host $channel $id $lun
# and return in SGDEV
sgdevice ()
{
  SGDEV=
  if test -d /sys/class/scsi_device; then
    sgdevice26
  else
    DRV=`grep 'Attached drivers:' /proc/scsi/scsi 2>/dev/null`
    repdevstat=$((1-$?))
    if [ $repdevstat = 0 ]; then
      echo "scsi report-devs 1" >/proc/scsi/scsi
      DRV=`grep 'Attached drivers:' /proc/scsi/scsi 2>/dev/null`
      if [ $? = 1 ]; then return; fi
    fi
    if ! `echo $DRV | grep 'drivers: sg' >/dev/null`; then
      modprobe sg
    fi
    sgdevice24
    if [ $repdevstat = 0 ]; then
      echo "scsi report-devs 0" >/proc/scsi/scsi
    fi
  fi
}

# Test if SCSI device is still responding to commands
testonline ()
{
  : testonline
  RC=0
  if test ! -x /usr/bin/sg_turs; then return 0; fi
  sgdevice
  if test -z "$SGDEV"; then return 0; fi
  sg_turs /dev/$SGDEV >/dev/null 2>&1
  RC=$?
  # Handle in progress of becoming ready and unit attention -- wait at max 11s
  declare -i ctr=0
  if test $RC = 2 -o $RC = 6; then
    RMB=`sg_inq /dev/$SGDEV | grep 'RMB=' | sed 's/^.*RMB=\(.\).*$/\1/'`
    print_and_scroll_back "$host:$channel:$id:$lun $SGDEV ($RMB) "
  fi
  while test $RC = 2 -o $RC = 6 && test $ctr -le 8; do
    if test $RC = 2 -a "$RMB" != "1"; then echo -n "."; let $LN+=1; sleep 1
    else usleep 20000; fi
    let ctr+=1
    sg_turs /dev/$SGDEV >/dev/null 2>&1
    RC=$?
  done
  if test $ctr != 0; then white_out; fi
  # echo -e "\e[A\e[A\e[A${yellow}Test existence of $SGDEV = $RC ${norm} \n\n\n"
  if test $RC = 1; then return $RC; fi
  # Reset RC (might be !=0 for passive paths)
  RC=0
  # OK, device online, compare INQUIRY string
  INQ=`sg_inq $sg_len_arg /dev/$SGDEV 2>/dev/null`
  IVEND=`echo "$INQ" | grep 'Vendor identification:' | sed 's/^[^:]*: \(.*\)$/\1/'`
  IPROD=`echo "$INQ" | grep 'Product identification:' | sed 's/^[^:]*: \(.*\)$/\1/'`
  IPREV=`echo "$INQ" | grep 'Product revision level:' | sed 's/^[^:]*: \(.*\)$/\1/'`
  STR=`printf "  Vendor: %-08s Model: %-16s Rev: %-4s" "$IVEND" "$IPROD" "$IPREV"`
  IPTYPE=`echo "$INQ" | sed -n 's/.* Device_type=\([0-9]*\) .*/\1/p'`
  IPQUAL=`echo "$INQ" | sed -n 's/ *PQual=\([0-9]*\)  Device.*/\1/p'`
  if [ "$IPQUAL" != 0 ] ; then
    echo -e "\e[A\e[A\e[A\e[A${red}$SGDEV changed: ${bold}LU not available (PQual $IPQUAL)${norm}    \n\n\n"
    return 2
  fi

  TYPE=$(printtype $IPTYPE)
  procscsiscsi
  TMPSTR=`echo "$SCSISTR" | grep 'Vendor:'`
  if [ "$TMPSTR" != "$STR" ]; then
    echo -e "\e[A\e[A\e[A\e[A${red}$SGDEV changed: ${bold}\nfrom:${SCSISTR#* } \nto: $STR ${norm} \n\n\n"
    return 1
  fi
  TMPSTR=`echo "$SCSISTR" | sed -n 's/.*Type: *\(.*\) *ANSI.*/\1/p'`
  if [ $TMPSTR != $TYPE ] ; then
    echo -e "\e[A\e[A\e[A\e[A${red}$SGDEV changed: ${bold}\nfrom:${TMPSTR} \nto: $TYPE ${norm} \n\n\n"
    return 1
  fi
  return $RC
}

# Test if SCSI device $host $channen $id $lun exists
# Outputs description from /proc/scsi/scsi (unless arg passed)
# Returns SCSISTR (empty if no dev)
testexist ()
{
  : testexist
  SCSISTR=
  if procscsiscsi && test -z "$1"; then
    echo "$SCSISTR" | head -n1
    echo "$SCSISTR" | tail -n2 | pr -o4 -l1
  fi
}

# Returns the list of existing channels per host
chanlist ()
{
  local hcil
  local cil
  local chan
  local tmpchan

  for dev in /sys/class/scsi_device/${host}:* ; do
    [ -d $dev ] || continue;
    hcil=${dev##*/}
    cil=${hcil#*:}
    chan=${cil%%:*}
    for tmpchan in $channelsearch ; do
      if test "$chan" -eq $tmpchan ; then
	chan=
      fi
    done
    if test -n "$chan" ; then
      channelsearch="$channelsearch $chan"
    fi
  done
  if test -z "$channelsearch"; then channelsearch="0"; fi
}

# Returns the list of existing targets per host
idlist ()
{
  local hcil
  local cil
  local il
  local target
  local tmpid

  for dev in /sys/class/scsi_device/${host}:${channel}:* ; do
    [ -d $dev ] || continue;
    hcil=${dev##*/}
    cil=${hcil#*:}
    il=${cil#*:}
    target=${il%%:*}
    for tmpid in $idsearch ; do
      if test "$target" -eq $tmpid ; then
	target=
	break
      fi
    done
    if test -n "$target" ; then
      idsearch="$idsearch $target"
    fi
  done
}

# Returns the list of existing LUNs from device $host $channel $id $lun
# and returns list to stdout
getluns()
{
  sgdevice
  if test -z "$SGDEV"; then return; fi
  if test ! -x /usr/bin/sg_luns; then echo 0; return; fi
  LLUN=`sg_luns -d /dev/$SGDEV 2>/dev/null`
  if test $? != 0; then echo 0; return; fi
  echo "$LLUN" | sed -n 's/.*lun=\(.*\)/\1/p'
}

# Wait for udev to settle (create device nodes etc.)
udevadm_settle()
{
  if test -x /sbin/udevadm; then
    print_and_scroll_back " Calling udevadm settle (can take a while) "
    /sbin/udevadm settle
    white_out
  else
    usleep 20000
  fi
}

# Perform scan on a single lun $host $channel $id $lun
dolunscan()
{
  SCSISTR=
  devnr="$host $channel $id $lun"
  echo "Scanning for device $devnr ... "
  printf "${yellow}OLD: $norm"
  testexist
  # Special case: lun 0 just got added (for reportlunscan),
  # so make sure we correctly treat it as new
  if test "$lun" = "0" -a "$1"; then
    SCSISTR=""
    printf "\r\e[A\e[A\e[A"
  fi
  : f $remove s $SCSISTR
  if test "$remove" -a "$SCSISTR"; then
    # Device exists: Test whether it's still online
    # (testonline returns 1 if it's gone or has changed)
    testonline
    RC=$?
    if test $RC != 0 -o ! -z "$forceremove"; then
      echo -en "\r\e[A\e[A\e[A${red}REM: "
      echo "$SCSISTR" | head -n1
      echo -e "${norm}\e[B\e[B"
      if test -e /sys/class/scsi_device/${host}:${channel}:${id}:${lun}/device; then
        echo 1 > /sys/class/scsi_device/${host}:${channel}:${id}:${lun}/device/delete
	if test $RC -eq 1 -o $lun -eq 0 ; then
          # Try readding, should fail if device is gone
          echo "$channel $id $lun" > /sys/class/scsi_host/host${host}/scan
	fi
	# FIXME: Can we skip udevadm settle for removal?
	#udevadm_settle
	usleep 20000
      else
        echo "scsi remove-single-device $devnr" > /proc/scsi/scsi
	if test $RC -eq 1 -o $lun -eq 0 ; then
          # Try readding, should fail if device is gone
          echo "scsi add-single-device $devnr" > /proc/scsi/scsi
	fi
      fi
    fi
    if test $RC = 0 -o "$forcerescan" ; then
      if test -e /sys/class/scsi_device/${host}:${channel}:${id}:${lun}/device; then
        echo 1 > /sys/class/scsi_device/${host}:${channel}:${id}:${lun}/device/rescan
	udevadm_settle
      fi
    fi
    printf "\r\e[A\e[A\e[A${yellow}OLD: $norm"
    testexist
    if test -z "$SCSISTR"; then
      printf "\r${red}DEL: $norm\r\n\n"
      let rmvd+=1;
      return 1
    fi
  fi
  if test -z "$SCSISTR"; then
    # Device does not exist, try to add
    printf "\r${green}NEW: $norm"
    if test -e /sys/class/scsi_host/host${host}/scan; then
      echo "$channel $id $lun" > /sys/class/scsi_host/host${host}/scan 2> /dev/null
      udevadm_settle
    else
      echo "scsi add-single-device $devnr" > /proc/scsi/scsi
    fi
    testexist
    if test -z "$SCSISTR"; then
      # Device not present
      printf "\r\e[A";
      # Optimization: if lun==0, stop here (only if in non-remove mode)
      if test $lun = 0 -a -z "$remove" -a $optscan = 1; then
        break;
      fi
    else
      let found+=1;
    fi
  fi
}

# Perform report lun scan on $host $channel $id using REPORT_LUNS
doreportlun()
{
  lun=0
  SCSISTR=
  devnr="$host $channel $id $lun"
  echo -en "Scanning for device $devnr ...\r"
  lun0added=
  #printf "${yellow}OLD: $norm"
  # Phase one: If LUN0 does not exist, try to add
  testexist -q
  if test -z "$SCSISTR"; then
    # Device does not exist, try to add
    #printf "\r${green}NEW: $norm"
    if test -e /sys/class/scsi_host/host${host}/scan; then
      echo "$channel $id $lun" > /sys/class/scsi_host/host${host}/scan 2> /dev/null
      udevadm_settle
    else
      echo "scsi add-single-device $devnr" > /proc/scsi/scsi
    fi
    testexist -q
    if test -n "$SCSISTR"; then
      lun0added=1
      #testonline
    else
      # Device not present
      # return
      # Find alternative LUN to send getluns to
      for dev in /sys/class/scsi_device/${host}:${channel}:${id}:*; do
        [ -d "$dev" ] || continue
	lun=${dev##*:}
        break
      done
    fi
  fi
  targetluns=`getluns`
  lunremove=
  #echo "getluns reports " $targetluns
  # Check existing luns
  for dev in /sys/class/scsi_device/${host}:${channel}:${id}:*; do
    [ -d "$dev" ] || continue
    lun=${dev##*:}
    newsearch=
    inlist=
    # OK, is existing $lun (still) in reported list
    for tmplun in $targetluns; do
      if test $tmplun -eq $lun ; then
	inlist=1
	dolunscan $lun0added
      else
	newsearch="$newsearch $tmplun"
      fi
    done
    # OK, we have now done a lunscan on $lun and
    # $newsearch is the old $targetluns without $lun
    if [ -z "$inlist" ]; then
      # Stale lun
      lunremove="$lunremove $lun"
    fi
    # $lun removed from $lunsearch (echo for whitespace cleanup)
    targetluns=`echo $newsearch`
  done
  # Add new ones and check stale ones
  for lun in $targetluns $lunremove; do
    dolunscan $lun0added
  done
}

# Perform search (scan $host)
dosearch ()
{
  if test -z "$channelsearch" ; then
    chanlist
  fi
  for channel in $channelsearch; do
    if test -z "$idsearch" ; then
      idlist
    fi
    for id in $idsearch; do
      if test -z "$lunsearch" ; then
	doreportlun
      else
	for lun in $lunsearch; do
          dolunscan
        done
      fi
    done
  done
}

expandlist ()
{
  list=$1
  result=""
  first=${list%%,*}
  rest=${list#*,}
  while test ! -z "$first"; do
    beg=${first%%-*};
    if test "$beg" = "$first"; then
      result="$result $beg";
    else
      end=${first#*-}
      result="$result `seq $beg $end`"
    fi
    test "$rest" = "$first" && rest=""
    first=${rest%%,*}
    rest=${rest#*,}
  done
  echo $result
}

# main
if test @$1 = @--help -o @$1 = @-h -o @$1 = @-?; then
    echo "Usage: rescan-scsi-bus.sh [options] [host [host ...]]"
    echo "Options:"
    echo " -l      activates scanning for LUNs 0--7   [default: 0]"
    echo " -L NUM  activates scanning for LUNs 0--NUM [default: 0]"
    echo " -w      scan for target device IDs 0--15   [default: 0--7]"
    echo " -c      enables scanning of channels 0 1   [default: 0 / all detected ones]"
    echo " -r      enables removing of devices        [default: disabled]"
    echo " -i      issue a FibreChannel LIP reset     [default: disabled]"
    echo "--remove:        same as -r"
    echo "--issue-lip:     same as -i"
    echo "--forcerescan:   Rescan existing devices"
    echo "--forceremove:   Remove and readd every device (DANGEROUS)"
    echo "--nooptscan:     don't stop looking for LUNs is 0 is not found"
    echo "--color:         use coloured prefixes OLD/NEW/DEL"
    echo "--hosts=LIST:    Scan only host(s) in LIST"
    echo "--channels=LIST: Scan only channel(s) in LIST"
    echo "--ids=LIST:      Scan only target ID(s) in LIST"
    echo "--luns=LIST:     Scan only lun(s) in LIST"
    echo "--sync/nosync:   Issue a sync / no sync [default: sync if remove]"
    echo "--attachpq3:     Tell kernel to attach sg to LUN 0 that reports PQ=3"
    echo "--reportlun2:    Tell kernel to try REPORT_LUN even on SCSI2 devices"
    echo "--largelun:      Tell kernel to support LUNs > 7 even on SCSI2 devs"
    echo "--sparselun:     Tell kernel to support sparse LUN numbering"
    echo " Host numbers may thus be specified either directly on cmd line (deprecated) or"
    echo " or with the --hosts=LIST parameter (recommended)."
    echo "LIST: A[-B][,C[-D]]... is a comma separated list of single values and ranges"
    echo " (No spaces allowed.)"
    exit 0
fi

if test ! -d /sys/class/scsi_host/ -a ! -d /proc/scsi/; then
  echo "Error: SCSI subsystem not active"
  exit 1
fi

# Make sure sg is there
modprobe sg >/dev/null 2>&1

if test -x /usr/bin/sg_inq; then
    sg_version=$(sg_inq -V 2>&1 | cut -d " " -f 3)
    sg_version=${sg_version##0.}
    #echo "\"$sg_version\""
    if [ -z "$sg_version" -o "$sg_version" -lt 70 ] ; then
        sg_len_arg="-36"
    else
        sg_len_arg="--len=36"
    fi
fi

# defaults
unsetcolor
lunsearch=
opt_idsearch=`seq 0 7`
opt_channelsearch=
remove=
forceremove=
optscan=1
sync=1
declare -i scan_flags=0
if test -d /sys/class/scsi_host; then
  findhosts_26
else
  findhosts
fi

# Scan options
opt="$1"
while test ! -z "$opt" -a -z "${opt##-*}"; do
  opt=${opt#-}
  case "$opt" in
    l) lunsearch=`seq 0 7` ;;
    L) lunsearch=`seq 0 $2`; shift ;;
    w) opt_idsearch=`seq 0 15` ;;
    c) opt_channelsearch="0 1" ;;
    r) remove=1 ;;
    i) lipreset=1 ;;
    -remove)      remove=1 ;;
    -forcerescan) remove=1; forcerescan=1 ;;
    -forceremove) remove=1; forceremove=1 ;;
    -hosts=*)     arg=${opt#-hosts=};   hosts=`expandlist $arg` ;;
    -channels=*)  arg=${opt#-channels=};opt_channelsearch=`expandlist $arg` ;;
    -ids=*)   arg=${opt#-ids=};         opt_idsearch=`expandlist $arg` ;;
    -luns=*)  arg=${opt#-luns=};        lunsearch=`expandlist $arg` ;;
    -color) setcolor ;;
    -nooptscan) optscan=0 ;;
    -issue-lip) lipreset=1 ;;
    -sync) sync=2 ;;
    -nosync) sync=0 ;;
    -attachpq3) scan_flags=$(($scan_flags|0x1000000)) ;;
    -reportlun2) scan_flags=$(($scan_flags|0x20000)) ;;
    -largelun) scan_flags=$(($scan_flags|0x200)) ;;
    -sparselun) scan_flags=$((scan_flags|0x40)) ;;
    *) echo "Unknown option -$opt !" ;;
  esac
  shift
  opt="$1"
done

# Hosts given ?
if test "@$1" != "@"; then
  hosts=$*
fi

if [ -d /sys/class/scsi_host -a ! -w /sys/class/scsi_host ]; then
  echo "You need to run scsi-rescan-bus.sh as root"
  exit 2
fi
if test "$sync" = 1 -a "$remove" = 1; then sync=2; fi
if test "$sync" = 2; then echo "Syncing file systems"; sync; fi
if test -w /sys/module/scsi_mod/parameters/default_dev_flags -a $scan_flags != 0; then
  OLD_SCANFLAGS=`cat /sys/module/scsi_mod/parameters/default_dev_flags`
  NEW_SCANFLAGS=$(($OLD_SCANFLAGS|$scan_flags))
  if test "$OLD_SCANFLAGS" != "$NEW_SCANFLAGS"; then
    echo -n "Temporarily setting kernel scanning flags from "
    printf "0x%08x to 0x%08x\n" $OLD_SCANFLAGS $NEW_SCANFLAGS
    echo $NEW_SCANFLAGS > /sys/module/scsi_mod/parameters/default_dev_flags
  else
    unset OLD_SCANFLAGS
  fi
fi
echo "Scanning SCSI subsystem for new devices"
test -z "$remove" || echo " and remove devices that have disappeared"
declare -i found=0
declare -i rmvd=0
for host in $hosts; do
  echo -n "Scanning host $host "
  if test -e /sys/class/fc_host/host$host ; then
    # It's pointless to do a target scan on FC
    if test -n "$lipreset" ; then
      echo 1 > /sys/class/fc_host/host$host/issue_lip 2> /dev/null;
    fi
    # Always trigger a rescan for FC to update channels and targets
    echo "- - -" > /sys/class/scsi_host/host$host/scan 2> /dev/null;
    channelsearch=
    idsearch=
    udevadm_settle
  else
    channelsearch=$opt_channelsearch
    idsearch=$opt_idsearch
  fi
  [ -n "$channelsearch" ] && echo -n "channels $channelsearch "
  echo -n "for "
  if [ -n "$idsearch" ] ; then
    echo -n " SCSI target IDs " $idsearch
  else
    echo -n " all SCSI target IDs"
  fi
  if [ -n "$lunsearch" ] ; then
    echo ", LUNs " $lunsearch
  else
    echo ", all LUNs"
  fi
  dosearch
done
if test -n "$OLD_SCANFLAGS"; then
  echo $OLD_SCANFLAGS > /sys/module/scsi_mod/parameters/default_dev_flags
fi
echo "$found new device(s) found.               "
echo "$rmvd device(s) removed.                 "
