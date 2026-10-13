# Disable the building of the debug package(s).
%define debug_package %{nil}

# config.sh parses '_firmwarepath' to define FIRMWAREDIR in parent Makefile
%global _firmwarepath	/usr/lib/firmware

# Compat path macros
# pilfered from https://src.fedoraproject.org/rpms/snapd/blob/master/f/snapd.spec
%{!?_systemdgeneratordir: %global _systemdgeneratordir %{_prefix}/lib/systemd/system-generators}

%define _unpackaged_files_terminate_build 0

%define mhvtl_home_dir /var/lib/mhvtl
%global dkms_version %{version}.%{minor}

Summary: Virtual tape library. kernel pseudo HBA driver + userspace daemons
%define real_name mhvtl
Name: mhvtl-utils
%define real_version 2026-03-10
%define minor 0
Version: 1.8
Release: %{minor}%{?dist}
License: GPL
Group: System/Kernel
URL: http://sites.google.com/site/linuxvtl2/

Source: mhvtl-%{real_version}.tgz
BuildRoot: %{_tmppath}/%{name}-%{version}-%{release}-build-%(%{__id_u} -n)

Recommends: lsscsi mtx mt-st

Requires:sg3_utils
Requires: policycoreutils
Requires: tar >= 1.28
Requires: dkms

BuildRequires: systemd
BuildRequires: systemd-rpm-macros
BuildRequires: zlib-devel
BuildRequires: pkgconfig
BuildRequires: pkgconfig(libudev)
BuildRequires: pkgconfig(liblz4)
BuildRequires: pkgconfig(libzstd)
%{?systemd_requires}
%{?systemd_ordering}

Obsoletes: mhvtl <= %{version}-%{release}
Provides: mhvtl = %{version}-%{release}

%description
A Virtual tape library and tape drives:

Used to emulate hardware robot & tape drives:

VTL consists of a pseudo HBA kernel driver and user-space daemons which
function as the SCSI target.

Communication between the kernel module and the daemons is achieved
via /dev/mhvtl? device nodes.

The kernel module is based on the scsi_debug driver.
The SSC/SMC target daemons have been written from scratch.

%prep
%setup -n %{real_name}-%{version}

%post
/sbin/semanage fcontext -a -t systemd_unit_file_t %{_unitdir}/mhvtl-load-modules.service
/sbin/semanage fcontext -a -t systemd_unit_file_t %{_unitdir}/vtllibrary@.service
/sbin/semanage fcontext -a -t systemd_unit_file_t %{_unitdir}/vtltape@.service
/sbin/semanage fcontext -a -t systemd_unit_file_t %{_unitdir}/mhvtl.target
/sbin/restorecon -R -v %{_unitdir}
/bin/systemctl daemon-reload
%tmpfiles_create %{_tmpfilesdir}/mhvtl.conf
%{service_add_post mhvtl.target mhvtl-load-modules.service vtllibrary@.service vtltape@.service}
/bin/systemctl start mhvtl.target
/bin/systemctl enable  mhvtl.target
if [ "$1" -eq 1 ]; then
	# Fresh install only: seed the default library's media under the home dir.
	# On upgrade the admin's existing media and %config(noreplace) device.conf
	# are left untouched (whatever home path they already reference keeps
	# working), so a relocated default never duplicates or orphans data.
	make_vtl_media --config-dir=%{_sysconfdir}/mhvtl --home-dir=%{mhvtl_home_dir} --mktape-path=%{_bindir}
fi

# DKMS registration (additive; do not fail install if DKMS isn't active)
/usr/sbin/dkms add -m mhvtl -v %{dkms_version} || :
/usr/sbin/dkms build -m mhvtl -v %{dkms_version} || :
/usr/sbin/dkms install -m mhvtl -v %{dkms_version} || :

%postun
/sbin/ldconfig
/bin/systemctl daemon-reload

# Remove DKMS module on package erase
if [ "$1" -eq 0 ]; then
	/usr/sbin/dkms remove -m mhvtl -v %{dkms_version} --all || :
fi

#%{service_del_postun mhvtl.target mhvtl-load-modules.service vtllibrary@.service vtltape@.service}

%pre
#%{service_add_pre mhvtl.target mhvtl-load-modules.service vtllibrary@.service vtltape@.service}

