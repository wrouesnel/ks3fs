#!/bin/bash
# Build ks3fs-dkms_<version>_all.deb from debian/ (as Launchpad would) out of
# the working tree, uncommitted changes included.  Needs debhelper and
# dh-dkms.  The sources land in /usr/src/ks3fs-<ver> and DKMS builds the
# module for every installed kernel.
#   packaging/build-deb.sh [OUTDIR]
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-$ROOT/build/deb}
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

mkdir "$work/ks3fs"
cp -r "$ROOT"/{debian,src,tools,dkms.conf,README.md,COPYING} "$work/ks3fs/"
# a build tree may hold objects from local builds: ship sources only
find "$work/ks3fs/src" -type f ! -name '*.[ch]' ! -name Kbuild ! -name Makefile -delete
(cd "$work/ks3fs" && dpkg-buildpackage -b -us -uc >"$work/build.log" 2>&1) ||
	{ cat "$work/build.log"; exit 1; }
mv "$work"/ks3fs-dkms_*_all.deb "$OUT/"
ls "$OUT"/ks3fs-dkms_*_all.deb
