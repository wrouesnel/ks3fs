#!/bin/bash
# Print the concrete kernel versions for tests/kernels.txt, one per line.
#   --json   print a JSON array instead (for a GitHub Actions matrix)
# KERNELS_FILE overrides the list (e.g. one line "mainline-latest").
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
out=()
while read -r k; do
	case $k in ''|\#*) continue ;; esac
	if [ "$k" = mainline-latest ]; then
		# newest build in Ubuntu's mainline archive (release or -rc)
		d=$(curl -fsSL https://kernel.ubuntu.com/mainline/ |
		    grep -oE 'href="v[0-9]+\.[0-9]+(\.[0-9]+)?(-rc[0-9]+)?/"' |
		    sed 's/href="//;s|/"||' | sort -V | tail -1)
		k=$(curl -fsSL "https://kernel.ubuntu.com/mainline/$d/amd64/" |
		    grep -oE 'linux-headers-[0-9.]+-[0-9]{6}(rc[0-9]+)?-generic_' | head -1 |
		    sed 's/^linux-headers-//;s/_$//')
		[ -n "$k" ] || { echo "no mainline kernel found" >&2; exit 1; }
	elif [[ $k == *-latest ]]; then
		series=${k%-latest}
		v=$(apt-cache search --names-only "^linux-image-unsigned-${series//./\\.}-[0-9]+-generic\$" |
		    awk '{print $1}' | sed 's/^linux-image-unsigned-//' | sort -V | tail -1)
		[ -n "$v" ] || { echo "no kernel found for $k" >&2; exit 1; }
		k=$v
	fi
	out+=("$k")
done <"${KERNELS_FILE:-$ROOT/tests/kernels.txt}"
if [ "${1:-}" = --json ]; then
	printf '%s\n' "${out[@]}" | sort -uV | python3 -c 'import json,sys; print(json.dumps([l.strip() for l in sys.stdin if l.strip()]))'
else
	printf '%s\n' "${out[@]}" | sort -uV
fi
