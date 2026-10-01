# Network failures, injected by tests/tools/faultproxy.py between the guest
# and the S3 server (plus a guest link-down), and signal handling.
SUITE=faults
. /tests/lib.sh
ctl() { wget -q -T 10 -O- "http://10.0.2.2:$FP_CTL/$1" >/dev/null; }
cuts() { wget -q -T 10 -O- "http://10.0.2.2:$FP_CTL/stats" | sed -n 's/.*"cuts": *\([0-9]*\).*/\1/p'; }
elapsed() { echo $(( $(date +%s) - t0 )); }
M=/mnt/f
M2=/mnt/f2
M3=/mnt/f3
mkdir -p $M $M2 $M3
CREDS="access_key=$S3_AK,secret_key=$S3_SK"
# parallel=1: these scenarios rely on one connection carrying the transfer
# (parallel readahead gets its own disconnect test below)
P="addr=10.0.2.2,port=$FP_PLAIN,$CREDS,timeout=3,parallel=1"

check "mount via fault proxy" mount -t ks3fs -o $P,retry_timeout=60 ks3test $M
ctl "set?mode=normal"

# ---- connections cut mid-transfer: GETs resume, PUTs restart
c0=$(cuts)
ctl "cut?dir=down&bytes=3000000&count=4"
drop_caches
eq "read survives 4 mid-body disconnects" "$(sha $M/big.bin)" "$BIG_SHA"
c1=$(cuts)
check "the proxy really cut connections ($c0 -> $c1)" test "$c1" -ge $((c0 + 4))

dd if=/dev/urandom of=/tmp/up.bin bs=1M count=8 2>/dev/null
c0=$(cuts)
ctl "cut?dir=up&bytes=2000000&count=2"
check "upload survives 2 mid-body disconnects" cp /tmp/up.bin $M/faults-up.bin
c1=$(cuts)
# budgets go to idle keep-alive connections first, which the upload may not
# reuse: insist only that at least one upload connection really was cut
check "the proxy really cut uploads ($c0 -> $c1)" test "$c1" -ge $((c0 + 1))
drop_caches
eq "uploaded data intact" "$(sha $M/faults-up.bin)" "$(sha /tmp/up.bin)"

# ---- every connection reset for a while
drop_caches
( sleep 0.3; ctl "set?mode=reset"; sleep 3; ctl "set?mode=normal" ) &
ctl "slow?bps=4000000"
eq "read through a reset storm" "$(sha $M/big.bin)" "$BIG_SHA"
ctl "slow?bps=0"
wait

# ---- traffic silently black-holed for longer than the socket timeout
drop_caches
ctl "slow?bps=4000000"
( sleep 1; ctl "set?mode=blackhole"; sleep 8; ctl "set?mode=normal" ) &
t0=$(date +%s)
eq "read through an 8s black hole" "$(sha $M/big.bin)" "$BIG_SHA"
check "and it really waited it out ($(elapsed)s)" test "$(elapsed)" -ge 8
ctl "slow?bps=0"
wait
check "outage was logged" sh -c "dmesg | grep -q 'not responding'"
check "recovery was logged" sh -c "dmesg | grep -q 'ks3fs: server .* OK'"

# ---- server down (connection refused) while writing
ctl "set?mode=refuse"
( sleep 5; ctl "set?mode=normal" ) &
check "create+write while the server is down for 5s" sh -c "echo refused > $M/faults-refuse.txt"
wait
eq "written data landed" "$(cat $M/faults-refuse.txt)" "refused"

# ---- the guest's own link goes down
drop_caches
ip link set eth0 down
# (taking the link down also drops the default route; put it back)
( sleep 5; ip link set eth0 up; ip route add default via 10.0.2.2 2>/dev/null ) &
eq "read across a 5s link outage" "$(cat $M/hello.txt)" "hello world"
wait

