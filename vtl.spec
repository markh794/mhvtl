Summary: Virtual tape library. kernel pseudo HBA driver + userspace daemons
Name: vtl
Version: 0.12
Release: 26
Source: vtl-2007-09-26.tgz
License: GPL
Group: System/Kernel
BuildRoot: /var/tmp/%{name}-buildroot
URL: http://sydsup.veritas.com/~mharvey/

%description
A Virtual tape library and tape drives:

Used to emulate hardware robot & tape drives.

VTL consists of a pseudo HBA kernel driver which reports it has
8 SSC targets (tape drives) and 1 SMC target (robot) attached.

User-space daemon(s) act as the target devices. Communication between
the kernel module and the daemons is achieved via /dev/vtl? device nodes.

The kernel module is based on the scsi_debug driver.
The SSC/SMC target daemons have been written from scratch.

Note: Currently, the kernel module needs to be built separately. For
      instructions install src.rpm and read the INSTALL file.

%prep
%setup

%build
make RPM_OPT_FLAGS="$RPM_OPT_FLAGS"

%install
[ "%{buildroot}" != "/" ] && rm -rf %[buildroot}

mkdir -p $RPM_BUILD_ROOT/etc/init.d
mkdir -p $RPM_BUILD_ROOT/etc/vtl
mkdir -p $RPM_BUILD_ROOT/usr/bin
mkdir -p $RPM_BUILD_ROOT/usr/share/man/man1
mkdir -p $RPM_BUILD_ROOT/usr/share/man/man5

%ifarch x86_64 amd64
mkdir -p $RPM_BUILD_ROOT/usr/lib64
%else
mkdir -p $RPM_BUILD_ROOT/usr/lib
%endif

install -m 750 vtl $RPM_BUILD_ROOT/etc/init.d/vtl
install -m 750 -s vtltape $RPM_BUILD_ROOT/usr/bin/vtltape
install -m 750 -s vtllibrary $RPM_BUILD_ROOT/usr/bin/vtllibrary
install -m 750 vtlcmd $RPM_BUILD_ROOT/usr/bin/vtlcmd
install -m 750 mktape $RPM_BUILD_ROOT/usr/bin/mktape
install -m 700 build_library_config $RPM_BUILD_ROOT/usr/bin/build_library_config
install -m 700 make_vtl_devices $RPM_BUILD_ROOT/usr/bin/make_vtl_devices

%ifarch x86_64 amd64
install -m 755 libvtlscsi.so $RPM_BUILD_ROOT/usr/lib64/libvtlscsi.so
%else
install -m 755 libvtlscsi.so $RPM_BUILD_ROOT/usr/lib/libvtlscsi.so
%endif

install -m 644 man/build_library_config.1 $RPM_BUILD_ROOT/usr/share/man/man1/build_library_config.1
install -m 644 man/make_vtl_devices.1 $RPM_BUILD_ROOT/usr/share/man/man1/make_vtl_devices.1
install -m 644 man/mktape.1 $RPM_BUILD_ROOT/usr/share/man/man1/mktape.1
install -m 644 man/vtl.1 $RPM_BUILD_ROOT/usr/share/man/man1/vtl.1
install -m 644 man/vtlcmd.1 $RPM_BUILD_ROOT/usr/share/man/man1/vtlcmd.1
install -m 644 man/vtllibrary.1 $RPM_BUILD_ROOT/usr/share/man/man1/vtllibrary.1
install -m 644 man/vtltape.1 $RPM_BUILD_ROOT/usr/share/man/man1/vtltape.1
install -m 644 man/library_contents.5 $RPM_BUILD_ROOT/usr/share/man/man5/library_contents.5

%pre
if ! getent group vtl > /dev/null 2>&1; then
 if [ -f /etc/SuSE-release ]; then
   groupadd --system vtl
 else
   groupadd vtl
 fi
fi
if ! getent passwd vtl > /dev/null 2>&1; then
 if [ -f /etc/SuSE-release ]; then
   useradd --system -g vtl -c "VTL daemon" -s /bin/bash vtl
 else
   useradd -g vtl -c "VTL daemon" -s /bin/bash vtl
 fi
fi
if [ -x /etc/init.d/vtl ]; then
 /etc/init.d/vtl shutdown
fi

