#!/bin/bash
# Run tests/run.sh for every kernel in tests/kernels.txt (or those given).
ROOT=$(cd "$(dirname "$0")/.." && pwd)
KERNELS=("$@")
[ ${#KERNELS[@]} -gt 0 ] || mapfile -t KERNELS < <("$ROOT/tests/resolve-kernels.sh")
declare -A RES
rc=0
for k in "${KERNELS[@]}"; do
	if "$ROOT/tests/run.sh" "$k" >"$ROOT/build/matrix-$k.log" 2>&1; then
		RES[$k]=PASS
	else
		RES[$k]=FAIL; rc=1
	fi
	echo "$k: ${RES[$k]}"
done
exit $rc
