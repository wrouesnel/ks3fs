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
[ -f /tests/xfstests.exclude ] && args="$args -E /tests/xfstests.exclude"
echo "# xfstests $args"
# shellcheck disable=SC2086
./check $args > /tmp/check.out 2>&1
rc=$?
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