%post
/sbin/ldconfig
r=`/sbin/chkconfig --list|grep vtl|awk '{print $1}'`
if [ "X"$r == "X" ]; then
	/sbin/chkconfig --add vtl
fi

if [ ! -d /opt/vtl ]; then
	mkdir /opt/vtl
fi
chown vtl:vtl /opt/vtl
chmod 770 /opt/vtl

%preun
if [ -x /etc/init.d/vtl ]; then
 /etc/init.d/vtl shutdown
fi

%postun
/sbin/ldconfig
userdel vtl
groupdel vtl

%clean
if [ "$RPM_BUILD_ROOT" == "/" ];then
 echo "Attempt to remove / failed! - Fix RPM_BUILD_ROOT macro"
else
 rm -rf $RPM_BUILD_ROOT
fi

%files
%defattr(-,vtl,vtl)
%doc INSTALL README library_contents.sample
/etc/init.d/vtl
%{_prefix}/bin/vtlcmd
%{_prefix}/bin/vtltape
%{_prefix}/bin/vtllibrary
%{_prefix}/bin/mktape
%{_prefix}/bin/build_library_config
%{_prefix}/bin/make_vtl_devices
%ifarch x86_64 amd64
%{_prefix}/lib64/libvtlscsi.so
%else
%{_prefix}/lib/libvtlscsi.so
%endif
%doc %{_prefix}/share/man/man1/build_library_config.1.gz
%doc %{_prefix}/share/man/man1/make_vtl_devices.1.gz
%doc %{_prefix}/share/man/man1/mktape.1.gz
%doc %{_prefix}/share/man/man1/vtlcmd.1.gz
%doc %{_prefix}/share/man/man1/vtl.1.gz
%doc %{_prefix}/share/man/man1/vtllibrary.1.gz
%doc %{_prefix}/share/man/man1/vtltape.1.gz
%doc %{_prefix}/share/man/man5/library_contents.5.gz

%changelog
* Wed Sep 26 2007 Mark Harvey <markh794@gmail.com> <mark_harvey@symantec.com>
- Bumped vers to 0.12-26
- vtl kernel module: - bumped to 0.12.14 20070926-0
  Moved memory alloc from devInfoReg() to vtl_slave_alloc() - I now don't get
  those horrible "Debug: sleeping function called from invalid context"

* Tue Sep 25 2007 Mark Harvey <markh794@gmail.com> <mark_harvey@symantec.com>
- Bumped vers to 0.12-25
- Resolved an issue where virtual media was being corrupted when performing
  erase operation.

* Sat Sep 22 2007 Mark Harvey <markh794@gmail.com> <mark_harvey@symantec.com>
- Bumped vers to 0.12-24
- On corrupt media load, return NOT READY/MEDIUM FORMAT CORRUPT

* Fri Sep 21 2007 Mark Harvey <markh794@gmail.com> <mark_harvey@symantec.com>
- Bumped vers to 0.12-23
- vtl kernel module bug fix - resolved a race condition with my usage of
  copy_to_user()/copy_from_user() and c_ioctl() routines.
  Thanks to Ray Schafer for finding and being able to reproduce race.

* Fri Aug 24 2007 Mark Harvey <markh794@gmail.com> <mark_harvey@symantec.com>
- Bumped vers to 0.12-22
- Set correct directory ownership and permissions at post install time
  for /opt/vtl

* Wed Aug 01 2007 Mark Harvey <markh794@gmail.com> <mark_harvey@symantec.com>
- Bumped vers to 0.12-21
- Corrected warnings identified by sparse

* Sat Apr 07 2007 Mark Harvey <markh794@gmail.com> <mark_harvey@symantec.com>
- Bumped vers to 0.12-20
- Calls to tune kernel behaviour of out-of-memory always return 'success'.
  Found out the hard way, earlier kernel versions do not support this feature.
- Added check for return status from build_library_config in rc script

* Sat Mar 31 2007 Mark Harvey <markh794@gmail.com> <mark_harvey@symantec.com>
- Bumped vers to 0.12-19
- Added conditional x86_64/amd64 to vtl.spec so it builds correctly on x86_64
  platforms.

* Wed Mar 28 2007 Mark Harvey <markh794@gmail.com> <mark_harvey@symantec.com>
- Bumped vers to 0.12-18
- Improved (slightly) checking of MAM header on media open.
  At least made it 32/64 bit friendly.

