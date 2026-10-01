# shellcheck shell=bash
# S3 server launchers for tests/run.sh.  Each start_<name> sets PORT, AK, SK
# and registers its own teardown in SERVER_STOP.  S3_SERVER picks one:
#   minio      MinIO binary ($MINIO, or build/tools/minio built from source)
#   versitygw  Versity S3 gateway, posix backend ($VERSITYGW or build/tools)
#   rgw        Ceph RADOS Gateway in the quay.io/ceph/demo container
S3_SERVER=${S3_SERVER:-minio}
AK=ks3test
SK=ks3test-secret-key

free_port() {
	python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])'
}

wait_http() {	# wait until something answers HTTP on PORT
	for _ in $(seq "${2:-100}"); do
		curl -s -o /dev/null "http://127.0.0.1:$PORT/" && return 0
		sleep "${1:-0.2}"
	done
	echo "S3 server did not come up" >&2
	return 1
}

start_minio() {
	local bin=${MINIO:-$(command -v minio || echo "$ROOT/build/tools/minio")}
	[ -x "$bin" ] || { echo "no minio binary (tools/build-minio.sh builds one)" >&2; return 1; }
	SERVER_DIR=$(mktemp -d -p "$ROOT/build")	# not /tmp: may be tmpfs
	PORT=$(free_port)
	MINIO_ROOT_USER=$AK MINIO_ROOT_PASSWORD=$SK "$bin" server --quiet \
		--address 127.0.0.1:"$PORT" --console-address 127.0.0.1:0 \
		"$SERVER_DIR/data" >"$OUT/server.log" 2>&1 &
	SERVER_STOP="kill $!; rm -rf $SERVER_DIR"
	wait_http
}

start_versitygw() {
	local bin=${VERSITYGW:-$(command -v versitygw || echo "$ROOT/build/tools/versitygw")}
	[ -x "$bin" ] || { echo "no versitygw binary" >&2; return 1; }
	SERVER_DIR=$(mktemp -d -p "$ROOT/build")	# not /tmp: may be tmpfs
	PORT=$(free_port)
	"$bin" --access "$AK" --secret "$SK" --port 127.0.0.1:"$PORT" \
		posix "$SERVER_DIR" >"$OUT/server.log" 2>&1 &
	SERVER_STOP="kill $!; rm -rf $SERVER_DIR"
	wait_http
}

# RGW_NAME becomes rgw_dns_name: without it the demo takes any Host that is
# not its hostname or an IP (s3.ks3fs.test, used for TLS) as a virtual-host
# bucket name, and every TLS request fails with NoSuchBucket
start_rgw() {
	local engine
	engine=$(command -v docker || command -v podman)
	PORT=$(free_port)
	RGW_NAME=ks3fs-rgw-$$
	$engine run -d --rm --name "$RGW_NAME" --net=host \
		-e MON_IP=127.0.0.1 -e CEPH_PUBLIC_NETWORK=127.0.0.0/8 \
		-e CEPH_DEMO_UID=ks3 -e CEPH_DEMO_ACCESS_KEY="$AK" \
		-e CEPH_DEMO_SECRET_KEY="$SK" -e RGW_FRONTEND_PORT="$PORT" \
		-e DEMO_DAEMONS="osd rgw" \
		-e RGW_NAME=s3.ks3fs.test \
		"${RGW_IMAGE:-quay.io/ceph/demo:latest}" demo >/dev/null
	SERVER_STOP="$engine logs $RGW_NAME >$OUT/server.log 2>&1; $engine rm -f $RGW_NAME >/dev/null"
	wait_http 2 150
	# the demo user is created after the frontend starts listening
	for _ in $(seq 60); do s3 s3 ls >/dev/null 2>&1 && return 0; sleep 2; done
	return 1
}

stop_server() { [ -n "${SERVER_STOP:-}" ] && eval "$SERVER_STOP"; SERVER_STOP=; }

s3() {
	AWS_ACCESS_KEY_ID=$AK AWS_SECRET_ACCESS_KEY=$SK AWS_DEFAULT_REGION=us-east-1 AWS_REGION=us-east-1 \
	AWS_EC2_METADATA_DISABLED=true AWS_REQUEST_CHECKSUM_CALCULATION=when_required AWS_RESPONSE_CHECKSUM_VALIDATION=when_required AWS_CONFIG_FILE=/dev/null AWS_SHARED_CREDENTIALS_FILE=/dev/null \
		aws --endpoint-url "http://127.0.0.1:$PORT" "$@"
}
