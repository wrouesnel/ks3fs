#!/bin/bash
# Runs inside the Ubuntu chroot: configure and run xfstests on ks3fs.
set -a
. /etc/ks3fs-test.env
set +a
cd /var/lib/xfstests || exit 1
mkdir -p /mnt/test /mnt/scratch
opts="-o addr=10.0.2.2,port=$S3_PORT,access_key=$S3_AK,secret_key=$S3_SK"
cat > local.config <<CONF
export FSTYP=ks3fs
export TEST_DEV=xfstest
export TEST_DIR=/mnt/test
export SCRATCH_DEV=xfscratch
export SCRATCH_MNT=/mnt/scratch
export MOUNT_OPTIONS="$opts"
export TEST_FS_MOUNT_OPTS="$opts"
CONF
args=${XFSTESTS_ARGS:--g quick}
# check wants options before test names
[ -f /tests/xfstests.exclude ] && args="-E /tests/xfstests.exclude $args"
echo "# xfstests $args"
# a test that makes no progress for STALL_SECS gets the kernel's view of
# every CPU and blocked task dumped to the console (sysrq l and w)
STALL_SECS=${XFSTESTS_STALL_SECS:-900}
(
	echo 1 > /proc/sys/kernel/sysrq
	while sleep 60; do
		age=$(( $(date +%s) - $(stat -c %Y /tmp/check.out 2>/dev/null || date +%s) ))
		[ "$age" -ge "$STALL_SECS" ] || continue
		echo "#   no progress for ${age}s: dumping kernel state"
		echo 8 > /proc/sys/kernel/printk
		echo l > /proc/sysrq-trigger
		echo w > /proc/sysrq-trigger
		touch /tmp/check.out	# next dump after another STALL_SECS
	done
) &
watchdog=$!
# shellcheck disable=SC2086
# stream progress to the console as it happens (per-test lines)
./check $args 2>&1 | tee /tmp/check.out | sed -u 's/^/#   /'
rc=${PIPESTATUS[0]}
kill $watchdog 2>/dev/null
grep -E "^(Ran|Not run|Failures|Failed|Passed)" /tmp/check.out | sed 's/^/# /'
for t in $(sed -n 's/^Failures: //p' /tmp/check.out); do
	echo "not ok - xfstests $t"
	if [ -f "results/$t.out.bad" ]; then
		diff -u "tests/$t.out" "results/$t.out.bad" 2>/dev/null | head -25 | sed 's/^/#   /'
	fi
	sed 's/^/#   /' "results/$t.full" 2>/dev/null | tail -8
done
n=$(sed -n 's/^Ran: //p' /tmp/check.out | wc -w)
echo "# xfstests: ran $n tests, exit $rc"
exit $rc
