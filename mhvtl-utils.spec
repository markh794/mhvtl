# Disable the building of the debug package(s).
%define debug_package %{nil}

# config.sh parses '_firmwarepath' to define FIRMWAREDIR in parent Makefile
%global _firmwarepath	/usr/lib/firmware

# Compat path macros
# pilfered from https://src.fedoraproject.org/rpms/snapd/blob/master/f/snapd.spec
%{!?_systemdgeneratordir: %global _systemdgeneratordir %{_prefix}/lib/systemd/system-generators}

%define mhvtl_home_dir /var/lib/mhvtl
# Former default media home; only consulted when upgrading from a release
# that used it.
%define mhvtl_legacy_home_dir /opt/mhvtl

# On openSUSE, also build prebuilt kernel module packages (KMP), one per kernel
# flavor, as the distribution's own mhvtl package does. They provide
# mhvtl-kmod as an alternative to mhvtl-dkms. Not built where the
# distribution kernel has no flavors to build against.
%if 0%{?suse_version}
%ifnarch ppc s390x
%define buildkmp 1
%endif
%endif
%global dkms_version %{version}

Summary: Virtual tape library. kernel pseudo HBA driver + userspace daemons
%define real_name mhvtl
Name: mhvtl-utils
# Version comes from meson.build; scripts/sync-version.sh writes it here (and
# into the changelogs), and CI fails when they disagree. Release counts
# packaging revisions of that version.
Version: 1.8.0
Release: 1%{?dist}
%if 0%{?suse_version}
# openSUSE ships its own mhvtl packages versioned 1.72_release+... (upstream
# 1.7-2), which RPM orders above 1.8. Without an epoch the solver would prefer
# them, e.g. pick openSUSE's older kernel module package for a new kernel next
# to these 1.8 daemons. Epoch 1 orders every package built from this spec
# (KMPs included) above them. It is SUSE-only, so other distributions do not
# carry it; the mhvtl-kmod capability stays epoch-less on purpose.
Epoch: 1
%endif
License: GPL
URL: http://sites.google.com/site/linuxvtl2/

Source: mhvtl-%{version}.tgz
# Subpackage preamble for the openSUSE kernel module packages (KMP) below.
Source2: mhvtl.preamble
BuildRoot: %{_tmppath}/%{name}-%{version}-%{release}-build-%(%{__id_u} -n)

Recommends: lsscsi mtx mt-st

Requires:sg3_utils
Requires: policycoreutils
Requires: tar >= 1.28
# The daemons and the kernel module share the vtl_common.h ioctl ABI, so the
# module must come from the same release. mhvtl-dkms provides it; a prebuilt
# kmod/KMP package can provide the same capability instead.
Requires: mhvtl-kmod = %{version}
%if 0%{?buildkmp}
# Steer the solver to the prebuilt module over DKMS when both are available.
Recommends: mhvtl-kmp
%endif

BuildRequires: systemd
BuildRequires: systemd-rpm-macros
BuildRequires: pkgconfig(zlib)
BuildRequires: pkgconfig
BuildRequires: pkgconfig(libudev)
BuildRequires: pkgconfig(liblz4)
BuildRequires: pkgconfig(libzstd)
%{?systemd_requires}
%{?systemd_ordering}

# Replaces the distributions' "mhvtl" package. openSUSE versions upstream 1.7-2
# as 1.72_release, which RPM orders above 1.8, so a bound at our own version
# would never match it; any 1.x is obsoleted instead. This deliberately claims
# the mhvtl name for all 1.x; a package never obsoletes itself, and the mhvtl
# Provides below keeps dependencies on the old name satisfied.
Obsoletes: mhvtl < 2
Provides: mhvtl = %{version}-%{release}

%if 0%{?buildkmp}
BuildRequires: %kernel_module_package_buildreqs
# Named mhvtl-kmp-<flavor>, as in openSUSE; kdump, ec2 and um are excluded.
%suse_kernel_module_package -n mhvtl -p %{S:2} kdump ec2 um
%endif

%description
A Virtual tape library and tape drives:

Used to emulate hardware robot & tape drives:

VTL consists of a pseudo HBA kernel driver and user-space daemons which
function as the SCSI target.

