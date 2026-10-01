#!/bin/bash
# Build Ubuntu Noble's kernel source with KASAN/lockdep/kmemleak for the
# sanitizer test run.  Result: build/kernels/<name>/{vmlinuz,kdir}, usable
# as "tests/run.sh <name>".  The source comes from the linux-source-6.8.0
# package and is never modified (out-of-tree O= build).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
NAME=${1:-6.8.0-debug}
SRCPKG=${SRCPKG:-linux-source-6.8.0}
OUT=$ROOT/build/kernels/$NAME
SRC=$ROOT/build/noble-linux
OBJ=$OUT/obj
mkdir -p "$OUT" "$ROOT/build/debs"

if [ ! -f "$SRC/Makefile" ]; then
	(cd "$ROOT/build/debs" && ls "$SRCPKG"_*.deb >/dev/null 2>&1 || apt-get download "$SRCPKG")
	tmp=$(mktemp -d)
	dpkg-deb -x "$ROOT"/build/debs/"$SRCPKG"_*.deb "$tmp"
	tar -C "$tmp" -xjf "$tmp"/usr/src/linux-source-*/linux-source-*.tar.bz2
	mv "$tmp"/linux-source-* "$SRC"
	rm -rf "$tmp"
fi

mkdir -p "$OBJ"
if [ ! -f "$OBJ/.config" ]; then
	# start from Ubuntu's generic config so behaviour matches the shipped kernel
	base=$(ls -d "$ROOT"/build/kernels/6.8.0-*-generic/headers/usr/src/linux-headers-*-generic 2>/dev/null | sort -V | tail -1)
	[ -n "$base" ] || { "$ROOT/vm/fetch-kernel.sh" "$("$ROOT/tests/resolve-kernels.sh" | grep '^6\.8\.' | tail -1)" --headers >/dev/null;
		base=$(ls -d "$ROOT"/build/kernels/6.8.0-*-generic/headers/usr/src/linux-headers-*-generic | sort -V | tail -1); }
	cp "$base/.config" "$OBJ/.config"
	"$SRC/scripts/kconfig/merge_config.sh" -m -O "$OBJ" "$OBJ/.config" "$ROOT/vm/debug.config"
	make -C "$SRC" O="$OBJ" olddefconfig
	# built-ins only; the test VM loads nothing but ks3fs
	make -C "$SRC" O="$OBJ" localyesconfig LSMOD=/dev/null >/dev/null 2>&1 || true
	"$SRC/scripts/config" --file "$OBJ/.config" -e VIRTIO_NET -e VIRTIO_PCI -e MODULES \
		-e TLS -e NET_HANDSHAKE
	make -C "$SRC" O="$OBJ" olddefconfig
fi
make -C "$SRC" O="$OBJ" -j"$(nproc)" CC="${CC:-x86_64-linux-gnu-gcc-13}" bzImage modules_prepare
cp "$OBJ/arch/x86/boot/bzImage" "$OUT/vmlinuz"
# no in-tree modules are built, so vmlinux's symbol CRCs are the full set
[ -f "$OBJ/Module.symvers" ] || cp "$OBJ/vmlinux.symvers" "$OBJ/Module.symvers"
echo "$OBJ" >"$OUT/kdir"
echo "$OUT"