* Thu Mar 24 2007 Mark Harvey <markh794@gmail.com> <mark_harvey@symantec.com>
- Bumped vers to 0.12-17
- Added 'Out Of Memory killer tuning to both vtltape and vtllibrary.
- Added 'tags' as a target in Makefile, i.e. 'make tags'
  TAGS will be removed with a 'make distclean'

* Thu Mar 13 2007 Mark Harvey <markh794@gmail.com> <mark_harvey@symantec.com>
- Bumped vers to 0.12-16
- Added -D_LARGEFILE64 switch to Makefile to allow a more portable way of
  opening files > 2G. There should be no need for the x86_64 patch any more.
  Needs testing.

* Thu Feb 22 2007 Mark Harvey <markh794@gmail.com> <mark_harvey@symantec.com>
- Bumped vers to 0.12-14
- Added 'ERASE(6)' command as a result of bug report from Laurent Dongradi.
  NetBackup "bplabel -erase -l " command failing.

* Thu Sep 07 2006 Mark Harvey <markh794@gmail.com> <mark_harvey@symantec.com>
- Bumped vers to 0.12-13
- Just updated some time/date in kernel module. Nothing major.

* Tue Sep 05 2006 Mark Harvey <markh794@gmail.com> <mark_harvey@symantec.com>
- Bumped vers to 0.12-12
  Shared library (libvxscsi.so) conflict with Symantec VRTSvxvm-common-4.1.20
  so I've renamed mine to libvtlscsi.so
  Should build correctly on an x86_64 RPM system now.

