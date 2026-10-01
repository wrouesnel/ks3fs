# Nix with its store on ks3fs: install Nix from the official release
# tarball (read through a ks3fs mount of the public nix-releases bucket)
# into a ks3fs-backed /nix/store, substitute a stdenv from cache.nixos.org
# into it, compile GNU hello from source against it, and verify every store
# path's contents (NAR hashes cover file data, executable bits and symlinks)
# before and after a remount.
SUITE=nixstore
. /tests/lib.sh
set -a
. /tests/nix/pins.env
set +a
CREDS="access_key=$S3_AK,secret_key=$S3_SK"
STORE_OPTS="addr=10.0.2.2,port=$S3_PORT,$CREDS"
mkdir -p /nix/store /nix/var/nix /root /mnt/rel /mnt/src
export HOME=/root USER=root NIX_SSL_CERT_FILE=/etc/ssl/certs/ca-certificates.crt
t0=$(date +%s)
stamp() { echo "# [$(( $(date +%s) - t0 ))s] $*"; }

check "mount ks3fs at /nix/store" mount -t ks3fs -o $STORE_OPTS,ttl=30 nixstore /nix/store
check "mount nix-releases over HTTPS" mount -t ks3fs \
	-o addr=$NIX_ADDR,host=s3.amazonaws.com,vhost,region=eu-west-1,tls,ro nix-releases /mnt/rel
check "mount the source bucket" mount -t ks3fs -o addr=10.0.2.2,port=$S3_PORT,$CREDS,ro nixsrc /mnt/src

# ---- install Nix into the ks3fs store, the way the single-user installer does
T=/mnt/rel/nix/nix-$NIX_VERSION/nix-$NIX_VERSION-x86_64-linux.tar.xz
eq "release tarball checksum (read via ks3fs)" "$(sha $T)" "$(cat $T.sha256)"
check "unpack release tarball" sh -c "xz -dc $T | tar -x -C /tmp"
D=/tmp/nix-$NIX_VERSION-x86_64-linux
stamp "copying $(ls $D/store | wc -l) store paths into ks3fs"
check "copy the Nix closure into /nix/store" sh -c "for p in $D/store/*; do cp -a \$p /nix/store/ || exit 1; done"
stamp "copied"
# the release's own installer names its nix store path
NIX=$(sed -n 's/^nix="\(.*\)"$/\1/p' $D/install)
export PATH=$NIX/bin:$PATH
check "nix store path installed ($NIX)" test -d "$NIX"
if [ -n "$KS3_DEBUG" ]; then
	ls -d /nix/store/*nix-2* | sed 's/^/#   /'
	ls -l $NIX/bin | head -5 | sed 's/^/#   /'
	interp=$(strings $NIX/bin/nix | grep -m1 'ld-linux')
	echo "#   interpreter: $interp"
	ls -l "$interp" 2>&1 | sed 's/^/#   /'
	ls -l "$(dirname "$interp")" 2>&1 | head -8 | sed 's/^/#   /'
fi
eq "nix runs from ks3fs" "$(nix-store --version)" "nix-store (Nix) $NIX_VERSION"
check "register the closure (nix-store --load-db)" sh -c "nix-store --load-db < $D/.reginfo"
check "verify the installed closure" nix-store --verify --check-contents
rm -rf $D

# ---- substitute a toolchain and build from source
stamp "substituting stdenv"
check "substitute stdenv from cache.nixos.org" nix-store -r $STDENV
stamp "building hello"
check "nix-build GNU hello from source" nix-build /tests/nix/hello.nix \
	--argstr stdenv $STDENV --argstr shell $STDENV_SHELL \
	--arg src /mnt/src/hello-src.tar.gz -o /tmp/result
stamp "built"
eq "the built binary runs" "$(/tmp/result/bin/hello)" "Hello, world!"
check "hello was really built here" sh -c "nix-store -q --deriver \$(readlink /tmp/result) | grep -q hello-ks3fs"
eq "store paths are read-only" "$(stat -c %a $(readlink /tmp/result)/bin/hello)" "555"
eq "canonical mtime" "$(stat -c %Y $(readlink /tmp/result)/bin/hello)" "1"
stamp "verifying the whole store"
check "verify every store path" nix-store --verify --check-contents

# ---- a fresh mount must see the same store (metadata comes back via HEAD)
check "remount /nix/store" sh -c "umount /nix/store && mount -t ks3fs -o $STORE_OPTS nixstore /nix/store"
eq "hello runs after remount" "$(/tmp/result/bin/hello)" "Hello, world!"
stamp "verifying after remount"
check "verify every store path after remount" nix-store --verify --check-contents
stamp "done"
check "umount" sh -c "umount /mnt/src && umount /mnt/rel && umount /nix/store"
finish
