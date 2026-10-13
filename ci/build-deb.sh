#!/bin/bash
# Build the mhvtl .deb packages in a clean Debian/Ubuntu container and run
# lintian. Invoked by .github/workflows/packages.yml and runnable by hand:
#   docker run --rm -v "$PWD:/src:ro" <image> bash /src/ci/build-deb.sh
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
. /etc/os-release; echo "::group::$PRETTY_NAME - install tooling"
apt-get update
apt-get install -y --no-install-recommends \
	build-essential devscripts equivs dpkg-dev lintian ca-certificates
echo "::endgroup::"

# dpkg-buildpackage writes artifacts to the parent directory, so build from a
# writable copy of the (possibly read-only) mounted tree.
SRC="$(cd "$(dirname "$0")/.." && pwd)"
BUILD=/tmp/mhvtl-build
rm -rf "$BUILD"; mkdir -p "$BUILD"; cp -a "$SRC"/. "$BUILD"/; cd "$BUILD"

echo "::group::install build-deps (debian/control)"
mk-build-deps --install --remove --tool 'apt-get -y --no-install-recommends' debian/control
echo "::endgroup::"

echo "::group::dpkg-buildpackage -b"
dpkg-buildpackage -us -uc -b
echo "::endgroup::"

echo "::group::built debs"; ls -1 ../*.deb; echo "::endgroup::"
echo "::group::lintian (informational)"; lintian ../*.deb || true; echo "::endgroup::"

# Expose artifacts to the mounted workspace for upload (when writable).
if [ -w "$SRC" ]; then mkdir -p "$SRC/dist/deb" && cp ../*.deb "$SRC/dist/deb/"; fi
echo "DEB_BUILD_OK"
