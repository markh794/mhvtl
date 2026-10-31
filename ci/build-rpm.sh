#!/bin/bash
# Build the mhvtl RPM in a clean container (Rocky/EL via dnf, openSUSE via
# zypper) and run rpmlint. Invoked by .github/workflows/packages.yml and
# runnable by hand: docker run --rm -v "$PWD:/src" <image> bash /src/ci/build-rpm.sh
set -euo pipefail
cd "$(dirname "$0")/.."
SPEC=mhvtl-utils.spec

. /etc/os-release; echo "::group::$PRETTY_NAME - install tooling"
if command -v dnf >/dev/null; then
	INSTALL="dnf -y install"; $INSTALL 'dnf-command(builddep)' || true
elif command -v zypper >/dev/null; then
	INSTALL="zypper --non-interactive install --no-recommends"
	zypper --non-interactive refresh
else
	echo "no dnf/zypper" >&2; exit 1
fi
$INSTALL rpm-build gcc make tar gzip gawk findutils sed
# rpmlint is informational and not in every base repo (e.g. EL10 ships it only
# via EPEL/CRB), so never let its absence fail the build.
$INSTALL rpmlint || echo "rpmlint unavailable - lint step will be skipped"
echo "::endgroup::"

echo "::group::assemble source tarball"
VER=$(awk '/^Version:/ {print $2}' "$SPEC")
echo "version=$VER"
# Pin _topdir: openSUSE defaults it to /usr/src/packages, EL to ~/rpmbuild;
# fix it so the tarball location and the rpmbuild lookup always agree.
TOPDIR="$PWD/rpmbuild"
mkdir -p "$TOPDIR"/{SOURCES,SPECS,BUILD,BUILDROOT,RPMS,SRPMS}
# Name the tarball as Source: expects and give it the %setup -n top dir.
tar czf "$TOPDIR/SOURCES/mhvtl-$VER.tgz" \
	--transform "s,^\.,mhvtl-$VER," \
	--exclude=.git --exclude=./ci --exclude=./rpmbuild --exclude=./dist .
# Additional sources referenced by the spec (Source2: the KMP preamble).
cp mhvtl.preamble "$TOPDIR/SOURCES/"
echo "::endgroup::"

echo "::group::install build requires (from spec)"
# rpmspec reads the spec's BuildRequires, including pkgconfig(...) virtual
# provides, which both dnf and zypper resolve to the right package.
# On openSUSE the spec builds kernel module packages. Their subpackage macro
# comes from kernel-macros and only expands once a kernel flavor to build
# against is installed (OBS installs those before parsing the spec), so install
# the macro's own build requirements (kernel-syms etc.) first.
if command -v zypper >/dev/null; then
	$INSTALL kernel-macros
	# shellcheck disable=SC2046 # word splitting is intended: a package list
	$INSTALL $(rpm --eval '%kernel_module_package_buildreqs')
fi
mapfile -t BR < <(rpmspec --define "_topdir $TOPDIR" -q --buildrequires "$SPEC" | sed 's/[[:space:]].*//')
printf '%s\n' "${BR[@]}"
[ "${#BR[@]}" -gt 0 ] && $INSTALL "${BR[@]}"
echo "::endgroup::"

echo "::group::fetch remote sources"
# Sources given as URLs (on EL, Source3: the tcmu-runner release libtcmu is
# built from). Parsing the spec needs the kernel macros installed above. The
# EL images ship curl-minimal, which installing curl would conflict with, so
# only install curl where none is present.
command -v curl >/dev/null || $INSTALL curl
rpmspec --define "_topdir $TOPDIR" -P "$SPEC" |
	awk '/^Source[0-9]*:[[:space:]]*[a-z]+:\/\//{print $2}' |
	while read -r url; do
		echo "fetch $url"
		curl -fsSL --retry 3 -o "$TOPDIR/SOURCES/${url##*/}" "$url"
	done
echo "::endgroup::"

echo "::group::rpmbuild"
rpmbuild --define "_topdir $TOPDIR" -bb "$SPEC"
echo "::endgroup::"

echo "::group::built RPMs"; find "$TOPDIR/RPMS" -name '*.rpm' -printf '%f\n'; echo "::endgroup::"
echo "::group::rpmlint (informational)"
if command -v rpmlint >/dev/null; then rpmlint "$TOPDIR"/RPMS/*/*.rpm || true; else echo "rpmlint not installed - skipped"; fi
echo "::endgroup::"

# Expose artifacts to the mounted workspace for upload.
mkdir -p dist/rpm && find "$TOPDIR/RPMS" -name '*.rpm' -exec cp {} dist/rpm/ \;
echo "RPM_BUILD_OK"
