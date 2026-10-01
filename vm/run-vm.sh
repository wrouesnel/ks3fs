#!/bin/bash
# Boot an Ubuntu kernel under KVM with ks3fs.ko and the guest test suites.
#   vm/run-vm.sh <kver> <ks3fs.ko> <env-file> [log]
# Exits 0 only if the guest reports success.
set -euo pipefail
# Run from a private copy: bash reads scripts as it goes, so editing this
# file during a long test run would otherwise corrupt the run.
if [ -z "${KS3_VM_COPY:-}" ]; then
	copy=$(mktemp "${TMPDIR:-/tmp}/ks3fs-$(basename "$0").XXXXXX")
	cp "$0" "$copy"
	KS3_VM_COPY=$copy KS3_VM_SELF=$(realpath "$0") exec bash "$copy" "$@"
fi
rm -f "$KS3_VM_COPY"	# bash already has it open
KVER=$1 MOD=$2 ENVF=$3 LOG=${4:-}
ROOT=$(cd "$(dirname "$KS3_VM_SELF")/.." && pwd)
KDIR=$ROOT/build/kernels/$KVER
[ -f "$KDIR/vmlinuz" ] || "$ROOT/vm/fetch-kernel.sh" "$KVER" >/dev/null

BUSYBOX=${BUSYBOX:-$(command -v busybox)}
file -L "$BUSYBOX" | grep -q "statically linked" || {
	echo "need a static busybox (apt install busybox-static)" >&2; exit 2; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
[ -n "$LOG" ] || LOG=$WORK.log
mkdir -p "$WORK"/{bin,sbin,etc,proc,sys,dev,tmp,tests}
cp "$BUSYBOX" "$WORK/bin/busybox"
ln -s busybox "$WORK/bin/sh"
cp "$ROOT/vm/init" "$WORK/init"
cp "$MOD" "$WORK/ks3fs.ko"
cp "$ENVF" "$WORK/etc/ks3fs-test.env"
cp "$ROOT"/tests/guest/* "$WORK/tests/"
for b in "$ROOT"/build/guest-bin/*; do [ -f "$b" ] && cp "$b" "$WORK/bin/"; done
[ -f "$KDIR/tls.ko" ] && cp "$KDIR/tls.ko" "$WORK/tls.ko"
# extra files for the guest root, e.g. tlshd and a CA bundle (colon separated)
IFS=: read -ra overlays <<<"${GUEST_OVERLAYS:-}"
for o in "${overlays[@]}"; do [ -d "$o" ] && cp -a "$o"/. "$WORK"/; done
chmod +x "$WORK/init"
(cd "$WORK" && find . | cpio -o -H newc --quiet | gzip -1) > "$WORK.cpio.gz"
trap 'rm -rf "$WORK" "$WORK.cpio.gz" "$WORK.log"' EXIT

ACCEL="-enable-kvm -cpu host"
[ -w /dev/kvm ] || ACCEL="-cpu max"	# TCG fallback (slow)

timeout "${VM_TIMEOUT:-900}" qemu-system-x86_64 $ACCEL -m "${VM_MEM:-2048}" -smp "${VM_CPUS:-2}" \
	-kernel "$KDIR/vmlinuz" -initrd "$WORK.cpio.gz" \
	-append "console=ttyS0 panic=-1 oops=panic softlockup_panic=1 hung_task_panic=1 loglevel=4" \
	-netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
	${ROOT_DISK:+-drive file=$ROOT_DISK,if=virtio,format=raw,snapshot=on} \
	-nographic -no-reboot 2>&1 | stdbuf -oL tr -d '\r' | tee "$LOG" | grep -v "^\[" || true

grep -q "^KS3FS-RESULT: 0$" "$LOG"
