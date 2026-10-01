#!/bin/bash
# Build static fsx and fsstress from a pinned xfstests release, inside an
# Ubuntu 24.04 container (no build dependencies on the host), into
# build/guest-bin for the test VM.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
TAG=${XFSTESTS_TAG:-v2026.09.22}
OUT=$ROOT/build/guest-bin
mkdir -p "$OUT"
[ -x "$OUT/fsx" ] && [ -x "$OUT/fsstress" ] && [ "$(cat "$OUT/.xfstests-tag" 2>/dev/null)" = "$TAG" ] && exit 0
ENGINE=$(command -v docker || command -v podman)
$ENGINE run --rm -v "$OUT":/out ubuntu:24.04 bash -euc "
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq >/dev/null
apt-get install -y -qq --no-install-recommends ca-certificates git build-essential \
	autoconf automake libtool pkg-config libaio-dev uuid-dev xfslibs-dev \
	libattr1-dev libacl1-dev liburing-dev libgdbm-dev >/dev/null
git clone -q --depth 1 --branch $TAG https://git.kernel.org/pub/scm/fs/xfs/xfstests-dev.git /x
cd /x
make configure >/dev/null
./configure >/dev/null
make -C lib >/dev/null 2>&1 || true
# xfstests links through libtool, which will not link statically: do it by
# hand with the flags its own build uses
CFLAGS='-O2 -g -Iinclude -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 -std=gnu11 -funsigned-char -fno-strict-aliasing -DXFS -Isrc -DAIO -DURING -DFALLOCATE -DHAVE_COPY_FILE_RANGE -DNEED_INTERNAL_XFS_IOC_EXCHANGE_RANGE'
for t in fsx fsstress; do
	gcc -static \$CFLAGS -o /out/\$t ltp/\$t.c lib/.libs/libtest.a -laio -luring -lpthread
done
"
echo "$TAG" >"$OUT/.xfstests-tag"
file "$OUT/fsx" "$OUT/fsstress"
