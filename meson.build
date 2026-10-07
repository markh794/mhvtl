# Parallel user-space build system (Phase 1 modernization).
#
# This builds ONLY the user-space C (the daemons/CLIs and libvtlscsi that
# usr/Makefile builds). The canonical Makefile and the kbuild/DKMS kernel
# module build remain authoritative and are intentionally NOT replaced here.
#
# warning_level=2 => -Wall -Wextra. -Wpedantic is intentionally deferred
# (upstream is not pedantic-clean; tracked as a separate cleanup).
project('mhvtl', 'c',
  version : '1.8.0',
  license : 'GPL-2.0-or-later',
  meson_version : '>= 0.56.0',
  default_options : [
    'c_std=gnu99',
    'warning_level=2',
    'buildtype=debugoptimized',   # -g -O2, matches the Makefile
  ],
)

subdir('usr')
