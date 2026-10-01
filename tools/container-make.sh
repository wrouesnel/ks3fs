#!/bin/bash
# Build the module in a container that has the compiler a kernel was built
# with (mainline kernels use newer compilers and host tools than Noble has).
#   tools/container-make.sh <kdir> <srcdir> <compiler, e.g. x86_64-linux-gnu-gcc-15>
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
KDIR=$(realpath "$1") SRC=$(realpath "$2") CC=$3
IMAGE=${BUILD_IMAGE:-ubuntu:26.04}
ENGINE=$(command -v docker || command -v podman)
pkg=${CC#x86_64-linux-gnu-}	# gcc-15
$ENGINE run --rm -v "$ROOT":"$ROOT" -w "$SRC" "$IMAGE" bash -euc "
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq >/dev/null
apt-get install -y -qq --no-install-recommends $pkg make libelf1 libssl3t64 >/dev/null 2>&1 ||
	apt-get install -y -qq --no-install-recommends $pkg make libelf1 >/dev/null
make KDIR=$KDIR CC=$CC
" 2>&1 | grep -v '^Emulate'
