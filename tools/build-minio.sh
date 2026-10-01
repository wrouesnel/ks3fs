#!/bin/bash
# MinIO no longer publishes binaries or images, and its repository is
# archived; build the last community release from source into build/tools.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
TAG=${MINIO_TAG:-RELEASE.2025-10-15T17-29-55Z}
mkdir -p "$ROOT/build/tools"
[ -x "$ROOT/build/tools/minio" ] && exit 0
GOBIN=$ROOT/build/tools CGO_ENABLED=0 go install "github.com/minio/minio@$TAG"
"$ROOT/build/tools/minio" --version
