# The request-saving defaults: lookups answered by directories this mount
# made (own_dir_ttl), files created by their first upload (lazy_create)
# and uploads held back after close (upload_delay_ms), so that a chmod or
# utimes after close goes with the data.  Request counts come from
# /proc/self/mountstats.
SUITE=deferred
. /tests/lib.sh
M=/mnt/d
M2=/mnt/d2
OPTS="addr=10.0.2.2,port=$S3_PORT,access_key=$S3_AK,secret_key=$S3_SK"
B=$S3_BUCKET/deferred	# its own prefix: rw checks the bucket root
mkdir -p $M $M2
P=/sys/module/ks3fs/parameters
eq "default own_dir_ttl" "$(cat $P/own_dir_ttl)" "60"
eq "default lazy_create" "$(cat $P/lazy_create)" "Y"
eq "default upload_delay_ms" "$(cat $P/upload_delay_ms)" "5000"
eq "default meta_delay_ms" "$(cat $P/meta_delay_ms)" "5000"
eq "default readdir_window_ms" "$(cat $P/readdir_window_ms)" "10000"
# a delay long enough that nothing goes out by itself during a check
ks3param upload_delay_ms 3000
check "mount" mount -t ks3fs -o $OPTS $B $M
check "second mount" mount -t ks3fs -o $OPTS,ttl=0 $B $M2
# delta NAME: how much counter NAME of $M grew since snap
snap() { for c in head list put copy delete; do eval "s_$c=$(stat_of $M $c)"; done; }
delta() { eval "echo \$(( $(stat_of $M $1) - s_$1 ))"; }

# ---- a store path's worth of files: data, mode and mtime in one PUT each
rm -rf $M/pkg 2>/dev/null
check "mkdir" mkdir $M/pkg
snap
# by name: a glob would list the directory
for i in $(seq 1 20); do echo "file $i" > $M/pkg/f$i; done
for i in $(seq 1 20); do chmod 444 $M/pkg/f$i; done
for i in $(seq 1 20); do touch -t 197001010000.01 $M/pkg/f$i; done
eq "creating in an own directory: no HEAD" "$(delta head)" "0"
eq "creating in an own directory: no LIST" "$(delta list)" "0"
eq "nothing sent before the delay" "$(delta put)" "0"
fails "not visible elsewhere yet" test -e $M2/pkg/f1
eq "listed here before it is stored" "$(ls $M/pkg | wc -l)" "20"
eq "stat here before it is stored" "$(stat -c '%s %a %Y' $M/pkg/f7)" "7 444 1"
fails "absent name in an own directory" test -e $M/pkg/nope
eq "  ...without a request" "$(delta head)" "0"
fails "rmdir with unstored files" rmdir $M/pkg
sleep 5
eq "after the delay: one PUT per file, one for the directory" "$(delta put)" "21"
eq "  ...and no CopyObject for chmod/utimes" "$(delta copy)" "0"
eq "content stored" "$(cat $M2/pkg/f12)" "file 12"
eq "mode and mtime stored with it" "$(stat -c '%a %Y' $M2/pkg/f12)" "444 1"
eq "listed elsewhere" "$(ls $M2/pkg | wc -l)" "20"

# ---- sync, fsync and unmount send what is queued
echo synced > $M/pkg/sync.txt
check "sync" sync
eq "sync stores queued files" "$(cat $M2/pkg/sync.txt)" "synced"
check "write with fsync" dd if=/dev/zero of=$M/pkg/fsync.bin bs=4k count=3 conv=fsync
eq "fsync stores at once" "$(stat -c %s $M2/pkg/fsync.bin)" "12288"
echo unmounted > $M/pkg/umount.txt
check "umount with queued uploads" umount $M
eq "unmount stores queued files" "$(cat $M2/pkg/umount.txt)" "unmounted"
check "remount" mount -t ks3fs -o $OPTS $B $M

