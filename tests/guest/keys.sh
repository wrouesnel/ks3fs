# Credentials from the kernel keyring (creds_key=): a "logon" key that user
# space cannot read back, re-read for every request so it can be rotated.
SUITE=keys
. /tests/lib.sh
M=/mnt/keys
mkdir -p $M
EP="addr=10.0.2.2,port=$S3_PORT"
GOOD=$(printf '%s\n%s' "$S3_AK" "$S3_SK")
id=$(ks3test addkey logon ks3fs:test "$GOOD")
check "logon key added ($id)" test "$id" -gt 0 2>/dev/null
check "mount with creds_key" mount -t ks3fs -o $EP,creds_key=test $S3_BUCKET $M
check "creds_key in /proc/mounts" grep -q "creds_key=test" /proc/mounts
check "no secret in /proc/mounts" sh -c "! grep -q -e secret -e '$S3_SK' /proc/mounts"
eq "read with keyring credentials" "$(cat $M/hello.txt)" "hello world"
echo keyed > $M/keyed.txt
eq "write with keyring credentials" "$(cat $M/keyed.txt)" "keyed"

ks3test addkey logon ks3fs:test "$(printf '%s\n%s' "$S3_AK" wrong-secret)" >/dev/null
drop_caches
fails "after rotating to a wrong secret, requests fail" cat $M/hello.txt
ks3test addkey logon ks3fs:test "$GOOD" >/dev/null
drop_caches
eq "after rotating back, requests work (no remount)" "$(cat $M/hello.txt)" "hello world"
check "revoke the key" ks3test revokekey "$id"
drop_caches
fails "with the key revoked, requests fail" cat $M/hello.txt
check "umount" umount $M

fails "mount with a missing key" mount -t ks3fs -o $EP,creds_key=nosuch $S3_BUCKET $M
fails "creds_key together with access_key is refused" \
	mount -t ks3fs -o $EP,creds_key=test,access_key=$S3_AK,secret_key=$S3_SK $S3_BUCKET $M
ks3test addkey logon ks3fs:bad "just-one-line" >/dev/null
fails "mount with a malformed key" mount -t ks3fs -o $EP,creds_key=bad,retry_timeout=0 $S3_BUCKET $M
check "malformed key is reported" sh -c "dmesg | grep -q 'credentials key must hold'"
finish
