#!/bin/bash
# Stage tlshd (from Ubuntu's ktls-utils) and its shared-library closure into
# a guest overlay directory:  vm/guest-tlshd.sh <overlay-dir>
# The libraries are taken from the host, which must be Ubuntu Noble like the
# guest kernels (true for dev machines and ubuntu-24.04 CI runners).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DST=$(realpath -m "$1")
mkdir -p "$ROOT/build/debs" "$DST/usr/sbin" "$DST/etc"
cd "$ROOT/build/debs"
ls ktls-utils_*.deb >/dev/null 2>&1 || apt-get download ktls-utils >/dev/null
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
dpkg-deb -x ktls-utils_*.deb "$tmp"
cp "$tmp/usr/sbin/tlshd" "$DST/usr/sbin/tlshd"
ldd "$tmp/usr/sbin/tlshd" | awk '/=> \// {print $3} /^\t\/lib/ {print $1}' |
	while read -r lib; do
		mkdir -p "$DST$(dirname "$lib")"
		cp -L "$lib" "$DST$lib"
	done
if ldd "$tmp/usr/sbin/tlshd" | grep -q "not found"; then
	echo "missing host libraries for tlshd:" >&2
	ldd "$tmp/usr/sbin/tlshd" | grep "not found" >&2
	exit 1
fi
cat >"$DST/etc/tlshd.conf" <<CONF
[main]
debug=1
tlsdebug=${TLSHD_TLSDEBUG:-0}
nl_debug=0

[authenticate.client]
CONF