%preun
/bin/systemctl stop  mhvtl.target
/bin/systemctl disable  mhvtl.target
#%{service_del_preun mhvtl.target mhvtl-load-modules.service vtllibrary@.service vtltape@.service}

%build
# LIBDIR and MHVTL_LIBEXECDIR must match the install section below: LIBDIR
# is baked into the binaries as the RUNPATH to libvtlscsi.so (under
# LIBDIR/mhvtl), and the daemons' unit ExecStart path is generated here from
# MHVTL_LIBEXECDIR, where the daemons are installed below.
make MHVTL_HOME_PATH=%{mhvtl_home_dir} VERSION=%{version} EXTRAVERSION=%{minor} LIBDIR=%{_libdir} \
	MHVTL_LIBEXECDIR=%{_libexecdir}/mhvtl \
	SYSTEMD_GENERATOR_DIR=%{_systemdgeneratordir} \
	SYSTEMD_SERVICE_DIR=%{_unitdir}

%install
%make_install \
	MHVTL_HOME_PATH=%{mhvtl_home_dir} VERSION=%{version} EXTRAVERSION=%{minor} LIBDIR=%{_libdir} \
	MHVTL_LIBEXECDIR=%{_libexecdir}/mhvtl MHVTL_DATADIR=%{_datadir}/mhvtl \
	SYSTEMD_GENERATOR_DIR=%{_systemdgeneratordir} \
	SYSTEMD_SERVICE_DIR=%{_unitdir} \
	TMPFILES_DIR=%{_tmpfilesdir}
# The kernel module is built out of band from the packaged mhvtl_kernel.tgz;
# discard the module the recursive install staged for the build host's kernel.
%{__rm} -rf %{buildroot}/lib/modules
install -d -m 755 %{buildroot}%{_sbindir}
ln -s %{_sbindir}/service %{buildroot}/%{_sbindir}/rc%{name}
# The media home directory (%{mhvtl_home_dir}) is created by %make_install.

# DKMS source tree (additive; does not replace existing install behavior)
install -d -m 755 %{buildroot}%{_usrsrc}/mhvtl-%{dkms_version}
tar -xf mhvtl_kernel.tgz -C %{buildroot}%{_usrsrc}/mhvtl-%{dkms_version}
sed -i 's/^PACKAGE_VERSION=.*/PACKAGE_VERSION="%{dkms_version}"/' \
	%{buildroot}%{_usrsrc}/mhvtl-%{dkms_version}/dkms.conf

%clean
%{__rm} -rf %{buildroot}

