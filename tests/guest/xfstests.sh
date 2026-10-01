# xfstests with FSTYP=ks3fs, run from an Ubuntu root filesystem (virtio
# disk, snapshot mode) that the busybox guest chroots into.
SUITE=xfstests
. /tests/lib.sh
R=/newroot
mkdir -p $R
check "mount the xfstests root disk" mount -t ext4 /dev/vda $R
for d in proc sys dev; do mount --bind /$d $R/$d; done
mount -t devpts devpts $R/dev/pts 2>/dev/null
mount -t tmpfs tmp $R/tmp
cp /etc/ks3fs-test.env $R/etc/ks3fs-test.env
cp -r /tests $R/
chroot $R /bin/bash /tests/xfstests-inner.sh
rc=$?
umount -l $R 2>/dev/null
[ $rc -eq 0 ] || FAIL=$((FAIL + 1))
finish
