# Read/write behaviour against an S3 server seeded by tests/run.sh (MinIO, RGW, versitygw).
SUITE=${SUITE:-rw}
. /tests/lib.sh
M=/mnt/a
M2=/mnt/b
# rw-tls.sh reruns this suite over TLS with its own bucket and options
OPTS=${RW_OPTS:-"addr=10.0.2.2,port=$S3_PORT,access_key=$S3_AK,secret_key=$S3_SK"}
B=${RW_BUCKET:-$S3_BUCKET}
mkdir -p $M $M2

# ---- mount handling
fails "mount with bad secret" mount -t ks3fs -o addr=10.0.2.2,port=$S3_PORT,access_key=$S3_AK,secret_key=wrong,retry_timeout=0 $B $M
fails "mount of missing bucket" mount -t ks3fs -o $OPTS no-such-bucket $M
fails "mount without addr" mount -t ks3fs -o port=$S3_PORT $B $M
check "mount" mount -t ks3fs -o $OPTS $B $M
check "secret not shown in /proc/mounts" sh -c "! grep -q secret_key /proc/mounts"
check "second mount of same bucket" mount -t ks3fs -o $OPTS,ttl=0 $B $M2

# ---- reads of seeded fixtures
eq "read small object" "$(cat $M/hello.txt)" "hello world"
eq "root listing" "$(for f in $M/*; do echo "${f#$M/}"; done | tr '\n' ' ')" "$S3_ROOT_LS"
eq "implicit dir listing" "$(ls $M/dir1 | tr '\n' ' ')" "a.txt sub "
check "implicit dir is a dir" test -d $M/dir1/sub
eq "nested read" "$(cat $M/dir1/sub/b.txt)" "b"
eq "key with spaces" "$(cat "$M/sp ace/file with space.txt")" "space"
eq "key with plus" "$(cat "$M/plus+sign.txt")" "plus"
eq "key with percent" "$(cat "$M/pct%41.txt")" "percent"
eq "key with unicode" "$(cat "$M/unicodé.txt")" "unicode"
eq "stat size" "$(stat -c %s $M/big.bin)" "$BIG_SIZE"
check "mtime is set" test "$(stat -c %Y $M/hello.txt)" -gt 1600000000
eq "big object sha256" "$(sha $M/big.bin)" "$BIG_SHA"
drop_caches
eq "slice read" "$(dd if=$M/big.bin bs=4096 skip=1000 count=10 2>/dev/null | sha256sum | cut -d' ' -f1)" "$BIG_SLICE_SHA"
eq "paginated listing (2500 keys)" "$(ls $M/many | wc -l)" "2500"
eq "ls -l of paginated dir" "$(ls -l $M/many | grep -c '^-')" "2500"
check "empty marker dir" test -d $M/emptydir
eq "empty dir listing" "$(ls -A $M/emptydir | wc -l)" "0"
check "file shadows same-named prefix" test -f $M/shadow
fails "missing file" cat $M/nope
fails "file as dir" ls $M/hello.txt/x

# concurrent readers of one object
for i in 1 2 3 4; do (sha $M/big.bin > /tmp/par.$i) & done
wait
eq "parallel readers" "$(cat /tmp/par.1 /tmp/par.2 /tmp/par.3 /tmp/par.4 | sort -u)" "$BIG_SHA"

# ---- writes
echo "new content" > $M/new.txt
eq "write then read back" "$(cat $M/new.txt)" "new content"
eq "write visible on other mount" "$(cat $M2/new.txt 2>&1)" "new content"
echo "replaced" > $M/overwrite.txt
eq "overwrite (O_TRUNC)" "$(cat $M2/overwrite.txt)" "replaced"
echo "more" >> $M/append.txt
eq "append (read-modify-write)" "$(cat $M2/append.txt | tr '\n' ' ')" "start more "
printf 'XYZ' | dd of=$M/rmw.bin bs=1 seek=500000 conv=notrunc 2>/dev/null
eq "in-place write keeps size" "$(stat -c %s $M2/rmw.bin)" "1048576"
eq "in-place write content" "$(dd if=$M2/rmw.bin bs=1 skip=499999 count=5 2>/dev/null | od -An -c | tr -d ' ')" '\0XYZ\0'
check "touch creates empty object" touch $M/empty.txt
eq "empty object size" "$(stat -c %s $M2/empty.txt)" "0"

dd if=/dev/urandom of=/tmp/up.bin bs=1M count=48 2>/dev/null
UP_SHA=$(sha /tmp/up.bin)
check "copy 48MB file in" cp /tmp/up.bin $M/upload.bin
drop_caches
eq "48MB upload read back" "$(sha $M/upload.bin)" "$UP_SHA"
eq "48MB upload via other mount" "$(sha $M2/upload.bin)" "$UP_SHA"
rm /tmp/up.bin

# truncate
cp $M/hello.txt $M/trunc.txt
check "truncate shrink" truncate -s 5 $M/trunc.txt
eq "shrunk content" "$(cat $M2/trunc.txt)" "hello"
check "truncate grow" truncate -s 8 $M/trunc.txt
eq "grown tail is zero" "$(od -An -c $M2/trunc.txt | tr -d ' ')" 'hello\0\0\0'

# fallocate: extend, keep-size no-op, punch and zero ranges, no collapse
odc() { od -An -c "$1" | tr -d ' \n'; }
printf 'abcdefghij' > $M/falloc.txt
check "fallocate extends" ks3test fallocate 0 0 16 $M/falloc.txt
eq "extended size" "$(stat -c %s $M2/falloc.txt)" 16
eq "extended tail is zero" "$(odc $M2/falloc.txt)" 'abcdefghij\0\0\0\0\0\0'
check "fallocate keep-size past EOF" ks3test fallocate keep 0 4096 $M/falloc.txt
eq "keep-size leaves the size" "$(stat -c %s $M/falloc.txt)" 16
check "punch a hole" ks3test fallocate keep,punch 2 3 $M/falloc.txt
eq "punched range reads zero" "$(odc $M2/falloc.txt)" 'ab\0\0\0fghij\0\0\0\0\0\0'
check "zero a range across EOF" ks3test fallocate zero 8 12 $M/falloc.txt
eq "zero range extends the file" "$(stat -c %s $M2/falloc.txt)" 20
eq "zeroed range" "$(odc $M2/falloc.txt)" 'ab\0\0\0fgh\0\0\0\0\0\0\0\0\0\0\0\0'
eq "collapse is refused" "$(ks3test fallocate collapse 0 4096 $M/falloc.txt)" "Operation not supported"

# shared writable mmap: stored when the last reference goes, without msync
printf 'hello mmap world' > $M/mmap.txt
check "store through a shared mapping" ks3test mmapwrite $M/mmap.txt 6 MMAP
eq "mapped store reaches the object" "$(cat $M2/mmap.txt)" "hello MMAP world"

# ---- multipart uploads: with 5 MiB parts, files of 10 MiB and more
# stream completed parts out while they are still being written
pat() { ks3test pattern "$@"; }
psha() { sh -c "$1" | sha256sum | cut -d' ' -f1; }
MP=/mnt/mp
mkdir -p $MP
check "mount with part_size=5" mount -t ks3fs -o $OPTS,part_size=5 $B $MP
check "write 23 MiB (streamed multipart)" sh -c "ks3test pattern 23M 7 > $MP/mp.bin"
eq "multipart content via other mount" "$(sha $M2/mp.bin)" "$(psha 'ks3test pattern 23M 7')"
check "append to a multipart object" sh -c "ks3test pattern 1M 8 >> $MP/mp.bin"
eq "appended content" "$(sha $M2/mp.bin)" "$(psha 'ks3test pattern 23M 7; ks3test pattern 1M 8')"
printf 'MIDDLE' | dd of=$MP/mp.bin bs=1 seek=12000000 conv=notrunc 2>/dev/null
eq "in-place write into a multipart object" "$(dd if=$M2/mp.bin bs=1 skip=12000000 count=6 2>/dev/null)" "MIDDLE"
eq "size after in-place write" "$(stat -c %s $M2/mp.bin)" "25165824"
check "mapped store into a multipart object" ks3test mmapwrite $MP/mp.bin 20000003 MAPPED
eq "mapped store via other mount" "$(dd if=$M2/mp.bin bs=1 skip=20000003 count=6 2>/dev/null)" "MAPPED"
eq "rest of the object unchanged" "$(dd if=$M2/mp.bin bs=1 skip=12000000 count=6 2>/dev/null)" "MIDDLE"
# read parts of a file that were already streamed out while it is still open
( exec 3>$MP/open.bin
  ks3test pattern 16M 9 >&3
  head -c 1048576 $MP/open.bin | sha256sum | cut -d' ' -f1 >/tmp/early
  ks3test pattern 4M 10 >&3 )
eq "read streamed parts of an open file" "$(cat /tmp/early)" "$(psha 'ks3test pattern 1M 9')"
eq "open file content after close" "$(sha $M2/open.bin)" "$(psha 'ks3test pattern 16M 9; ks3test pattern 4M 10')"
check "umount part_size mount" umount $MP

# ---- namespace
check "mkdir" mkdir $M/newdir
check "mkdir visible elsewhere" test -d $M2/newdir
echo f > $M/newdir/f
eq "file in new dir" "$(cat $M2/newdir/f)" "f"
fails "rmdir non-empty" rmdir $M/newdir
check "unlink" rm $M/newdir/f
fails "unlinked file gone" test -e $M2/newdir/f
check "rmdir empty" rmdir $M/newdir
fails "rmdir'd dir gone" test -e $M2/newdir
fails "mkdir existing" mkdir $M/dir1

exec 8<$M/new.txt
check "rename file" mv $M/new.txt $M/renamed.txt
drop_caches
# the open file reads through the ETag recorded from the copy
eq "renamed file readable through an open fd" "$(cat <&8)" "new content"
exec 8<&-
eq "renamed content" "$(cat $M2/renamed.txt)" "new content"
fails "old name gone" test -e $M2/new.txt
echo victim > $M/victim.txt
check "rename over existing" mv $M/renamed.txt $M/victim.txt
eq "target replaced" "$(cat $M2/victim.txt)" "new content"
check "rename dir" ks3test rename $M/dir1 $M/dir1-moved
eq "moved tree content" "$(cat $M2/dir1-moved/sub/b.txt)" "b"
fails "old dir gone" test -e $M2/dir1

# ---- unlink (and rename over) while open: the open file stays readable
ks3test pattern 3M 21 > $M/open-unlink.bin
exec 6<$M/open-unlink.bin
drop_caches	# the open file has nothing cached any more
check "unlink an open file" rm $M/open-unlink.bin
echo replacement > $M/open-unlink.bin
eq "open fd still reads the unlinked file" "$(sha256sum <&6 | cut -d' ' -f1)" "$(psha 'ks3test pattern 3M 21')"
exec 6<&-
eq "the new file is separate" "$(cat $M2/open-unlink.bin)" "replacement"
ks3test pattern 2M 22 > $M/open-over.bin
echo newer > $M/over-src.txt
exec 7<$M/open-over.bin
drop_caches
check "rename over an open file" mv $M/over-src.txt $M/open-over.bin
eq "open fd still reads the replaced file" "$(sha256sum <&7 | cut -d' ' -f1)" "$(psha 'ks3test pattern 2M 22')"
exec 7<&-
fails "orphans are hidden" test -e $M2/.ks3fs-orphans
eq "orphans are not listed" "$(ls -a $M2 | grep -c ks3fs-orphans)" "0"

# ---- directory rename in depth
mkdir -p $M/tree/a/b
echo f1 > $M/tree/a/f1
echo f2 > $M/tree/a/b/f2
ln -s a/f1 $M/tree/link
chmod 700 $M/tree/a/b
sync
exec 4<$M/tree/a/f1
check "rename a tree" ks3test rename $M/tree $M/tree2
eq "open file follows the rename" "$(cat <&4)" "f1"
exec 4<&-
eq "nested content moved" "$(cat $M2/tree2/a/b/f2)" "f2"
eq "symlink moved" "$(readlink $M2/tree2/link)" "a/f1"
eq "directory mode moved" "$(stat -c %a $M2/tree2/a/b)" "700"
fails "old tree gone" test -e $M2/tree
echo more > $M/tree2/a/b/f3
eq "cached dir still usable after rename" "$(cat $M2/tree2/a/b/f3)" "more"
mkdir -p $M/dest
check "rename into another directory" ks3test rename $M/tree2 $M/dest/tree3
eq "moved across parents" "$(cat $M2/dest/tree3/a/f1)" "f1"
mkdir -p $M/emptytarget $M/full/x
check "rename over an empty directory" ks3test rename $M/dest/tree3 $M/emptytarget
eq "replaced empty dir" "$(cat $M2/emptytarget/a/f1)" "f1"
eq "rename over a non-empty directory" "$(ks3test rename $M/emptytarget $M/full)" "Directory not empty"
# a writer that keeps unsaved data open (every close() would flush it)
sh -c 'printf busy; sleep 4' > $M/emptytarget/busy.txt &
sleep 1
eq "file being written below: EXDEV (mv copies)" "$(ks3test rename $M/emptytarget $M/busytarget)" "Invalid cross-device link"
wait
eq "the writer's data landed" "$(cat $M2/emptytarget/busy.txt)" "busy"
t0=$(date +%s)
check "rename a 2500-object directory" ks3test rename $M/many $M/many2
echo "#   moved 2500 objects in $(( $(date +%s) - t0 ))s"
eq "all objects moved" "$(ls $M2/many2 | wc -l)" "2500"
fails "old directory gone" test -e $M2/many

# ---- POSIX metadata (x-amz-meta-*), checked through the second mount
umask 022
echo m > $M/meta.txt
eq "new file mode follows umask" "$(stat -c %a $M2/meta.txt)" "644"
check "chmod" chmod 750 $M/meta.txt
check "chown" chown 1234:5678 $M/meta.txt
check "set mtime" touch -t 197001010000.01 $M/meta.txt
eq "local view is immediate" "$(stat -c '%a %u %g %Y' $M/meta.txt)" "750 1234 5678 1"
sync
eq "mode, owner and mtime stored" "$(stat -c '%a %u %g %Y' $M2/meta.txt)" "750 1234 5678 1"
eq "content unchanged by metadata update" "$(cat $M2/meta.txt)" "m"
printf '#!/bin/sh\necho executed\n' > $M/script.sh
check "chmod +x" chmod 755 $M/script.sh
sync
eq "run an executable from the store" "$($M2/script.sh)" "executed"
check "mkdir -m 700" mkdir -m 700 $M/private
sync	# busybox mkdir -m is mkdir + chmod; the chmod is written back lazily
eq "directory mode stored" "$(stat -c %a $M2/private)" "700"
check "chmod implicit directory (creates a marker)" sh -c "chmod 711 $M/sp\ ace && sync"
eq "implicit directory mode stored" "$(stat -c %a "$M2/sp ace")" "711"

check "symlink" ln -s hello.txt $M/link
eq "readlink via other mount" "$(readlink $M2/link)" "hello.txt"
eq "follow symlink" "$(cat $M2/link)" "hello world"
check "symlink type" test -L $M2/link
check "dangling symlink" ln -s /nonexistent/target $M/dangling
eq "dangling target" "$(readlink $M2/dangling)" "/nonexistent/target"
check "symlink to directory" ln -s dir1-moved/sub $M/sublink
eq "path through symlinked dir" "$(cat $M2/sublink/b.txt)" "b"
check "lchown symlink" chown -h 42:43 $M/link
sync
eq "symlink owner stored" "$(stat -c '%u %g' $M2/link)" "42 43"
check "unlink symlink" rm $M/dangling
fails "symlink gone" test -L $M2/dangling
fails "hardlink is refused" ln $M/hello.txt $M/hard
fails "mknod is refused" mknod $M/fifo p

# ---- ls -l on a fresh mount: attributes prefetched in parallel
mkdir -p $M/lsdir
for i in $(seq 1 40); do echo $i > $M/lsdir/f$i; done
ln -s f1 $M/lsdir/sym
chmod 700 $M/lsdir/f2
mkdir $M/lsdir/sub && chmod 750 $M/lsdir/sub
sync
mkdir -p /mnt/ls
check "fresh mount" mount -t ks3fs -o $OPTS $B /mnt/ls
ls -l /mnt/ls/lsdir > /tmp/ls.out
eq "ls -l entries" "$(grep -c '^[-ld]' /tmp/ls.out)" "42"
eq "ls -l shows the symlink" "$(grep ' sym -> f1$' /tmp/ls.out | cut -c1)" "l"
eq "ls -l shows a chmod-ed file" "$(grep ' f2$' /tmp/ls.out | cut -c1-10)" "-rwx------"
eq "ls -l shows a directory's mode" "$(grep ' sub$' /tmp/ls.out | cut -c1-10)" "drwxr-x---"
check "umount" umount /mnt/ls
for par in 16 1; do
	mount -t ks3fs -o $OPTS,parallel=$par $B /mnt/ls
	t0=$(date +%s)
	n=$(ls -l /mnt/ls/many2 | grep -c '^-')
	echo "#   ls -l of 2500 objects, parallel=$par: $(( $(date +%s) - t0 ))s ($n files)"
	umount /mnt/ls
done

# nometa keeps the old fixed-permission behaviour
mkdir -p /mnt/nm
check "nometa mount" mount -t ks3fs -o $OPTS,nometa $B /mnt/nm
fails "nometa: chmod is refused" chmod 777 /mnt/nm/hello.txt
fails "nometa: symlink is refused" ln -s hello.txt /mnt/nm/link2
check "nometa: umount" umount /mnt/nm

# ---- external modification becomes visible after ttl
echo v1 > $M/ext.txt
eq "ext v1" "$(cat $M/ext.txt)" "v1"
echo "version2" > $M2/ext.txt
sleep 2
eq "external rewrite visible after ttl" "$(cat $M/ext.txt)" "version2"
rm $M2/ext.txt
sleep 2
fails "external delete visible after ttl" cat $M/ext.txt

# ---- persistence across remount
check "umount" umount $M
check "remount" mount -t ks3fs -o $OPTS $B $M
eq "data persists" "$(cat $M/victim.txt)" "new content"
eq "metadata persists" "$(stat -c '%a %u %g %Y' $M/meta.txt)" "750 1234 5678 1"
eq "symlink persists" "$(readlink $M/link)" "hello.txt"
eq "directory mode persists" "$(stat -c %a $M/private)" "700"
check "umount both" sh -c "umount $M && umount $M2"
finish
