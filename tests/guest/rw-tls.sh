# The full rw suite again, over TLS (through the TLS-terminating proxy, with
# the certificate verified by tlshd against the test CA), on its own bucket.
SUITE=rw-tls
RW_BUCKET=ks3tls
RW_OPTS="addr=10.0.2.2,port=$FP_TLS,host=s3.ks3fs.test,tls,access_key=$S3_AK,secret_key=$S3_SK"
. /tests/rw.sh
