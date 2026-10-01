# TLS behaviour: verification, failure modes and the handshake agent.
SUITE=tls
. /tests/lib.sh
M=/mnt/tls
mkdir -p $M
CREDS="access_key=$S3_AK,secret_key=$S3_SK"
T="addr=10.0.2.2,port=$FP_TLS,$CREDS"

echo "guest date: $(date -u)"
check "tlshd is running" kill -0 "$(cat /tmp/tlshd.pid)"
check "mount with verified certificate" mount -t ks3fs -o $T,host=s3.ks3fs.test,tls ks3test $M
check "is mounted" grep -q " $M ks3fs " /proc/mounts
check "tls in /proc/mounts" grep -q "ks3fs .*,tls," /proc/mounts
eq "read over TLS" "$(cat $M/hello.txt)" "hello world"
eq "20MB read over TLS" "$(sha $M/big.bin)" "$BIG_SHA"
dd if=/dev/urandom of=/tmp/t.bin bs=1M count=16 2>/dev/null
check "16MB write over TLS" cp /tmp/t.bin $M/tls-upload.bin
drop_caches
eq "16MB read back over TLS" "$(sha $M/tls-upload.bin)" "$(sha /tmp/t.bin)"
rm -f /tmp/t.bin
check "umount" umount $M

fails "tls without a DNS host= is refused" mount -t ks3fs -o $T,tls ks3test $M
check "and says why" sh -c "dmesg | grep -q 'tls needs host='"
fails "wrong peer name is rejected" mount -t ks3fs -o $T,host=wrong.ks3fs.test,tls,retry_timeout=0 ks3test $M
fails "untrusted CA is rejected" mount -t ks3fs -o addr=10.0.2.2,port=$FP_ROGUE,$CREDS,host=s3.ks3fs.test,tls,retry_timeout=0 ks3test $M
fails "plain HTTP to a TLS port fails" mount -t ks3fs -o $T,retry_timeout=2 ks3test $M

kill "$(cat /tmp/tlshd.pid)"; sleep 1
t0=$(date +%s)
fails "no tlshd: mount fails" mount -t ks3fs -o $T,host=s3.ks3fs.test,tls ks3test $M
check "no tlshd: fails fast" test $(( $(date +%s) - t0 )) -le 5
check "no tlshd: logged" sh -c "dmesg | grep -q 'tlshd) is not running'"
/usr/sbin/tlshd -s >>/tmp/tlshd.log 2>&1 &
echo $! >/tmp/tlshd.pid
sleep 1

# the real thing: AWS over HTTPS, public CA chain, wildcard certificate
if [ -n "$NIX_ADDR" ]; then
	V=nix/nix-2.18.1
	check "mount nix-releases over HTTPS" mount -t ks3fs \
		-o addr=$NIX_ADDR,host=s3.amazonaws.com,vhost,region=eu-west-1,tls,ro nix-releases $M
	check "is mounted" grep -q " $M ks3fs " /proc/mounts
	eq "AWS HTTPS read matches published sha256" "$(sha $M/$V/install)" "$(cat $M/$V/install.sha256)"
	eq "AWS HTTPS 20MB read" "$(sha $M/$V/nix-2.18.1-x86_64-linux.tar.xz)" \
		"$(cat $M/$V/nix-2.18.1-x86_64-linux.tar.xz.sha256)"
	check "umount" umount $M
fi
finish
