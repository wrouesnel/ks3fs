#!/bin/bash
# Build ks3fs-dkms_<ver>_all.deb: sources in /usr/src/ks3fs-<ver>, registered
# with DKMS on install so the module is rebuilt for every installed kernel.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
VER=$(sed -n 's/^PACKAGE_VERSION="\(.*\)"/\1/p' "$ROOT/dkms.conf")
OUT=${1:-$ROOT/build/deb}
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT

SRC=$STAGE/usr/src/ks3fs-$VER
mkdir -p "$SRC/src" "$STAGE/DEBIAN" "$STAGE/usr/sbin" "$STAGE/usr/share/doc/ks3fs-dkms"
cp "$ROOT/dkms.conf" "$SRC/"
cp "$ROOT"/src/*.[ch] "$ROOT/src/Kbuild" "$ROOT/src/Makefile" "$SRC/src/"
install -m 755 "$ROOT/tools/mount.ks3fs" "$STAGE/usr/sbin/mount.ks3fs"
cp "$ROOT/README.md" "$STAGE/usr/share/doc/ks3fs-dkms/" 2>/dev/null || true

cat >"$STAGE/DEBIAN/control" <<CTRL
Package: ks3fs-dkms
Version: $VER
Architecture: all
Maintainer: ks3fs developers <noreply@example.invalid>
Depends: dkms (>= 3.0)
Recommends: linux-headers-generic, ktls-utils
Section: kernel
Priority: optional
Description: in-kernel S3 object store filesystem (DKMS)
 ks3fs mounts an S3-compatible bucket as a filesystem without FUSE.
CTRL

cat >"$STAGE/DEBIAN/postinst" <<POST
#!/bin/sh
set -e
if [ "\$1" = configure ]; then
	dkms add -m ks3fs -v $VER --rpm_safe_upgrade >/dev/null 2>&1 || true
	# build for every kernel with headers; failure for one kernel is not fatal
	for k in /lib/modules/*; do
		k=\${k##*/}
		[ -e /lib/modules/\$k/build ] || continue
		dkms install -m ks3fs -v $VER -k "\$k" || echo "ks3fs: build for \$k failed" >&2
	done
fi
POST
cat >"$STAGE/DEBIAN/prerm" <<PRERM
#!/bin/sh
set -e
dkms remove -m ks3fs -v $VER --all --rpm_safe_upgrade >/dev/null 2>&1 || true
PRERM
chmod 755 "$STAGE/DEBIAN/postinst" "$STAGE/DEBIAN/prerm"

mkdir -p "$OUT"
dpkg-deb --root-owner-group -Zxz -b "$STAGE" "$OUT/ks3fs-dkms_${VER}_all.deb" >/dev/null
echo "$OUT/ks3fs-dkms_${VER}_all.deb"
