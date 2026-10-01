#!/bin/bash
# Fetch an Ubuntu kernel image (and optionally its headers) without
# installing anything:  vm/fetch-kernel.sh 6.8.0-136-generic [--headers]
# Output: build/kernels/<kver>/{vmlinuz,headers/}
set -euo pipefail
KVER=${1:?kernel version, e.g. 6.8.0-136-generic}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$ROOT/build/kernels/$KVER
mkdir -p "$OUT/debs"
cd "$OUT/debs"

MAINLINE=https://kernel.ubuntu.com/mainline
# mainline builds, e.g. 7.3.0-070300rc3-generic -> $MAINLINE/v7.3-rc3/amd64/
mainline_dir() {
	local v=${KVER%-generic} base rc
	base=${v%%-*}
	rc=$(sed -n 's/.*-[0-9]\{6\}\(rc[0-9]*\).*/\1/p' <<<"$v")
	base=${base%.0}
	echo "$MAINLINE/v$base${rc:+-$rc}/amd64"
}

fetch() {
	local pkg=$1 dir file
	ls "$pkg"_*.deb >/dev/null 2>&1 && return 0
	apt-get download "$pkg" >/dev/null 2>&1 && return 0
	if [[ $KVER =~ -[0-9]{6}(rc[0-9]+)?-generic$ ]]; then
		dir=$(mainline_dir)
		file=$(curl -fsSL "$dir/" | grep -oE "href=\"${pkg}_[^\"]*\.deb\"" |
		       sed 's/href="//;s/"$//' | head -1)
		[ -n "$file" ] && curl -fsSLO "$dir/$file" && return 0
	fi
	echo "cannot download $pkg" >&2
	return 1
}

if [ ! -f "$OUT/vmlinuz" ]; then
	fetch "linux-image-unsigned-$KVER"
	dpkg-deb -x linux-image-unsigned-"$KVER"_*.deb "$OUT/img"
	cp "$OUT/img/boot/vmlinuz-$KVER" "$OUT/vmlinuz"
	rm -rf "$OUT/img"
fi

# tls.ko (kTLS) is modular in Ubuntu kernels; keep just that, decompressed
if [ ! -f "$OUT/tls.ko" ]; then
	fetch "linux-modules-$KVER"
	tmp=$(mktemp -d)
	dpkg-deb --fsys-tarfile linux-modules-"$KVER"_*.deb |
		tar -x -C "$tmp" --wildcards '*/kernel/net/tls/tls.ko*'
	f=$(find "$tmp" -name 'tls.ko*' | head -1)
	case $f in
	*.zst) zstd -q -d -c "$f" >"$OUT/tls.ko" ;;
	*.xz)  xz -d -c "$f" >"$OUT/tls.ko" ;;
	*)     cp "$f" "$OUT/tls.ko" ;;
	esac
	rm -rf "$tmp"
fi

if [ "${2:-}" = "--headers" ] && [ ! -d "$OUT/headers" ]; then
	fetch "linux-headers-$KVER"
	# the flavour headers depend on a common package (e.g. linux-headers-6.8.0-136)
	common=$(dpkg-deb -f linux-headers-"$KVER"_*.deb Depends | tr ',' '\n' |
		 sed -n 's/^ *\(linux-[a-z0-9.-]*headers-[0-9][^ ]*\).*/\1/p' | head -1)
	fetch "$common"
	mkdir -p "$OUT/headers"
	for d in linux-headers-*.deb linux-*-headers-*.deb; do
		[ -f "$d" ] && dpkg-deb -x "$d" "$OUT/headers"
	done
fi
echo "$OUT"
