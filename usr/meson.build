cc = meson.get_compiler('c')

inc = include_directories(
  '../include',
  '../include/utils',
  '../include/common',
  '../ccan',
)

# ---- version / build defines (parity with config.mk) ----
spec = meson.project_source_root() / 'mhvtl-utils.spec'
ver   = run_command('awk', '/^Version:/{print $2}', spec, check : true).stdout().strip()
minor = run_command('awk', '/define minor/{print $3}', spec, check : true).stdout().strip()
mhvtl_version = ver + '.' + minor

githash = ''
gitdate = ''
git = find_program('git', required : false)
if git.found() and run_command('test', '-d', meson.project_source_root() / '.git', check : false).returncode() == 0
  h = run_command(git, 'show', '-s', '--format=%h', check : false)
  if h.returncode() == 0
    githash = 'commit: ' + h.stdout().strip()
  endif
  d = run_command(git, 'show', '-s', '--format=%aI', check : false)
  if d.returncode() == 0
    gitdate = d.stdout().strip()
  endif
endif

add_project_arguments(
  '-D_LARGEFILE64_SOURCE',
  '-D_GNU_SOURCE',
  '-DMHVTL_DEBUG',
  '-DMHVTL_USERLAND_BACKEND',
  '-DMHVTL_VERSION="' + mhvtl_version + '"',
  '-DMHVTL_GITDATE="' + gitdate + '"',
  '-DMHVTL_GITHASH="' + githash + '"',
  '-DMHVTL_HOME_PATH="/opt/mhvtl"',
  '-DMHVTL_CONFIG_PATH="/etc/mhvtl"',
  '-DSYSTEMD_SERVICE_DIR="/usr/lib/systemd/system"',
  language : 'c',
)

# ---- dependencies ----
threads = dependency('threads')
zlib    = dependency('zlib')

# Phase 1: modern high-throughput compression libraries.
# Linked into the tape daemon now; the codec wiring lands in Phase 3.
lz4  = dependency('liblz4')
zstd = dependency('libzstd')

# Acceptance (Phase 1): verify lz4/zstd are actually linkable (symbol-link test).
assert(cc.has_function('LZ4_versionNumber',  dependencies : lz4,
       prefix : '#include <lz4.h>'),  'liblz4 present but not linkable')
assert(cc.has_function('ZSTD_versionNumber', dependencies : zstd,
       prefix : '#include <zstd.h>'), 'libzstd present but not linkable')

# ---- shared library (mirrors libvtlscsi.so) ----
# NOTE: libvtlscsi references `mhvtl_driver_name`, which is intentionally
# *undefined* here and provided by each linking executable (every cmd/*.c
# defines its own). That is upstream's contract, so this shared object must
# permit undefined symbols: disable meson's default -Wl,--no-undefined.
libvtlscsi = shared_library('vtlscsi',
  files(
    'vtllib.c', 'mhvtl_log.c', 'mode.c', 'vtlcart.c', 'vtlcart_raw.c',
    'raw_bay_conf.c',
    'spc.c', 'smc.c',
    'utils/q.c', 'utils/subprocess.c', 'utils/mhvtl_update.c',
    'utils/crc32c.c',
    'transport.c', 'transport_mhvtl.c', 'transport_userland.c',
  ),
  include_directories : inc,
  dependencies : [threads],
  override_options : ['b_lundef=false'],
  install : false,
)

# object group reused by vtltape and dump_tape
io_srcs = files('mhvtl_io.c', 'utils/minilzo.c', 'utils/crc32c.c', 'utils/reed-solomon.c')

# ---- daemons ----
executable('vtltape',
  [files('cmd/vtltape.c', 'ssc.c',
         'pm/default_ssc_pm.c', 'pm/ult3580_pm.c', 'pm/hp_ultrium_pm.c',
         'pm/stk9x40_pm.c', 'pm/quantum_dlt_pm.c', 'pm/ait_pm.c',
         'pm/t10000_pm.c', 'pm/ibm_03592_pm.c'), io_srcs],
  include_directories : inc,
  link_with : libvtlscsi,
  dependencies : [zlib, lz4, zstd],
  install : false,
)

executable('vtllibrary',
  files('cmd/vtllibrary.c', 'vtl_cart_type.c',
        'pm/stklxx_pm.c', 'pm/hp_smc_pm.c', 'pm/overland_pm.c',
        'pm/spectra_pm.c', 'pm/scalar_pm.c', 'pm/ibm_smc_pm.c',
        'pm/default_smc_pm.c'),
  include_directories : inc,
  link_with : libvtlscsi,
  install : false,
)

# ---- tools ----
executable('vtlcmd', files('cmd/vtlcmd.c'),
  include_directories : inc, link_with : libvtlscsi, install : false)
executable('mktape', files('cmd/mktape.c'),
  include_directories : inc, link_with : libvtlscsi, install : false)
executable('mhvtl-raw-format', files('cmd/mhvtl-raw-format.c'),
  include_directories : inc, link_with : libvtlscsi, install : false)
executable('mhvtl-raw-provision', files('cmd/mhvtl-raw-provision.c'),
  include_directories : inc, link_with : libvtlscsi, install : false)
executable('edit_tape', files('cmd/edit_tape.c'),
  include_directories : inc, link_with : libvtlscsi, install : false)
executable('dump_messageQ', files('cmd/dump_messageQ.c'),
  include_directories : inc, link_with : libvtlscsi, install : false)
executable('mhvtl-device-conf-generator', files('cmd/mhvtl-device-conf-generator.c'),
  include_directories : inc, link_with : libvtlscsi, install : false)
executable('tapeexerciser', files('cmd/tapeexerciser.c'),
  include_directories : inc, install : false)
executable('dump_tape',
  [files('cmd/tape_util.c', 'pm/default_ssc_pm.c'), io_srcs],
  include_directories : inc, link_with : libvtlscsi, dependencies : [zlib], install : false)
executable('validate_crc',
  files('utils/validate_crc.c', 'utils/crc32c.c', 'utils/reed-solomon.c'),
  include_directories : inc, install : false)