# ---- an unstored file: written again, renamed, unlinked
mkdir $M/u
echo one > $M/u/a
echo two >> $M/u/a
snap
check "rename before it is stored" mv $M/u/a $M/u/b
eq "  ...costs nothing" "$(( $(delta copy) + $(delta delete) + $(delta put) ))" "0"
echo gone > $M/u/c
check "unlink before it is stored" rm $M/u/c
eq "  ...costs nothing" "$(( $(delta delete) + $(delta put) ))" "0"
check "sync" sync
eq "rewritten and renamed file stored once" "$(cat $M2/u/b | tr '\n' ' ')" "one two "
eq "  ...with one PUT (and one for the directory)" "$(delta put)" "2"
fails "old name never stored" test -e $M2/u/a
fails "unlinked file never stored" test -e $M2/u/c

# ---- directory rename with queued files: stored first, then moved
mkdir -p $M/t/sub
echo x > $M/t/sub/x
echo y > $M/t/y
check "rename a directory with queued files" ks3test rename $M/t $M/t2
eq "moved file content" "$(cat $M2/t2/sub/x)" "x"
eq "moved file content (top)" "$(cat $M2/t2/y)" "y"
fails "old tree gone" test -e $M2/t

# ---- directories and symlinks: stored once, with their final metadata
snap
mkdir -p $M/tree/sub
ln -s ../target $M/tree/link
chown -h 7:8 $M/tree/link
chmod 555 $M/tree/sub
touch -t 197001010000.01 $M/tree/sub
eq "mkdir, symlink and their metadata: nothing sent yet" "$(( $(delta put) + $(delta copy) ))" "0"
eq "unstored symlink reads back" "$(readlink $M/tree/link)" "../target"
eq "unstored entries are listed" "$(ls $M/tree | tr '\n' ' ')" "link sub "
check "unstored directory is a directory" test -d $M/tree/sub
fails "rmdir of a directory holding unstored entries" rmdir $M/tree
sleep 5
eq "after the delay: one PUT each (tree, sub, link)" "$(delta put)" "3"
eq "  ...and no CopyObject" "$(delta copy)" "0"
eq "directory mode and mtime stored" "$(stat -c '%a %Y' $M2/tree/sub)" "555 1"
eq "symlink target stored" "$(readlink $M2/tree/link)" "../target"
eq "symlink owner stored" "$(stat -c '%u %g' $M2/tree/link)" "7 8"

# ---- own_dir_ttl counts from this mount's last change to the directory
ks3param own_dir_ttl 2
mkdir $M/slide
sleep 1
: > $M/slide/a
sleep 1.5
snap
fails "absent name 2.5 s after mkdir, 1.5 s after the last change" test -e $M/slide/nope
eq "  ...still answered locally" "$(delta head)" "0"
sleep 2.5
fails "absent name after own_dir_ttl without changes" test -e $M/slide/nope2
check "  ...asks the store" test "$(delta head)" -ge 1
ks3param own_dir_ttl 60

# ---- the tunables
ks3param own_dir_ttl 0
mkdir $M/plain
sync	# its marker: not to be counted below
snap
fails "absent name, own_dir_ttl=0" test -e $M/plain/nope
check "  ...asks the store" test "$(delta head)" -ge 1
ks3param own_dir_ttl 60
ks3param lazy_create 0
ks3param upload_delay_ms 0
snap
echo eager > $M/plain/eager.txt
eq "lazy_create=0, upload_delay_ms=0: create and data PUTs" "$(delta put)" "2"
eq "  ...stored on close" "$(cat $M2/plain/eager.txt)" "eager"
ks3param lazy_create 1
snap
echo lazy > $M/plain/lazy.txt
eq "lazy_create=1, upload_delay_ms=0: one PUT on close" "$(delta put)" "1"
eq "  ...stored on close" "$(cat $M2/plain/lazy.txt)" "lazy"
ks3param meta_delay_ms 500
chmod 600 $M/plain/lazy.txt
sleep 2
eq "meta_delay_ms=500: chmod written back" "$(stat -c %a $M2/plain/lazy.txt)" "600"
ks3param readdir_window_ms 0
eq "readdir_window_ms=0: ls -l still right" "$(ls -l $M/pkg | grep -c '^-r--r--r--')" "20"

check "clean up the prefix" sh -c "rm -rf $M/* && sync"
check "umount both" sh -c "umount $M && umount $M2"
finish
