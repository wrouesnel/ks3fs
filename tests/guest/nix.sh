# Read-only checks against the public nix-releases bucket (anonymous, vhost).
SUITE=nix
. /tests/lib.sh
M=/mnt/nix
V=nix/nix-2.18.1
mkdir -p $M

check "mount nix-releases" mount -t ks3fs \
	-o addr=$NIX_ADDR,host=s3.amazonaws.com,vhost,region=eu-west-1,ro \
	nix-releases $M
check "root lists nix/" test -d $M/nix
n=$(ls $M/nix | wc -l)
check "nix/ has many versions ($n)" test "$n" -ge 200
eq "install size" "$(stat -c %s $M/$V/install)" 4052
eq "install sha256 matches published" "$(sha $M/$V/install)" "$(cat $M/$V/install.sha256)"
eq "20MB tarball sha256 matches published" \
	"$(sha $M/$V/nix-2.18.1-x86_64-linux.tar.xz)" \
	"$(cat $M/$V/nix-2.18.1-x86_64-linux.tar.xz.sha256)"
drop_caches
eq "tarball sha256 after drop_caches" \
	"$(sha $M/$V/nix-2.18.1-x86_64-linux.tar.xz)" \
	"$(cat $M/$V/nix-2.18.1-x86_64-linux.tar.xz.sha256)"
# a read in the middle of the object (ranged GET, no readahead from 0)
drop_caches
a=$(dd if=$M/$V/nix-2.18.1-x86_64-linux.tar.xz bs=4096 skip=3000 count=16 2>/dev/null | sha256sum | cut -d' ' -f1)
b=$(dd if=$M/$V/nix-2.18.1-x86_64-linux.tar.xz bs=4096 2>/dev/null | dd bs=4096 skip=3000 count=16 2>/dev/null | sha256sum | cut -d' ' -f1)
eq "random-offset read matches sequential read" "$a" "$b"
check "manual/ is a directory" test -d $M/$V/manual
fails "write on ro mount" sh -c "echo x > $M/newfile"
fails "missing file" cat $M/$V/does-not-exist
check "umount" umount $M
finish
