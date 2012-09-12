# Disable the building of the debug package(s).
%define debug_package %{nil}

Summary: Virtual tape library. kernel pseudo HBA driver + userspace daemons
%define real_name mhvtl
Name: mhvtl-utils
%define real_version 2012-09-12
Version: 1.4
Release: 3%{?dist}
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
%{__make} RPM_OPT_FLAGS="%{optflags}" VERSION="%{version}.%{release}" INITD="%{_initrddir}" etc
%{__make} RPM_OPT_FLAGS="%{optflags}" VERSION="%{version}.%{release}" scripts

%install
%{__rm} -rf %{buildroot}
%{__make} install DESTDIR="%{buildroot}" INITD="%{_initrddir}" LIBDIR="%{_libdir}"

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
%doc %{_mandir}/man1/make_vtl_media.1*
%doc %{_mandir}/man5/device.conf.5*
%doc %{_mandir}/man5/mhvtl.conf.5*
%doc %{_mandir}/man5/library_contents.5*
%config %{_initrddir}/mhvtl
%{_bindir}/vtlcmd
%{_bindir}/mktape
%{_bindir}/dump_tape
%{_bindir}/tapeexerciser
%{_bindir}/build_library_config
%{_bindir}/make_vtl_media
%{_bindir}/update_device.conf
%{_libdir}/libvtlscsi.so
%{_libdir}/libvtlcart.so

%defattr(4750, root, vtl, 0755)
%{_bindir}/vtltape
%{_bindir}/vtllibrary

%defattr(-, vtl, vtl, 2770)
/opt/mhvtl/

%changelog
* Wed Aug  8 2012 Mark Harvey <markh794@gmail.com> - 1.4-1
- Updated to release 1.4-1 (2012-08-08).

* Wed Aug  1 2012 Mark Harvey <markh794@gmail.com> - 1.4-0
- Updated to release 1.4 (2012-08-01).
- install using Makefile

* Thu Jun 21 2012 Dag Wieers <dag@wieers.com> - 1.3-1
- Updated to release 1.3 (2012-06-15).

* Thu Aug 05 2010 Dag Wieers <dag@wieers.com> - 0.18-11
- Initial build of the kmod package.