%files
%defattr(-, root, root, 0755)
%doc INSTALL README etc/library_contents.sample
%doc %{_mandir}/man1/mktape.1*
%doc %{_mandir}/man1/edit_tape.1*
%doc %{_mandir}/man1/vtlcmd.1*
%doc %{_mandir}/man1/vtllibrary.1*
%doc %{_mandir}/man1/vtltape.1*
%doc %{_mandir}/man1/preload_tape.1*
%doc %{_mandir}/man1/dump_tape.1*
%doc %{_mandir}/man1/make_vtl_media.1*
%doc %{_mandir}/man1/mhvtl_kernel_mod_build.1*
%doc %{_mandir}/man1/tapeexerciser.1*
%doc %{_mandir}/man1/update_device.conf.1*
%doc %{_mandir}/man1/generate_device_conf.1*
%doc %{_mandir}/man1/generate_library_contents.1*
%doc %{_mandir}/man5/device.conf.5*
%doc %{_mandir}/man5/mhvtl.conf.5*
%doc %{_mandir}/man5/library_contents.5*
%{_bindir}/vtlcmd
%{_bindir}/mktape
%{_bindir}/edit_tape
%{_bindir}/dump_tape
%{_bindir}/preload_tape
%{_bindir}/tapeexerciser
%{_bindir}/make_vtl_media
%{_bindir}/mhvtl_kernel_mod_build
%{_bindir}/update_device.conf
%{_bindir}/generate_device_conf
%{_bindir}/generate_library_contents
%dir %{_libdir}/mhvtl
%{_libdir}/mhvtl/libvtlscsi.so
%{_firmwarepath}/mhvtl/mhvtl_kernel.tgz
%dir %{_sysconfdir}/mhvtl
%config(noreplace) %{_sysconfdir}/mhvtl/mhvtl.conf
%config(noreplace) %{_sysconfdir}/mhvtl/device.conf
%config(noreplace) %{_sysconfdir}/mhvtl/library_contents.10
%config(noreplace) %{_sysconfdir}/mhvtl/library_contents.30
%dir %{_datadir}/mhvtl
%{_datadir}/mhvtl/*
%dir %{_usrsrc}/mhvtl-%{dkms_version}
%{_usrsrc}/mhvtl-%{dkms_version}/*
%{_systemdgeneratordir}/mhvtl-device-conf-generator
%{_unitdir}/mhvtl-load-modules.service
%{_unitdir}/vtllibrary@.service
%{_unitdir}/vtltape@.service
%{_unitdir}/mhvtl.target
%{_tmpfilesdir}/mhvtl.conf
%dir %{mhvtl_home_dir}
%ghost %{mhvtl_home_dir}/*

%defattr(755, root, root, 0755)
%dir %{_libexecdir}/mhvtl
%{_libexecdir}/mhvtl/vtltape
%{_libexecdir}/mhvtl/vtllibrary

%changelog
* Fri Jan 30 2026 Maarten Jacobs <maarten256@outlook.com> - 1.7-3
- Added DKMS deployment so kernel module is built and kept in sync with kernel

* Mon Mar 10 2025 Mark Harvey <markh794@gmail.com> - 1.7-2
- Updated to release 1.7-2 (2025-03-10).

* Fri Mar 10 2023 Mark Harvey <markh794@gmail.com> - 1.7-1
- Updated to release 1.7-1 (2023-03-10).

* Thu Mar 10 2022 Mark Harvey <markh794@gmail.com> - 1.7-0
- Updated to release 1.7-0 (2022-03-10).

* Thu Oct 07 2021 Mark Harvey <markh794@gmail.com> - 1.6-4
- Updated to release 1.6-4 (2021-10-07).

* Tue Mar 03 2020 Mark Harvey <markh794@gmail.com> - 1.6-3
- Updated to release 1.6-3 (2020-03-10).

* Sun Oct 06 2019 Mark Harvey <markh794@gmail.com> - 1.6-2
- Updated to release 1.6-2 (2019-10-06).

* Thu Mar 10 2016 Mark Harvey <markh794@gmail.com> - 1.5-4
- Updated to release 1.5-4 (2016-03-10).

* Tue Apr 14 2015 Mark Harvey <markh794@gmail.com> - 1.5-3
- Updated to release 1.5-3 (2015-04-14).

* Sun Sep 7 2014 Mark Harvey <markh794@gmail.com> - 1.5-2
- Updated to release 1.5-2 (2014-09-04).

* Sun Apr 13 2014 Mark Harvey <markh794@gmail.com> - 1.5-0
- Updated to release 1.5-0 (2014-04-13).

* Sun Oct 20 2013 Mark Harvey <markh794@gmail.com> - 1.4-10
- Updated to release 1.4-10 (2013-10-20).

* Thu Aug 29 2013 Mark Harvey <markh794@gmail.com> - 1.4-9
- Updated to release 1.4-9 (2013-08-29).

* Sat Jun 29 2013 Mark Harvey <markh794@gmail.com> - 1.4-8
- Updated to release 1.4-8 (2013-06-29).

* Fri Mar 22 2013 Mark Harvey <markh794@gmail.com> - 1.4-7
- Updated to release 1.4-7 (2013-03-22).

* Thu Jan 31 2013 Mark Harvey <markh794@gmail.com> - 1.4-6
- Updated to release 1.4-6 (2013-01-31).

* Sat Jan 12 2013 Mark Harvey <markh794@gmail.com> - 1.4-5
- Updated to release 1.4-5 (2013-01-12).

* Mon Aug 13 2012 Mark Harvey <markh794@gmail.com> - 1.4-4
- Updated to release 1.4-4 (2012-09-13).

* Wed Aug  8 2012 Mark Harvey <markh794@gmail.com> - 1.4-1
- Updated to release 1.4-1 (2012-08-08).

* Wed Aug  1 2012 Mark Harvey <markh794@gmail.com> - 1.4-0
- Updated to release 1.4 (2012-08-01).
- install using Makefile

* Thu Jun 21 2012 Dag Wieers <dag@wieers.com> - 1.3-1
- Updated to release 1.3 (2012-06-15).

* Thu Aug 05 2010 Dag Wieers <dag@wieers.com> - 0.18-11
- Initial build of the kmod package.