Communication between the kernel module and the daemons is achieved
via /dev/mhvtl? device nodes.

The kernel module is based on the scsi_debug driver.
The SSC/SMC target daemons have been written from scratch.

%package -n mhvtl-dkms
Summary: Virtual tape library - kernel module source for DKMS
BuildArch: noarch
Requires: dkms
Requires: gcc
Requires: make
Provides: mhvtl-kmod = %{version}
# openSUSE's prebuilt kernel module packages: two mhvtl.ko builds would compete
# for the same kernel.
Conflicts: mhvtl-kmp

%description -n mhvtl-dkms
Source of the mhvtl pseudo HBA kernel module, registered with DKMS so the
module is built for the running kernel on installation and rebuilt whenever
a new kernel is installed. Kernel headers (kernel-devel / kernel-default-devel)
matching the running kernel must be installed for the build to succeed.

%prep
%setup -n %{real_name}-%{version}

%pre
# SUSE's service_add_post applies presets only to the units service_add_pre
# recorded as newly installed; the systemd macros need no pre step.
%if 0%{?suse_version}
%service_add_pre mhvtl-load-modules.service mhvtl-bay-monitor.service mhvtl.target
%endif

%post
# Label the unit files for SELinux only where the management tooling is
# actually installed; skip quietly otherwise (and never fail the install).
if command -v semanage >/dev/null 2>&1 && command -v restorecon >/dev/null 2>&1; then
	for unit in mhvtl-load-modules.service mhvtl-bay-monitor.service mhvtl-media.service vtllibrary@.service vtltape@.service mhvtl.target; do
		semanage fcontext -a -t systemd_unit_file_t "%{_unitdir}/$unit" 2>/dev/null || \
		semanage fcontext -m -t systemd_unit_file_t "%{_unitdir}/$unit" 2>/dev/null || :
	done
	restorecon -R %{_unitdir} || :
fi
%tmpfiles_create %{_tmpfilesdir}/mhvtl.conf
if [ "$1" -ge 2 ] && [ -n "$(ls -A %{mhvtl_legacy_home_dir} 2>/dev/null)" ] &&
   [ -z "$(ls -A %{mhvtl_home_dir} 2>/dev/null)" ]; then
	# Upgrade from a release that kept media under %{mhvtl_legacy_home_dir}.
	# %config(noreplace) only preserves configs the admin edited; unedited
	# ones were just replaced with copies pointing at %{mhvtl_home_dir},
	# which would orphan the existing media (and let mhvtl-media.service seed
	# a second, empty library there). Point them back at the old home.
	sed -i -e 's#^\([[:space:]]*Home directory:[[:space:]]*\)%{mhvtl_home_dir}\(/\|[[:space:]]*$\)#\1%{mhvtl_legacy_home_dir}\2#' \
		%{_sysconfdir}/mhvtl/device.conf
	sed -i -e 's#^MHVTL_HOME_PATH=%{mhvtl_home_dir}\(/\|[[:space:]]*$\)#MHVTL_HOME_PATH=%{mhvtl_legacy_home_dir}\1#' \
		%{_sysconfdir}/mhvtl/mhvtl.conf
	echo "mhvtl: existing media kept in %{mhvtl_legacy_home_dir}; to relocate it, move it to %{mhvtl_home_dir} and update device.conf and mhvtl.conf." >&2
fi
# Register the units via the distribution's service macros (which run
# daemon-reload and apply presets) rather than force-starting/enabling them:
# mhVTL needs a configured library before it can run, so it is not auto-started.
%if 0%{?suse_version}
%service_add_post mhvtl-load-modules.service mhvtl-bay-monitor.service mhvtl.target
%else
%systemd_post mhvtl-load-modules.service mhvtl-bay-monitor.service mhvtl.target
%endif

%postun
/sbin/ldconfig
%if 0%{?suse_version}
%service_del_postun mhvtl-load-modules.service mhvtl-bay-monitor.service mhvtl.target
%else
%systemd_postun mhvtl-load-modules.service mhvtl-bay-monitor.service mhvtl.target
%endif

