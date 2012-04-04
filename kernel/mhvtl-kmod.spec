# $Id: kmod-mhvtl-src.spec 9026 2010-08-02 12:20:46Z dag $
# Authority: dag

# ExclusiveDist: el5
# Archs: i686 x86_64

%define kversion 2.6.18-194.el5
%{!?kversion:%define kversion %(rpm -q kernel-devel --qf '%{RPMTAG_VERSION}-%{RPMTAG_RELEASE}\n' | tail -1)}

# Define the kmod package name here.
%define kmod_name mhvtl

Summary: Virtual Tape Library device driver for Linux
Name: mhvtl-kmod
Version: 1.2
Release: 3%{?dist}
License: GPL2
Group: System Environment/Kernel
URL: http://sites.google.com/site/linuxvtl2/

BuildRoot: %{_tmppath}/%{name}-%{version}-%{release}-build-%(%{__id_u} -n)
ExclusiveArch: i686 x86_64

# Sources.
Source0: http://sites.google.com/site/linuxvtl2/mhvtl-2012-03-22.tgz
Source10: kmodtool-mhvtl

# If kversion isn't defined on the rpmbuild line, build for the current kernel.
%{!?kversion: %define kversion %(uname -r)}

# Define the variants for each architecture.
%define basevar ""
%ifarch i686
%define paevar PAE default smp
%endif
%ifarch i686 x86_64
%define xenvar xen smp generic
%endif

# If kvariants isn't defined on the rpmbuild line, build all variants for this architecture.
%{!?kvariants: %define kvariants %{?basevar} %{?xenvar} %{?paevar}}

# Magic hidden here.
%define kmodtool sh %{SOURCE10}
%{expand:%(%{kmodtool} rpmtemplate_kmp %{kmod_name} %{kversion} %{kvariants} 2>/dev/null)}

%description
This package provides the Virtual Tape Library device driver module for
Linux.  It is built to depend upon the specific ABI provided by a range
of releases of the same variant of the Linux kernel and not on any one
specific build.

%prep
%setup -c -T -a 0
for kvariant in %{kvariants} ; do
    %{__cp} -a %{kmod_name}-%{version}/kernel/ _kmod_build_$kvariant
    %{__cat} <<-EOF >_kmod_build_$kvariant/%{kmod_name}.conf
override %{kmod_name} * weak-updates/%{kmod_name}
EOF
done

%build
for kvariant in %{kvariants} ; do
    ksrc=%{_usrsrc}/kernels/%{kversion}${kvariant:+-$kvariant}-%{_target_cpu}
    pushd _kmod_build_$kvariant
    %{__make} -C "${ksrc}" modules M="$PWD"
    popd
done

%install
export INSTALL_MOD_PATH="%{buildroot}"
export INSTALL_MOD_DIR="extra/%{kmod_name}"
for kvariant in %{kvariants} ; do
    ksrc=%{_usrsrc}/kernels/%{kversion}${kvariant:+-$kvariant}-%{_target_cpu}
    pushd _kmod_build_$kvariant
    %{__make} -C "${ksrc}" modules_install M="$PWD"
    %{__install} -d %{buildroot}%{_sysconfdir}/depmod.d/
    %{__install} %{kmod_name}.conf %{buildroot}%{_sysconfdir}/depmod.d/
    popd
done
# Strip the module(s).
find ${INSTALL_MOD_PATH} -type f -name \*.ko -exec strip --strip-debug \{\} \;

%clean
%{__rm} -rf %{buildroot}

%changelog
* Thu Aug 05 2010 Dag Wieers <dag@wieers.com> - 0.18-11
- Initial build of the kmod package.
