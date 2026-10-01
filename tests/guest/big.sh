# Objects bigger than guest RAM and than the 5 GiB single-PUT limit.  The
# guest has 1 GiB of RAM: without streaming multipart uploads this would
# pin the whole file in memory.
SUITE=big
. /tests/lib.sh
M=/mnt/big
M2=/mnt/big2
OPTS="addr=10.0.2.2,port=$S3_PORT,access_key=$S3_AK,secret_key=$S3_SK"
psha() { sh -c "$1" | sha256sum | cut -d' ' -f1; }
mkdir -p $M $M2
check "mount" mount -t ks3fs -o $OPTS $S3_BUCKET $M
check "second mount" mount -t ks3fs -o $OPTS,ttl=0 $S3_BUCKET $M2
echo "#   guest memory: $(grep MemTotal /proc/meminfo)"

t0=$(date +%s)
check "write 6 GiB" sh -c "ks3test pattern 6G 42 > $M/huge.bin"
echo "#   wrote 6 GiB in $(( $(date +%s) - t0 ))s"
eq "size" "$(stat -c %s $M2/huge.bin)" "6442450944"
t0=$(date +%s)
eq "6 GiB content" "$(sha $M2/huge.bin)" "$(psha 'ks3test pattern 6G 42')"
echo "#   read and hashed 6 GiB in $(( $(date +%s) - t0 ))s"

t0=$(date +%s)
check "append one byte (other parts copied server side)" sh -c "printf X >> $M/huge.bin"
echo "#   append took $(( $(date +%s) - t0 ))s"
eq "appended byte" "$(tail -c 1 $M2/huge.bin)" "X"
eq "head unchanged" "$(head -c 1048576 $M2/huge.bin | sha256sum | cut -d' ' -f1)" "$(psha 'ks3test pattern 1M 42')"

check "chmod a >5 GiB object" sh -c "chmod 600 $M/huge.bin && sync"
eq "mode stored (multipart copy onto itself)" "$(stat -c %a $M2/huge.bin)" "600"
check "rename a >5 GiB object" mv $M/huge.bin $M/huge2.bin
eq "renamed size" "$(stat -c %s $M2/huge2.bin)" "6442450945"
eq "renamed head" "$(head -c 1048576 $M2/huge2.bin | sha256sum | cut -d' ' -f1)" "$(psha 'ks3test pattern 1M 42')"
fails "old name gone" test -e $M2/huge.bin
check "remove it" rm $M/huge2.bin
check "umount" sh -c "umount $M && umount $M2"
finish