%preun
# Stop/disable only on erase (not on upgrade); the macro handles that.
%if 0%{?suse_version}
%service_del_preun mhvtl-load-modules.service mhvtl-bay-monitor.service mhvtl.target
%else
%systemd_preun mhvtl-load-modules.service mhvtl-bay-monitor.service mhvtl.target
%endif

%preun -n mhvtl-dkms
# Unregister this version on erase, upgrade and reinstall alike. On upgrade
# and reinstall the posttrans scriptlet below registers the new version again;
# it runs after this one, which a registration at post time would not.
#
# On EL, dkms remove calls weak-modules --remove-modules before it deletes the
# module, and weak-modules keeps any weak link whose target still exists, so
# the links it made for other installed kernels are left dangling. Note the
# modules they point to, and pass those dkms deleted to weak-modules again.
weak_targets=
if [ -x /usr/sbin/weak-modules ]; then
	weak_targets=$(find /lib/modules/*/weak-updates -name 'mhvtl.ko*' -type l \
		-exec readlink {} \; 2>/dev/null | sort -u)
fi
dkms remove -m mhvtl -v %{dkms_version} --all >/dev/null 2>&1 || :
gone=$(for module in $weak_targets; do [ -e "$module" ] || echo "$module"; done)
if [ -n "$gone" ]; then
	echo "$gone" | /usr/sbin/weak-modules --no-initramfs --remove-modules >/dev/null 2>&1 || :
fi

%posttrans -n mhvtl-dkms
# Releases before the mhvtl-dkms split shipped the source in mhvtl-utils and
# left their DKMS registration behind on upgrade, with the source tree gone.
# dkms remove refuses such an entry, and deleting it by hand would leave its
# installed module behind (to be restored when this version is removed). So
# lend it this release's source, whose module layout is the same, under its
# own version, and let dkms undo its install properly.
for tree in /var/lib/dkms/mhvtl/*/; do
	tree=${tree%/}
	[ -L "$tree/source" ] || continue
	ver=${tree##*/}
	[ "$ver" = "%{dkms_version}" ] && continue
	src=%{_usrsrc}/mhvtl-$ver
	[ -e "$src" ] && continue
	cp -a %{_usrsrc}/mhvtl-%{dkms_version} "$src" &&
		sed -i "s/^PACKAGE_VERSION=.*/PACKAGE_VERSION=\"$ver\"/" "$src/dkms.conf" &&
		dkms remove -m mhvtl -v "$ver" --all >/dev/null 2>&1
	rm -rf "$src"
done
# Build and install for the running kernel. A missing kernel-devel must not
# fail the transaction: DKMS retries automatically when a kernel is installed.
dkms add -m mhvtl -v %{dkms_version} -q || :
dkms build -m mhvtl -v %{dkms_version} -q || :
dkms install -m mhvtl -v %{dkms_version} -q --force || :

%build
# LIBDIR and MHVTL_LIBEXECDIR must match the install section below: LIBDIR
# is baked into the binaries as the RUNPATH to libvtlscsi.so (under
# LIBDIR/mhvtl), and the daemons' unit ExecStart path is generated here from
# MHVTL_LIBEXECDIR, where the daemons are installed below.
make MHVTL_HOME_PATH=%{mhvtl_home_dir} LIBDIR=%{_libdir} \
	MHVTL_LIBEXECDIR=%{_libexecdir}/mhvtl \
	SYSTEMD_GENERATOR_DIR=%{_systemdgeneratordir} \
	SYSTEMD_SERVICE_DIR=%{_unitdir}
%if 0%{?buildkmp}
# Build the module once per kernel flavor, from the same flattened source tree
# that mhvtl-dkms ships. KDIR is the flavor's /lib/modules/<release>/build, the
# layout DKMS also uses, so config.sh finds the headers via its source link.
[ -z "$SOURCE_DATE_EPOCH" ] || export KBUILD_BUILD_TIMESTAMP=@$SOURCE_DATE_EPOCH
make -C kernel ../mhvtl_kernel.tgz
for flavor in %flavors_to_build; do
	obj=/usr/src/linux-obj/%_target_cpu/$flavor
	krel=$(make -s -C $obj kernelrelease)
	rm -rf kmp/$flavor && mkdir -p kmp/$flavor
	tar -xf mhvtl_kernel.tgz -C kmp/$flavor
	make -C kmp/$flavor KDIR=/lib/modules/$krel/build mhvtl.ko
done
%endif

%install
%make_install \
	MHVTL_HOME_PATH=%{mhvtl_home_dir} LIBDIR=%{_libdir} \
	MHVTL_LIBEXECDIR=%{_libexecdir}/mhvtl MHVTL_DATADIR=%{_datadir}/mhvtl \
	SYSTEMD_GENERATOR_DIR=%{_systemdgeneratordir} \
	SYSTEMD_SERVICE_DIR=%{_unitdir} \
	TMPFILES_DIR=%{_tmpfilesdir}
# The kernel module is built out of band from the packaged mhvtl_kernel.tgz;
# discard the module the recursive install staged for the build host's kernel.
%{__rm} -rf %{buildroot}/lib/modules
# The LSB init script is for non-systemd systems; every distribution this spec
# targets uses systemd, where the native units own mhVTL.
%{__rm} -f %{buildroot}/etc/init.d/mhvtl
%if 0%{?buildkmp}
export INSTALL_MOD_PATH=%{buildroot}
export INSTALL_MOD_DIR=updates
for flavor in %flavors_to_build; do
	make -C /usr/src/linux-obj/%_target_cpu/$flavor M=$PWD/kmp/$flavor modules_install
done
%endif
# The media home directory (%{mhvtl_home_dir}) is created by %make_install.

# DKMS source tree, shipped by the mhvtl-dkms subpackage
install -d -m 755 %{buildroot}%{_usrsrc}/mhvtl-%{dkms_version}
tar -xf mhvtl_kernel.tgz -C %{buildroot}%{_usrsrc}/mhvtl-%{dkms_version}
sed -i 's/^PACKAGE_VERSION=.*/PACKAGE_VERSION="%{dkms_version}"/' \
	%{buildroot}%{_usrsrc}/mhvtl-%{dkms_version}/dkms.conf

%clean
%{__rm} -rf %{buildroot}

%files
%license COPYING
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
%{_bindir}/dump_messageQ
%{_bindir}/mhvtl-raw-format
%{_bindir}/mhvtl-raw-provision
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
%dir %{_firmwarepath}/mhvtl
%{_firmwarepath}/mhvtl/mhvtl_kernel.tgz
%dir %{_sysconfdir}/mhvtl
%config(noreplace) %{_sysconfdir}/mhvtl/mhvtl.conf
%config(noreplace) %{_sysconfdir}/mhvtl/device.conf
%config(noreplace) %{_sysconfdir}/mhvtl/library_contents.10
%config(noreplace) %{_sysconfdir}/mhvtl/library_contents.30
%dir %{_datadir}/mhvtl
%{_datadir}/mhvtl/*
%{_systemdgeneratordir}/mhvtl-device-conf-generator
%{_unitdir}/mhvtl-load-modules.service
%{_unitdir}/mhvtl-bay-monitor.service
%{_unitdir}/mhvtl-media.service
%{_unitdir}/vtllibrary@.service
%{_unitdir}/vtltape@.service
%{_unitdir}/mhvtl.target
%{_tmpfilesdir}/mhvtl.conf
%dir %{mhvtl_home_dir}
%ghost %{mhvtl_home_dir}/*

%dir %{_libexecdir}/mhvtl
%{_libexecdir}/mhvtl/vtltape
%{_libexecdir}/mhvtl/vtllibrary
%{_libexecdir}/mhvtl/mhvtl-bay-monitor

%files -n mhvtl-dkms
%{_usrsrc}/mhvtl-%{dkms_version}

%changelog
* Tue Oct 13 2026 Ciro Iriarte <ciro.iriarte@gmail.com> - 1.8.0-1
- Update to 1.8.0; the version now comes from meson.build.
- Split the kernel module source into mhvtl-dkms; build openSUSE KMPs.
- Install the daemons under libexec and default the media home to /var/lib/mhvtl.
- Use the distribution service macros; create media from mhvtl-media.service.

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