* Thu Aug 31 2006 Mark Harvey <markh794@gmail.com> <mark_harvey@symantec.com>
- Bumped vers to 0.12-10 (skipped numbering between -5 & -10
  Changed interface between user-space and kernel, hence need kernel module
  version 0.12.10
  The change is to support 'auto sense', where the sense buffer is sent straight
  after the data (if sense data is valid).
  Need to fix: Way of specifying size of sense buffer. Currently defined in
  kernel src (vtl.c) and user-space header vx.h. It would be nice to have this
  defined in one place only.
- Updated rc script to check kernel version before starting user-space daemons.

* Wed Jul 05 2006 Mark Harvey <markh794@gmail.com> <mark_harvey@symantec.com>
- Bumped vers to 0.12-5
- Updated man pages to reflect name changes from vxtape/vxlibrary to
  vtltape/vtllibrary
- Removed any vxtape/vxlibrary binaries from build process

* Tue Jul 04 2006 Mark Harvey <markh794@gmail.com> <mark_harvey@symantec.com>
- Bumped vers to 0.12-4
- Fixed (Well tested) kernel makefile compile on SLES 9 and suse 9.3
  i.e. Kernel versions 2.6.5-7.191-smp & 2.6.11.4-21.12-smp
  Hacked scsi.h include file and added #if KERNEL_VERSION conditional compile
  around kfifo.[ch] if below 2.6.10
- Attempt to improve install/startup on RedHat AS4 + SLES9

* Sun Jul 02 2006 Mark Harvey <markh794@gmail.com> <mark_harvey@symantec.com>
- Bumped vers to 0.12-0
- Forked this version from main tree. Main branch is 0.13.x which will continue
  to be work-in-progress for removal of kfifo routines.
- 0.12-x is to be current 'stable' branch.

* Fri May 19 2006 Mark Harvey <markh794@gmail.com> <mark_harvey@symantec.com>
- Bumped vers to 0.10-13
- Added TapeUsage & TapeCapacity log parameters. Even thou I'm specifying these
  pages are not supported, BackupExec still probes for them and barfs on the
  scsi error returned.

* Thu May 18 2006 Mark Harvey <markh794@gmail.com> <mark_harvey@symantec.com>
- Bumped vers to 0.10-12
- Re-worked the READ ELEMENT STATUS command with the idea of adding the extra
  options which appear to be required by BackupExec.

* Wed May 10 2006 Mark Harvey <markh794@gmail.com> <mark_harvey@symantec.com>
- Bumped vers to 0.10-10
- Fixed bug introduced in vtllibrary where I broke the inventory function.

* Tue May 09 2006 Mark Harvey <markh794@gmail.com> <mark_harvey@symantec.com>
- Changed poll of 'getting SCSI command' to use ioctl instead of kfifo.
  Requires kernel module dated > 20060503-0

* Sun Apr 30 2006 Mark Harvey <markh794@gmail.com> <mark_harvey@symantec.com>
- Fixed typo in rc script where trying to set permissions on /dev/vx? instead
  of /dev/vtl?

* Sat Apr 29 2006 Mark Harvey <markh794@gmail.com> <mark_harvey@symantec.com>
- Changed logic on WIRTE-FILEMARK SCSI command. Now only attempts to write
  filemarks if greater the 0 filemarks specified.
- Added check for SuSE-release and use '{user|group}add --system' switch
  otherwise {user|group}add without the --system switch.
- Bumped version to 0.10-6

* Thu Apr 27 2006 Mark Harvey <markh794@gmail.com> <mark_harvey@symantec.com>
- Seek to EndOfData bug where the tape daemon would loop over the last block
  and never return.

* Sat Apr  5 2006 Mark Harvey <markh794@gmail.com> <mark_harvey@symantec.com>
- Changed the kernel module from 'vx_tape' to vtl (no real impact to user space
- daemons) however changing /dev/ entries from /dev/vx? to /dev/vtl due to name
- space clash with VERITAS StorageFoundation (VxVM/VxFS)

* Sat Apr  1 2006 Mark Harvey <markh794@gmail.com> <mark_harvey@symantec.com>
- Bumped version to 0.9 as this has the start of a working Solaris port.

* Thu Mar 24 2006 Mark Harvey <markh794@gmail.com> <mark_harvey@symantec.com>
- Added extra logging for mode_sense command if verbose > 2

* Thu Mar 10 2006 Mark Harvey <markh794@gmail.com> <mark_harvey@symantec.com>
- Start of Solaris kernel port.
- Moved kernel driver into seperate subdir.
- Added some 'ifdef Solaris' to source code.

* Thu Feb 22 2006 Mark Harvey <markh794@gmail.com> <mark_harvey@symantec.com>
- Added check to make sure RPM_BUILD_ROOT is not / when attempting to remove.
- Shutdown daemons before package removal

* Thu Feb 16 2006 Mark Harvey <markh794@gmail.com> <mark_harvey@symantec.com>
- Added ALLOW/DENY MEDIUM REMOVAL SCSI cmd to vxlibrary daemon.
- Added Extend/Retract checking to MOVE MEDIUM SCSI cmds.

* Thu Feb 14 2006 Mark Harvey <markh794@gmail.com> <mark_harvey@symantec.com>
- Made RPM create system account of 'vtl' & group of 'vtl' before install.

* Thu Feb 11 2006 Mark Harvey <markh794@gmail.com> <mark_harvey@symantec.com>
- bump vers from 0.7 to 0.8
- Rename /opt/vx_media to /opt/vtl
- Rename /etc/vxlibrary to /etc/vtl
- RPM install now creates vtl user & group
- rc script now installs a default library config file in /etc/vtl/

* Thu Feb  2 2006 Mark Harvey <markh794@gmail.com> <mark_harvey@symantec.com>
- Teach me for not testing fully. Resolved bug where no data was written.

* Thu Feb  2 2006 Mark Harvey <markh794@gmail.com> <mark_harvey@symantec.com>
- Improvements to handling of cleaning media.

* Wed Jan  3 2006 Mark Harvey <markh794@gmail.com> <mark_harvey@symantec.com>
- Added a new man page vtl, which aims to document how this package hangs
  together.
- Corrected a couple of hard-coded paths in rc scripts

* Wed Jan  3 2006 Mark Harvey <markh794@gmail.com> <mark_harvey@symantec.com>
- Moved source man pages into separate man directory.
- Updated version of user-mode utilities to that of the kernel module (v0.7)

* Tue Jan  3 2006 Mark Harvey <markh794@gmail.com> <mark_harvey@symantec.com>
- Added man pages.

* Sun Jan  1 2006 Mark Harvey <markh794@gmail.com> <mark_harvey@symantec.com>
- Initial spec file created.
- $Id: vtl.spec,v 1.29.2.7 2006-08-30 06:35:01 markh Exp $
