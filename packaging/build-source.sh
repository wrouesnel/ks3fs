#!/bin/bash
# Source uploads for Launchpad: one orig tarball from git (without debian/),
# then a source package per Ubuntu series, versioned <upstream>-<rev>~<series>1
# so each series gets its own build of the same code.
#   packaging/build-source.sh [-k KEYID] [-r REF] OUTDIR [SERIES...]
# Without -k the packages are left unsigned (to check them locally).
# SIGN_COMMAND, if set, replaces gpg for signing (e.g. a non-interactive
# wrapper in CI).
# REF (default HEAD) is the commit or tag to package.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
KEY='' REF=HEAD
while getopts k:r: o; do
	case $o in
	k) KEY=$OPTARG ;;
	r) REF=$OPTARG ;;
	*) exit 2 ;;
	esac
done
shift $((OPTIND - 1))
mkdir -p "$1"
OUT=$(cd "$1" && pwd)
shift
[ $# -gt 0 ] || set -- noble

CHANGELOG=$(git -C "$ROOT" show "$REF:debian/changelog")
VERSION=$(dpkg-parsechangelog -l- -S Version <<<"$CHANGELOG")
UPVER=${VERSION%-*} REV=${VERSION##*-}
DKMSVER=$(git -C "$ROOT" show "$REF:dkms.conf" | sed -n 's/^PACKAGE_VERSION="\(.*\)"/\1/p')
[ "$DKMSVER" = "$UPVER" ] ||
	{ echo "dkms.conf says $DKMSVER but debian/changelog $UPVER" >&2; exit 1; }

# the same orig tarball for every series (Launchpad insists on that)
ORIG=$OUT/ks3fs_$UPVER.orig.tar.gz
git -C "$ROOT" archive --format=tar --prefix="ks3fs-$UPVER/" "$REF" -- . ':(exclude)debian' |
	gzip -n9 >"$ORIG"

export DEBFULLNAME DEBEMAIL
DEBFULLNAME=$(sed -n 's/^Maintainer: \(.*\) <.*/\1/p' "$ROOT/debian/control")
DEBEMAIL=$(sed -n 's/^Maintainer: .*<\(.*\)>/\1/p' "$ROOT/debian/control")
sign=(-us -uc)
[ -n "$KEY" ] && sign=("--sign-key=$KEY")
[ -n "$KEY" ] && [ -n "${SIGN_COMMAND:-}" ] && sign+=("--sign-command=$SIGN_COMMAND")

for series; do
	work=$(mktemp -d)
	tar -xzf "$ORIG" -C "$work"
	git -C "$ROOT" archive --format=tar "$REF" debian | tar -x -C "$work/ks3fs-$UPVER"
	cp "$ORIG" "$work/"
	(
		cd "$work/ks3fs-$UPVER"
		# ~ sorts below the plain version on purpose (-b allows it)
		dch -b --newversion "$UPVER-$REV~${series}1" --distribution "$series" \
			--force-distribution "Build for $series." >/dev/null
		# -d: the build dependencies are Launchpad's business
		dpkg-buildpackage -S -sa -d "${sign[@]}" >/dev/null
	)
	mv "$work"/*.dsc "$work"/*.debian.tar.* "$work"/*_source.changes \
		"$work"/*_source.buildinfo "$OUT/"
	rm -rf "$work"
	echo "$OUT/ks3fs_$UPVER-$REV~${series}1_source.changes"
done
