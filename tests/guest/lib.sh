# Tiny TAP-ish helpers for the busybox guest.
PASS=0
FAIL=0
ok()     { PASS=$((PASS + 1)); echo "ok - $*"; }
not_ok() { FAIL=$((FAIL + 1)); echo "not ok - $*"; }
# check "description" command...
check()  { d=$1; shift; if "$@" >/tmp/check.out 2>&1; then ok "$d"; else not_ok "$d"; sed 's/^/#   /' /tmp/check.out; fi; }
# fails "description" command...   (expects the command to fail)
fails()  { d=$1; shift; if "$@" >/dev/null 2>&1; then not_ok "$d (unexpectedly succeeded)"; else ok "$d"; fi; }
# eq "description" actual expected
eq()     { if [ "$2" = "$3" ]; then ok "$1"; else not_ok "$1: got '$2' want '$3'"; fi; }
sha()    { sha256sum "$1" | cut -d' ' -f1; }
drop_caches() { sync; echo 3 > /proc/sys/vm/drop_caches; }
finish() { echo "# $SUITE: $PASS passed, $FAIL failed"; [ "$FAIL" -eq 0 ]; }
now_ms() { awk '{printf "%d", $1 * 1000}' /proc/uptime; }
# ks3param NAME VALUE: set a module tunable (/sys/module/ks3fs/parameters)
ks3param() { echo "$2" > /sys/module/ks3fs/parameters/$1; }
# every suite starts from the module's defaults
[ -f /tmp/ks3fs.params ] && while read -r n v; do ks3param "$n" "$v"; done </tmp/ks3fs.params
# ...then KS3_PARAMS="name=value ..." from tests/run.sh's environment
for kv in ${KS3_PARAMS:-}; do ks3param "${kv%%=*}" "${kv#*=}"; done
# close_to_open: upload on close, so another mount sees a file once it is closed
close_to_open() { ks3param upload_delay_ms 0; }
# stat_of MOUNT NAME: one counter ("get", "tier1", "4xx", "sent", ...) of that mount
stat_of() {
	awk -v mnt="$1" -v key="$2" '
		$0 ~ (" mounted on " mnt " with fstype ks3fs") { f = 1; next }
		/^device / { f = 0 }
		f && /^\t(requests|billing|responses|bytes):/ {
			for (i = 2; i < NF; i += 2) if ($i == key) print $(i + 1)
		}' /proc/self/mountstats
}
# stats_sum MOUNT SECTION: the sum of one line's counters ("requests", "billing")
stats_sum() {
	awk -v mnt="$1" -v sec="$2" '
		$0 ~ (" mounted on " mnt " with fstype ks3fs") { f = 1; next }
		/^device / { f = 0 }
		f && $1 == sec ":" { for (i = 3; i <= NF; i += 2) n += $i }
		END { print n + 0 }' /proc/self/mountstats
}
