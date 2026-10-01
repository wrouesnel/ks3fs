#!/bin/bash
# End-to-end test: build ks3fs for an Ubuntu kernel, start + seed an S3
# server on the host, boot the kernel in a KVM guest that runs the guest
# suites, then verify from the host what the guest wrote.
#
#   tests/run.sh [kver]            (default: 6.8.0-136-generic)
#   SUITES="rw rw-tls tls faults nix" S3_SERVER=minio|versitygw|rgw tests/run.sh
#   SUITES=nixstore tests/run.sh      (Nix store on ks3fs; slow, needs internet)
#   SUITES=big tests/run.sh           (6 GiB object in a 1 GiB guest; slow)
#   SUITES=stress tests/run.sh        (xfstests fsx + fsstress)
#   SUITES=xfstests XFSTESTS_ARGS="-g quick" tests/run.sh
#                                     (xfstests on an Ubuntu root disk; slow)
#
# Needs: qemu-system-x86_64, static busybox, aws CLI, curl, gcc and the
# chosen S3 server (see tests/servers.sh).
set -euo pipefail
# Run from a private copy: bash reads scripts as it goes, so editing this
# file during a long test run would otherwise corrupt the run.
if [ -z "${KS3_RUN_COPY:-}" ]; then
	copy=$(mktemp "${TMPDIR:-/tmp}/ks3fs-$(basename "$0").XXXXXX")
	cp "$0" "$copy"
	KS3_RUN_COPY=$copy KS3_RUN_SELF=$(realpath "$0") exec bash "$copy" "$@"
fi
rm -f "$KS3_RUN_COPY"	# bash already has it open
ROOT=$(cd "$(dirname "$KS3_RUN_SELF")/.." && pwd)
KVER=${1:-6.8.0-136-generic}
SUITES=${SUITES:-rw rw-tls tls faults keys nix}
OUT=$ROOT/build/out/$KVER
mkdir -p "$OUT"

log() { echo "[run] $*" >&2; }

. "$ROOT/tests/servers.sh"
cleanup() {
	[ -n "${FP_PID:-}" ] && kill "$FP_PID" 2>/dev/null
	stop_server
	rm -rf "${FIXTURES:-}"
}
has() { [[ " $SUITES " == *" $1 "* ]]; }
needs_server() { has rw || has rw-tls || has tls || has faults || has nixstore || has big || has stress || has keys || has xfstests; }
needs_tls() { has rw-tls || has tls || has faults; }
trap cleanup EXIT

# ---- build the module against the target kernel's headers
if [ -f "$ROOT/build/kernels/$KVER/kdir" ]; then
	# locally built kernel (e.g. vm/build-debug-kernel.sh)
	KDIR=$(cat "$ROOT/build/kernels/$KVER/kdir")
else
	KDIR=$("$ROOT/vm/fetch-kernel.sh" "$KVER" --headers)/headers/usr/src/linux-headers-$KVER
fi
KCC=$(sed -n 's/^CONFIG_CC_VERSION_TEXT="\([^ ]*\) .*/\1/p' "$KDIR/.config")
rm -rf "$OUT/src" && cp -r "$ROOT/src" "$OUT/src"
make -s -C "$OUT/src" clean KDIR="$KDIR" >/dev/null 2>&1 || true
if command -v "$KCC" >/dev/null; then
	log "building ks3fs for $KVER with $KCC"
	make -s -C "$OUT/src" KDIR="$KDIR" CC="$KCC" >"$OUT/build.log" 2>&1 || {
		cat "$OUT/build.log"; exit 1; }
else
	# e.g. mainline kernels, built with a newer compiler than Noble has
	log "building ks3fs for $KVER with $KCC (in a container)"
	"$ROOT/tools/container-make.sh" "$KDIR" "$OUT/src" "$KCC" >"$OUT/build.log" 2>&1 || {
		cat "$OUT/build.log"; exit 1; }
fi
if grep -E "warning:" "$OUT/build.log" | grep -vE "compiler differs|pahole version differs"; then
	log "build has warnings"; exit 1
fi