# ---- soft mounts give up with EIO once retry_timeout passes
check "soft mount (retry_timeout=6)" mount -t ks3fs -o $P,timeout=2,retry_timeout=6 ks3test $M2
drop_caches
ctl "set?mode=blackhole"
t0=$(date +%s)
cat $M2/big.bin >/dev/null 2>/tmp/soft.err
rc=$?
check "soft: read fails during a long outage" test $rc -ne 0
check "soft: error is EIO" grep -q "Input/output error" /tmp/soft.err
check "soft: gave up in bounded time ($(elapsed)s)" test "$(elapsed)" -le 20
ctl "set?mode=normal"
eq "soft: works again after the outage" "$(cat $M2/hello.txt)" "hello world"

# ---- hard mounts wait forever, but SIGKILL still gets through
check "hard mount" mount -t ks3fs -o $P,timeout=2,hard ks3test $M3
drop_caches
ctl "set?mode=blackhole"
cat $M3/big.bin >/dev/null &
pid=$!
sleep 8
check "hard: still waiting after 8s" kill -0 $pid
t0=$(date +%s)
kill -9 $pid
wait $pid 2>/dev/null
check "hard: SIGKILL ends the wait promptly ($(elapsed)s)" test "$(elapsed)" -le 4
fails "hard: process is gone" kill -0 $pid
ctl "set?mode=normal"
eq "hard: works again after the outage" "$(cat $M3/hello.txt)" "hello world"

# ---- ordinary signals must not turn into I/O errors
drop_caches
ctl "slow?bps=8000000"
eq "read under a 2ms SIGALRM storm" "$(ks3test sigread $M/big.bin 2>/tmp/sig.err | sha256sum | cut -d' ' -f1)" "$BIG_SHA"
check "signals were delivered during the read" grep -q "signals" /tmp/sig.err
sed 's/^/#   /' /tmp/sig.err
ctl "slow?bps=0"

# ---- parallel readahead: chunks cut mid-transfer are refetched
c0=$(cuts)
mount -t ks3fs -o $P,parallel=16 ks3test $M3
ctl "cut?dir=down&bytes=150000&count=10"
drop_caches
eq "parallel readahead survives disconnects" "$(sha $M3/big.bin)" "$BIG_SHA"
c1=$(cuts)
check "the proxy really cut parallel reads ($c0 -> $c1)" test "$c1" -ge $((c0 + 1))
umount $M3

# ---- latency: ls -l issues its HEADs in parallel
mkdir -p $M/lat
for i in $(seq 1 200); do : > $M/lat/f$i; done
ctl "delay?ms=20"
for par in 16 1; do
	mount -t ks3fs -o $P,parallel=$par ks3test $M3
	t0=$(now_ms)
	n=$(ls -l $M3/lat | grep -c '^-')
	eval "lat_$par=$(( $(now_ms) - t0 ))"
	umount $M3
	echo "#   ls -l of 200 files at 20ms latency, parallel=$par: $(eval echo \$lat_$par)ms ($n files)"
done
check "parallel prefetch is at least 3x faster" test $(( lat_16 * 3 )) -le "$lat_1"
# sequential reads: readahead windows are fetched as parallel ranged GETs
for par in 16 1; do
	mount -t ks3fs -o $P,parallel=$par ks3test $M3
	t0=$(now_ms)
	s=$(sha $M3/big.bin)
	eval "rd_$par=$(( $(now_ms) - t0 ))"
	umount $M3
	echo "#   20 MB read at 20ms latency, parallel=$par: $(eval echo \$rd_$par)ms"
	eq "parallel=$par read content" "$s" "$BIG_SHA"
done
ctl "delay?ms=0"
check "parallel readahead is at least 3x faster" test $(( rd_16 * 3 )) -le "$rd_1"

# ---- faults over TLS
check "TLS mount via fault proxy" sh -c "umount $M2 && mount -t ks3fs -o addr=10.0.2.2,port=$FP_TLS,$CREDS,host=s3.ks3fs.test,tls,timeout=3,parallel=1 ks3test $M2"
c0=$(cuts)
ctl "cut?dir=down&bytes=3000000&count=3&port=$FP_TLS"
drop_caches
eq "TLS read survives mid-record disconnects" "$(sha $M2/big.bin)" "$BIG_SHA"
c1=$(cuts)
check "the proxy really cut TLS connections ($c0 -> $c1)" test "$c1" -ge $((c0 + 3))

check "umount all" sh -c "umount $M && umount $M2 && umount $M3"
finish
