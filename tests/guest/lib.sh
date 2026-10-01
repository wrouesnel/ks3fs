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
