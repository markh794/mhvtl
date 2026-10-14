#!/bin/bash
#
# meson.build is the only place the mhvtl version is set. This script carries
# it into the files that must repeat it - the RPM spec's Version, and the
# newest entry of the RPM and Debian changelogs - or, with --check, fails when
# any of them disagrees (CI runs that).
#
#   scripts/sync-version.sh           update the spec and changelogs
#   scripts/sync-version.sh --check   verify only; exit 1 on a mismatch
#
# A new changelog entry is signed with $DEBFULLNAME/$DEBEMAIL, falling back to
# the git user. Edit its text before committing.

set -euo pipefail

cd "$(dirname "$0")/.."

SPEC=mhvtl-utils.spec
DEBCHANGELOG=debian/changelog

CHECK=
case "${1:-}" in
	--check) CHECK=1 ;;
	"") ;;
	*) echo "usage: $0 [--check]" >&2; exit 2 ;;
esac

version=$(awk -F"'" '/^[[:space:]]*version[[:space:]]*:/ {print $2; exit}' meson.build)
if [[ ! $version =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
	echo "error: no X.Y.Z project version in meson.build (got '$version')" >&2
	exit 1
fi

spec_version=$(awk '/^Version:/ {print $2; exit}' "$SPEC")
# Newest %changelog entry: "* <date> <name> <email> - <version>-<release>"
spec_changelog=$(awk '/^%changelog/ {f=1; next} f && /^\*/ {print $NF; exit}' "$SPEC")
spec_changelog=${spec_changelog%-*}
# Newest debian/changelog entry: "mhvtl (<version>[-<revision>]) <dist>; ..."
deb_version=$(sed -n '1s/^[^(]*(\([^)]*\)).*/\1/p' "$DEBCHANGELOG")
deb_version=${deb_version%-*}

if [[ -n $CHECK ]]; then
	rc=0
	for pair in "$SPEC Version:$spec_version" \
	            "$SPEC %changelog:$spec_changelog" \
	            "$DEBCHANGELOG:$deb_version"; do
		where=${pair%:*} have=${pair##*:}
		if [[ $have != "$version" ]]; then
			echo "error: $where has $have, meson.build has $version" >&2
			rc=1
		fi
	done
	if [[ $rc -ne 0 ]]; then
		echo "run scripts/sync-version.sh to update them" >&2
		exit 1
	fi
	echo "version $version: consistent"
	exit 0
fi

name=${DEBFULLNAME:-$(git config user.name || true)}
email=${DEBEMAIL:-$(git config user.email || true)}
if [[ -z $name || -z $email ]]; then
	echo "error: set DEBFULLNAME/DEBEMAIL or the git user to sign the changelog" >&2
	exit 1
fi

if [[ $spec_version != "$version" ]]; then
	# A new upstream version restarts the packaging release count.
	sed -i -e "s/^Version:.*/Version: $version/" \
	       -e "s/^Release:[[:space:]]*[0-9]*/Release: 1/" "$SPEC"
	echo "$SPEC: Version $spec_version -> $version, Release 1"
fi

if [[ $spec_changelog != "$version" ]]; then
	release=$(awk '/^Release:/ {print $2; exit}' "$SPEC")
	release=${release%%%*}
	entry="* $(LC_ALL=C date '+%a %b %d %Y') $name <$email> - $version-$release
- Update to $version.
"
	awk -v entry="$entry" '{print} /^%changelog/ {print entry}' "$SPEC" > "$SPEC.tmp"
	mv "$SPEC.tmp" "$SPEC"
	echo "$SPEC: added %changelog entry for $version-$release"
fi

if [[ $deb_version != "$version" ]]; then
	dist=$(sed -n '1s/^[^)]*) \([^;]*\);.*/\1/p' "$DEBCHANGELOG")
	{
		echo "mhvtl ($version) ${dist:-unstable}; urgency=medium"
		echo
		echo "  * New upstream release $version."
		echo
		echo " -- $name <$email>  $(LC_ALL=C date -R)"
		echo
		cat "$DEBCHANGELOG"
	} > "$DEBCHANGELOG.tmp"
	mv "$DEBCHANGELOG.tmp" "$DEBCHANGELOG"
	echo "$DEBCHANGELOG: added entry for $version"
fi
