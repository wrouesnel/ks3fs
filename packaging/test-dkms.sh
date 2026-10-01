#!/bin/bash
# Install ks3fs-dkms in a clean Ubuntu 24.04 container alongside headers for
# the given kernels and check DKMS built a module for each.  Build-only: the
# module is never loaded outside a VM.
#   packaging/test-dkms.sh <deb> <kver>...
set -euo pipefail
DEB=$(readlink -f "$1"); shift
ENGINE=$(command -v docker || command -v podman)
HDRS=$(printf 'linux-headers-%s ' "$@")
$ENGINE run --rm -v "$(dirname "$DEB")":/deb:ro ubuntu:24.04 bash -euc "
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq >/dev/null
apt-get install -y -qq --no-install-recommends dkms kmod gcc-13 gcc-14 make $HDRS >/dev/null
apt-get install -y -qq --no-install-recommends /deb/$(basename "$DEB") 2>&1 | grep -E 'ks3fs|Error|error' || true
# the package builds for the running and newest kernels; the others are
# built by the kernels' own DKMS hooks (headers installed, boot), as here
for k in $*; do dkms autoinstall -k \$k >/dev/null 2>&1 || true; done
dkms status
rc=0
for k in $*; do
	f=\$(ls /lib/modules/\$k/updates/dkms/ks3fs.ko* 2>/dev/null | head -1)
	if [ -n \"\$f\" ] && [ \"\$(modinfo -F vermagic \"\$f\" | cut -d' ' -f1)\" = \"\$k\" ]; then
		echo \"dkms OK: \$k\"
	else
		echo \"dkms FAILED: \$k\"; cat /var/lib/dkms/ks3fs/*/build/make.log 2>/dev/null | tail -20; rc=1
	fi
done
apt-get remove -y -qq ks3fs-dkms >/dev/null
[ -z \"\$(dkms status ks3fs)\" ] || { echo 'dkms remove left state behind'; rc=1; }
exit \$rc
"
