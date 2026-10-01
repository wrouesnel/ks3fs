# fsx (random I/O checked against an in-memory model) and fsstress
# (concurrent random namespace and data operations) from xfstests.
SUITE=stress
. /tests/lib.sh
M=/mnt/st
M2=/mnt/st2
MP=/mnt/stmp
OPTS="addr=10.0.2.2,port=$S3_PORT,access_key=$S3_AK,secret_key=$S3_SK"
mkdir -p $M $M2 $MP
check "mount" mount -t ks3fs -o $OPTS $S3_BUCKET $M
check "second mount" mount -t ks3fs -o $OPTS,ttl=0 $S3_BUCKET $M2
check "part_size=5 mount" mount -t ks3fs -o $OPTS,part_size=5 $S3_BUCKET $MP
mkdir -p $M/fsx $M/stress

# mmap reads and writes are mixed in; everything else fsx probes
# for (fallocate, clone, dedupe...) is skipped when unsupported
run_fsx() {	# name args...
	n=$1; shift
	t0=$(date +%s)
	if fsx -q "$@" > /tmp/fsx.$n.out 2>&1; then
		ok "fsx $n ($(( $(date +%s) - t0 ))s)"
	else
		not_ok "fsx $n"
		# the failure itself, then the last operations before it
		sed -n '1,/LOG DUMP/p' /tmp/fsx.$n.out | tail -30 | sed 's/^/#   /'
		grep -v SKIPPED /tmp/fsx.$n.out | grep -E '^[0-9]+\(' | tail -12 | sed 's/^/#   /'
		dmesg | grep ks3fs: | tail -10 | sed 's/^/#   /'
		for f; do :; done	# the file is the last argument
		echo "#   size here: $(stat -c %s $f 2>&1), stored: $(stat -c %s $M2/${f#*/mnt/*/} 2>&1)"
	fi
}
run_fsx small -N 20000 -c 50 -S 1 $M/fsx/small
eq "fsx small: stored object matches" "$(sha $M2/fsx/small)" "$(sha $M/fsx/small)"
run_fsx medium -N 5000 -c 200 -l 4000000 -o 262144 -S 2 $M/fsx/medium
eq "fsx medium: stored object matches" "$(sha $M2/fsx/medium)" "$(sha $M/fsx/medium)"
# files of up to 8 parts: random writes cross streamed, frozen and copied parts
run_fsx multipart -N 3000 -c 100 -l 40000000 -o 1048576 -S 3 $MP/fsx/multipart
eq "fsx multipart: stored object matches" "$(sha $M2/fsx/multipart)" "$(sha $MP/fsx/multipart)"
run_fsx multipart-seq -N 2000 -l 40000000 -o 4194304 -S 4 $MP/fsx/multipart2
eq "fsx multipart-seq: stored object matches" "$(sha $M2/fsx/multipart2)" "$(sha $MP/fsx/multipart2)"

# fsstress: failures of unsupported operations are expected; the kernel log
# must stay clean (checked by init) and the filesystem must stay usable
t0=$(date +%s)
# time-bounded: on an object store every close of a (sparse, growing) file
# rewrites the whole object, so op rates are far below a local filesystem
fsstress -v -d $M/stress --duration=${FSSTRESS_SECS:-240} -p 4 -s 1 > /tmp/fsstress.out 2>&1 &
fpid=$!
while kill -0 $fpid 2>/dev/null && [ $(( $(date +%s) - t0 )) -lt ${FSSTRESS_LIMIT:-600} ]; do
	sleep 5
done
if kill -0 $fpid 2>/dev/null; then
	not_ok "fsstress finished within ${FSSTRESS_LIMIT:-600}s"
	echo "#   ops so far: $(grep -c '^[0-9]*/[0-9]*:' /tmp/fsstress.out)"
	for p in $(pidof fsstress); do
		echo "#   --- fsstress $p ($(cat /proc/$p/wchan 2>/dev/null)):"
		sed 's/^/#     /' /proc/$p/stack 2>/dev/null
		grep "^[0-9]*/" /tmp/fsstress.out | tail -3 | sed 's/^/#     /'
	done
	dmesg | grep ks3fs: | tail -15 | sed 's/^/#   /'
	killall -9 fsstress 2>/dev/null
	wait
else
	wait $fpid
	echo "#   fsstress: rc=$?, $(grep -c '^[0-9]*/[0-9]*:' /tmp/fsstress.out) ops in $(( $(date +%s) - t0 ))s"
fi
check "filesystem still usable" sh -c "echo alive > $M/stress/alive && cat $M/stress/alive"
check "the stress tree can be removed" rm -rf $M/stress
fails "and is gone" test -e $M2/stress
check "fsx files can be removed" rm -rf $M/fsx
check "umount" sh -c "umount $M && umount $M2 && umount $MP"
finish
