#!/bin/bash
# Build build/xfstests-rootfs.img: an Ubuntu 24.04 root filesystem with a
# pinned xfstests release (patched to know FSTYP=ks3fs) installed in
# /var/lib/xfstests.  Built in a container; the ext4 image is made with
# mke2fs -d inside it, so ownership is right without root on the host.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
TAG=${XFSTESTS_TAG:-v2026.09.22}
IMG=$ROOT/build/xfstests-rootfs.img
STAMP=$ROOT/build/xfstests-rootfs.stamp
want="$TAG $(sha256sum "$0" | cut -c1-16)"
[ -f "$IMG" ] && [ "$(cat "$STAMP" 2>/dev/null)" = "$want" ] && exit 0
ENGINE=$(command -v docker || command -v podman)
mkdir -p "$ROOT/build"
$ENGINE run --rm -v "$ROOT/build":/out ubuntu:24.04 bash -euc "
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq >/dev/null
apt-get install -y -qq --no-install-recommends ca-certificates git build-essential \
	autoconf automake libtool pkg-config libaio-dev uuid-dev xfslibs-dev libattr1-dev \
	libacl1-dev liburing-dev libgdbm-dev libgdbm-compat-dev libcap-dev \
	xfsprogs e2fsprogs attr acl bc gawk psmisc python3 perl sed util-linux \
	uuid-runtime procps kmod fio sqlite3 coreutils findutils diffutils file \
	iproute2 busybox-static >/dev/null
git clone -q --depth 1 --branch $TAG https://git.kernel.org/pub/scm/fs/xfs/xfstests-dev.git /x
cd /x
# teach xfstests about ks3fs: a network filesystem handled like virtiofs
# (no mkfs/fsck, a plain string as the 'device', scratch emptied by rm)
sed -i -E 's/virtiofs(\\)|\\|)/virtiofs|ks3fs\\1/g' common/rc common/config
grep -c ks3fs common/rc
make -j\$(nproc) >/dev/null 2>&1
make install >/dev/null 2>&1
mkdir -p /mnt/test /mnt/scratch
# copy the root filesystem (one file system only) and make the image
mkdir /rootfs
tar -C / --one-file-system --exclude=./rootfs --exclude=./x --exclude=./out \
	--exclude=./proc --exclude=./sys --exclude=./dev -cf - . | tar -C /rootfs -xf -
rm -rf /rootfs/var/cache/apt/archives/*.deb /rootfs/var/lib/apt/lists/*
mkdir -p /rootfs/proc /rootfs/sys /rootfs/dev /rootfs/tmp /rootfs/run
mke2fs -q -t ext4 -L xfstests-root -d /rootfs /out/xfstests-rootfs.img 6G
"
echo "$want" >"$STAMP"
ls -la "$IMG"