ENVF=$OUT/test.env
: >"$ENVF"
echo "KS3_SUITES=\"$SUITES\"" >>"$ENVF"
echo "KS3_UNLOAD=1" >>"$ENVF"
echo "KS3_DEBUG=${KS3_DEBUG:-}" >>"$ENVF"
echo "TLSHD_LOG_LINES=${TLSHD_LOG_LINES:-20}" >>"$ENVF"

# ---- guest helper binaries (static, kernel independent)
mkdir -p "$ROOT/build/guest-bin"
gcc -static -O2 -Wall -o "$ROOT/build/guest-bin/ks3test" "$ROOT/tests/guest-src/ks3test.c"
has stress && "$ROOT/tools/build-xfstests-tools.sh" >/dev/null

# the fixtures (thousands of objects) are only needed by these suites
needs_fixtures() { has rw || has rw-tls || has tls || has faults || has keys; }

seed_bucket() {
	local b=$1
	s3 s3api create-bucket --bucket "$b" >/dev/null
	needs_fixtures || return 0
	s3 s3 cp --quiet --recursive "$F" "s3://$b/"
	# filesystem-backed servers refuse a key below an existing object
	s3 s3api put-object --bucket "$b" --key shadow/inner >/dev/null 2>&1 || true
	s3 s3api put-object --bucket "$b" --key emptydir/ >/dev/null
}

if needs_server; then
	log "starting $S3_SERVER"
	"start_$S3_SERVER"

	F=$(mktemp -d)
	FIXTURES=$F
	mkdir -p "$F/dir1/sub" "$F/sp ace" "$F/many"
	echo "hello world" >"$F/hello.txt"
	echo a >"$F/dir1/a.txt"
	echo b >"$F/dir1/sub/b.txt"
	echo space >"$F/sp ace/file with space.txt"
	echo plus >"$F/plus+sign.txt"
	echo percent >"$F/pct%41.txt"
	echo unicode >"$F/unicodé.txt"
	echo start >"$F/append.txt"
	echo "old old old" >"$F/overwrite.txt"
	echo shadow-file >"$F/shadow"
	head -c 1048576 /dev/zero >"$F/rmw.bin"
	head -c 20000000 /dev/urandom >"$F/big.bin"
	for i in $(seq -w 1 2500); do : >"$F/many/f$i"; done
	BUCKETS="ks3test"
	has rw-tls && BUCKETS="$BUCKETS ks3tls"
	for b in $BUCKETS; do
		log "seeding bucket $b on port $PORT"
		seed_bucket "$b"
	done
	if has xfstests; then
		s3 s3api create-bucket --bucket xfstest >/dev/null
		s3 s3api create-bucket --bucket xfscratch >/dev/null
	fi
	if has nixstore; then
		# an empty bucket for the store, and the package source to build
		. "$ROOT/tests/nix/pins.env"
		s3 s3api create-bucket --bucket nixstore >/dev/null
		SRC=$ROOT/build/nix/hello-$HELLO_VERSION.tar.gz
		mkdir -p "$ROOT/build/nix"
		[ -f "$SRC" ] || curl -fsSL -o "$SRC" "$HELLO_URL"
		[ "sha256-$(openssl dgst -sha256 -binary "$SRC" | base64)" = "$HELLO_HASH" ] ||
			{ log "hello source hash mismatch"; exit 1; }
		s3 s3api create-bucket --bucket nixsrc >/dev/null
		s3 s3 cp --quiet "$SRC" s3://nixsrc/hello-src.tar.gz
	fi

	# expected root listing, sorted bytewise like busybox ls
	ROOT_LS=$(LC_ALL=C ls "$F" | { cat; echo emptydir; } | LC_ALL=C sort -u | tr '\n' ' ')
	{
		echo "S3_PORT=$PORT"
		echo "S3_AK=$AK"
		echo "S3_SK=$SK"
		echo "S3_BUCKET=ks3test"
		echo "S3_ROOT_LS=\"$ROOT_LS\""
		echo "BIG_SIZE=$(stat -c %s "$F/big.bin")"
		echo "BIG_SHA=$(sha256sum "$F/big.bin" | cut -d' ' -f1)"
		echo "BIG_SLICE_SHA=$(dd if="$F/big.bin" bs=4096 skip=1000 count=10 2>/dev/null | sha256sum | cut -d' ' -f1)"
	} >>"$ENVF"
fi

