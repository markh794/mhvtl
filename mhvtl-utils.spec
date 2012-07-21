# Disable the building of the debug package(s).
%define debug_package %{nil}

Summary: Virtual tape library. kernel pseudo HBA driver + userspace daemons
%define real_name mhvtl
Name: mhvtl-utils
%define real_version 2012-07-21
Version: 1.3
Release: 2%{?dist}
License: GPL
Group: System/Kernel
URL: http://sites.google.com/site/linuxvtl2/

Source: mhvtl-%{real_version}.tgz
BuildRoot: %{_tmppath}/%{name}-%{version}-%{release}-build-%(%{__id_u} -n)

BuildRequires: lzo-devel
BuildRequires: zlib-devel

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

%build
%{__make} RPM_OPT_FLAGS="%{optflags}" VERSION="%{version}.%{release}" usr
%{__make} RPM_OPT_FLAGS="%{optflags}" VERSION="%{version}.%{release}" etc
%{__make} RPM_OPT_FLAGS="%{optflags}" VERSION="%{version}.%{release}" scripts

%install
%{__rm} -rf %{buildroot}
%{__install} -d -m0755 %{buildroot}/opt/mhvtl/

%{__install} -Dp -m0750 etc/mhvtl %{buildroot}%{_initrddir}/mhvtl

%{__install} -Dp -m0700 usr/build_library_config %{buildroot}%{_bindir}/build_library_config
%{__install} -Dp -m0750 usr/dump_tape %{buildroot}%{_bindir}/dump_tape
%{__install} -Dp -m0700 usr/make_vtl_media %{buildroot}%{_bindir}/make_vtl_media
%{__install} -Dp -m0750 usr/mktape %{buildroot}%{_bindir}/mktape
%{__install} -Dp -m0750 usr/vtlcmd %{buildroot}%{_bindir}/vtlcmd
%{__install} -Dp -m0750 usr/vtllibrary %{buildroot}%{_bindir}/vtllibrary
%{__install} -Dp -m0750 usr/vtltape %{buildroot}%{_bindir}/vtltape
%{__install} -Dp -m0700 usr/tapeexerciser %{buildroot}%{_bindir}/tapeexerciser

%{__install} -Dp -m0755 usr/libvtlcart.so %{buildroot}%{_libdir}/libvtlcart.so
%{__install} -Dp -m0755 usr/libvtlscsi.so %{buildroot}%{_libdir}/libvtlscsi.so

%{__install} -Dp -m0644 man/build_library_config.1 %{buildroot}%{_mandir}/man1/build_library_config.1
%{__install} -Dp -m0644 man/mhvtl.1 %{buildroot}%{_mandir}/man1/mhvtl.1
%{__install} -Dp -m0644 man/mktape.1 %{buildroot}%{_mandir}/man1/mktape.1
%{__install} -Dp -m0644 man/vtlcmd.1 %{buildroot}%{_mandir}/man1/vtlcmd.1
%{__install} -Dp -m0644 man/vtllibrary.1 %{buildroot}%{_mandir}/man1/vtllibrary.1
%{__install} -Dp -m0644 man/vtltape.1 %{buildroot}%{_mandir}/man1/vtltape.1

%{__install} -Dp -m0644 man/device.conf.5 %{buildroot}%{_mandir}/man5/device.conf.5
%{__install} -Dp -m0644 man/library_contents.5 %{buildroot}%{_mandir}/man5/library_contents.5

%pre
if ! getent group vtl &>/dev/null; then
   groupadd -r vtl
fi
if ! getent passwd vtl &>/dev/null; then
   useradd -r -g vtl -c "VTL daemon" -d /opt/mhvtl -s /bin/bash vtl
fi

%post
/sbin/ldconfig
/sbin/chkconfig --add mhvtl

%preun
if (( $1 == 0 )); then
    /sbin/service mhvtl shutdown &>/dev/null || :
    /sbin/chkconfig --del mhvtl
fi

%postun -p /sbin/ldconfig

%clean
%{__rm} -rf %{buildroot}

%files
%defattr(-, vtl, vtl, 0755)
%doc INSTALL README etc/library_contents.sample
%doc %{_mandir}/man1/build_library_config.1*
%doc %{_mandir}/man1/mhvtl.1*
%doc %{_mandir}/man1/mktape.1*
%doc %{_mandir}/man1/vtlcmd.1*
%doc %{_mandir}/man1/vtllibrary.1*
%doc %{_mandir}/man1/vtltape.1*
%doc %{_mandir}/man5/device.conf.5*
%doc %{_mandir}/man5/library_contents.5*
%config %{_initrddir}/mhvtl
%{_bindir}/vtlcmd
%{_bindir}/mktape
%{_bindir}/dump_tape
%{_bindir}/tapeexerciser
%{_bindir}/build_library_config
%{_bindir}/make_vtl_media
%{_libdir}/libvtlscsi.so
%{_libdir}/libvtlcart.so

%defattr(4750, root, vtl, 0755)
%{_bindir}/vtltape
%{_bindir}/vtllibrary

%defattr(-, vtl, vtl, 2770)
/opt/mhvtl/

%changelog
* Thu Jun 21 2012 Dag Wieers <dag@wieers.com> - 1.3-1
- Updated to release 1.3 (2012-06-15).

* Thu Aug 05 2010 Dag Wieers <dag@wieers.com> - 0.18-11
- Initial build of the kmod package.