# ---- guest overlay: tlshd + libraries, a trust store, Nix configuration
OVL=$OUT/overlay
rm -rf "$OVL"
"$ROOT/vm/guest-tlshd.sh" "$OVL"
mkdir -p "$OVL/etc/ssl/certs" "$OVL/etc/nix" "$OVL/tests/nix"
cp /etc/ssl/certs/ca-certificates.crt "$OVL/etc/ssl/certs/ca-certificates.crt"
echo "root:x:0:0:root:/root:/bin/sh" >"$OVL/etc/passwd"
echo "root:x:0:" >"$OVL/etc/group"
echo "nameserver 10.0.2.3" >"$OVL/etc/resolv.conf"
cat >"$OVL/etc/nix/nix.conf" <<CONF
build-users-group =
sandbox = false
substituters = https://cache.nixos.org
trusted-public-keys = cache.nixos.org-1:6NCHdD59X431o0gWypbMrAURkbJ16ZPMQFGspcDShjY=
experimental-features = nix-command
max-jobs = 2
cores = 4
CONF
cp "$ROOT"/tests/nix/*.nix "$ROOT"/tests/nix/pins.env "$OVL/tests/nix/"
export GUEST_OVERLAYS=$OVL

if needs_tls; then
	# test PKI: a CA the guest trusts, and a rogue one it does not
	PKI=$OUT/pki
	rm -rf "$PKI" && mkdir -p "$PKI"
	mkcert() {	# mkcert <name> <ca-name>
		openssl req -x509 -newkey rsa:2048 -nodes -days 2 -subj "/CN=$2 test CA" \
			-keyout "$PKI/$2.key" -out "$PKI/$2.crt" 2>/dev/null
		openssl req -newkey rsa:2048 -nodes -subj "/CN=s3.ks3fs.test" \
			-keyout "$PKI/$1.key" -out "$PKI/$1.csr" 2>/dev/null
		printf 'subjectAltName=DNS:s3.ks3fs.test,IP:10.0.2.2,IP:127.0.0.1\n' >"$PKI/$1.ext"
		openssl x509 -req -in "$PKI/$1.csr" -CA "$PKI/$2.crt" -CAkey "$PKI/$2.key" \
			-CAcreateserial -days 2 -extfile "$PKI/$1.ext" -out "$PKI/$1.crt" 2>/dev/null
	}
	mkcert server ca
	mkcert rogue rogueca
	cat "$PKI/ca.crt" >>"$OVL/etc/ssl/certs/ca-certificates.crt"

	FP_CTL=$(free_port) FP_PLAIN=$(free_port) FP_TLS=$(free_port) FP_ROGUE=$(free_port)
	python3 "$ROOT/tests/tools/faultproxy.py" --upstream "127.0.0.1:$PORT" \
		--listen "$FP_PLAIN" --listen "$FP_TLS:$PKI/server.crt:$PKI/server.key" \
		--listen "$FP_ROGUE:$PKI/rogue.crt:$PKI/rogue.key" \
		--control "$FP_CTL" >"$OUT/faultproxy.log" 2>&1 &
	FP_PID=$!
	for _ in $(seq 50); do grep -q ready "$OUT/faultproxy.log" && break; sleep 0.1; done
	{
		echo "FP_CTL=$FP_CTL"
		echo "FP_PLAIN=$FP_PLAIN"
		echo "FP_TLS=$FP_TLS"
		echo "FP_ROGUE=$FP_ROGUE"
	} >>"$ENVF"
fi

if has nix || has tls || has nixstore; then
	NIX_ADDR=$(getent ahostsv4 nix-releases.s3.amazonaws.com | awk '{print $1; exit}')
	echo "NIX_ADDR=$NIX_ADDR" >>"$ENVF"
fi

if has stress; then
	export VM_TIMEOUT=${VM_TIMEOUT:-3000}
fi
if has xfstests; then
	# an Ubuntu root disk with xfstests (built once, cached)
	"$ROOT/tools/build-xfstests-rootfs.sh" >/dev/null
	export ROOT_DISK=$ROOT/build/xfstests-rootfs.img
	export VM_MEM=${VM_MEM:-4096} VM_CPUS=${VM_CPUS:-4} VM_TIMEOUT=${VM_TIMEOUT:-10800}
	echo "XFSTESTS_ARGS=\"${XFSTESTS_ARGS:--g quick}\"" >>"$ENVF"
fi
if has big; then
	# prove memory stays bounded: several times more data than guest RAM
	export VM_MEM=${VM_MEM:-1024} VM_TIMEOUT=${VM_TIMEOUT:-5400}
fi
if has nixstore; then
	# a compiler toolchain and a build want more room than the other suites
	export VM_MEM=${VM_MEM:-6144} VM_CPUS=${VM_CPUS:-4} VM_TIMEOUT=${VM_TIMEOUT:-5400}
fi

log "booting $KVER"
rc=0
"$ROOT/vm/run-vm.sh" "$KVER" "$OUT/src/ks3fs.ko" "$ENVF" "$OUT/console.log" >/dev/null || rc=1

verify_bucket() {	# what the rw suite left behind in bucket $1
	local b=$1
	want() {
		local got
		got=$(s3 s3 cp --quiet "s3://$b/$1" - 2>/dev/null | od -An -c | tr -d ' \n')
		[ "$got" = "$2" ] || { echo "host check $b/$1: got '$got' want '$2'"; fail=1; }
	}
	gone() {
		if s3 s3api head-object --bucket "$b" --key "$1" >/dev/null 2>&1; then
			echo "host check: $b/$1 should not exist"; fail=1
		fi
	}
	want victim.txt 'newcontent\n'
	want append.txt 'start\nmore\n'
	want trunc.txt 'hello\0\0\0'
	want dir1-moved/sub/b.txt 'b\n'
	want empty.txt ''
	gone new.txt
	gone renamed.txt
	gone dir1/a.txt
	gone newdir/
	# orphans of unlinked-while-open files are deleted at last close
	n=$(s3 s3api list-objects-v2 --bucket "$b" --prefix .ks3fs-orphans/ --query 'length(Contents || `[]`)')
	[ "$n" = 0 ] || { echo "host check: $b has $n leftover orphans"; fail=1; }
	s3 s3api head-object --bucket "$b" --key mp.bin --query ETag --output text | grep -q -- '-' ||
		{ echo "host check: $b/mp.bin was not a multipart upload"; fail=1; }
	[ "$(s3 s3api head-object --bucket "$b" --key mp.bin --query ContentLength)" = 25165824 ] ||
		{ echo "host check: $b/mp.bin size"; fail=1; }
	# s3fs-fuse compatible metadata: S_IFLNK|0777 = 41471, S_IFREG|0750 = 33256
	[ "$(s3 s3api head-object --bucket "$b" --key link --query Metadata.mode --output text)" = 41471 ] ||
		{ echo "host check: $b/link is not stored as a symlink"; fail=1; }
	want link 'hello.txt'
	[ "$(s3 s3api head-object --bucket "$b" --key meta.txt --query 'Metadata.[mode,uid,gid,mtime]' --output text | tr '\t' ' ')" = "33256 1234 5678 1" ] ||
		{ echo "host check: $b/meta.txt metadata"; fail=1; }
	[ "$(s3 s3api head-object --bucket "$b" --key upload.bin --query ContentLength)" = 50331648 ] ||
		{ echo "host check: $b/upload.bin size"; fail=1; }
	[ "$(s3 s3 cp --quiet "s3://$b/rmw.bin" - | dd bs=1 skip=500000 count=3 2>/dev/null)" = XYZ ] ||
		{ echo "host check: $b/rmw.bin"; fail=1; }
}

if [ $rc -eq 0 ] && needs_server; then
	log "verifying guest writes from the host"
	set +e
	fail=0
	has rw && verify_bucket ks3test
	has rw-tls && verify_bucket ks3tls
	[ $fail -eq 0 ] || rc=1
fi

# debugging aid: HOST_DEBUG='s3 s3api head-object --bucket ks3test --key x' ...
[ -n "${HOST_DEBUG:-}" ] && { set +e; eval "$HOST_DEBUG"; }

grep -a -E "^guest kernel:|^not ok|^# " "$OUT/console.log" || true
if [ $rc -eq 0 ]; then
	log "PASS ($KVER, $S3_SERVER)"
else
	log "FAIL ($KVER, $S3_SERVER) - see $OUT/console.log"
fi
exit $rc
