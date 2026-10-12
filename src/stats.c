// SPDX-License-Identifier: GPL-2.0
/*
 * Per-mount request counters, shown in /proc/self/mountstats.
 *
 * Every attempt that is sent counts, retries included, because the server
 * sees (and bills) each one.  Operations are grouped the way S3 prices them:
 * tier 1 is PUT, COPY, POST and LIST; tier 2 is GET, HEAD and everything
 * else; DELETE and AbortMultipartUpload are free.  Byte counts are on the
 * wire (headers included, TLS payload after decryption), which is what
 * per-GB charges such as NAT processing see, give or take TLS framing.
 */
#include <linux/seq_file.h>
#include "ks3fs.h"

static const char *const op_names[KS3_OP_MAX] = {
	[KS3_OP_GET]		= "get",
	[KS3_OP_HEAD]		= "head",
	[KS3_OP_LIST]		= "list",
	[KS3_OP_PUT]		= "put",
	[KS3_OP_PUT_PART]	= "put_part",
	[KS3_OP_COPY]		= "copy",
	[KS3_OP_COPY_PART]	= "copy_part",
	[KS3_OP_MPU_CREATE]	= "mpu_create",
	[KS3_OP_MPU_COMPLETE]	= "mpu_complete",
	[KS3_OP_DELETE]		= "delete",
	[KS3_OP_MPU_ABORT]	= "mpu_abort",
	[KS3_OP_OTHER]		= "other",
};

static const char *const resp_names[KS3_RESP_MAX] = {
	[KS3_RESP_2XX]		= "2xx",
	[KS3_RESP_3XX]		= "3xx",
	[KS3_RESP_4XX]		= "4xx",
	[KS3_RESP_5XX]		= "5xx",
	[KS3_RESP_TRANSPORT]	= "transport_errors",
};

static bool has_param(const struct ks3fs_req *req, const char *name)
{
	int i;

	for (i = 0; i < req->nr_params; i++)
		if (!strcmp(req->params[i].name, name))
			return true;
	return false;
}

static enum ks3fs_op classify(const struct ks3fs_req *req)
{
	const char *m = req->method;

	if (!strcmp(m, "GET"))
		return has_param(req, "list-type") ? KS3_OP_LIST : KS3_OP_GET;
	if (!strcmp(m, "HEAD"))
		return KS3_OP_HEAD;
	if (!strcmp(m, "PUT")) {
		if (has_param(req, "partNumber"))
			return req->copy_source ? KS3_OP_COPY_PART : KS3_OP_PUT_PART;
		return req->copy_source ? KS3_OP_COPY : KS3_OP_PUT;
	}
	if (!strcmp(m, "POST")) {
		if (has_param(req, "uploads"))
			return KS3_OP_MPU_CREATE;
		if (has_param(req, "uploadId"))
			return KS3_OP_MPU_COMPLETE;
		return KS3_OP_OTHER;
	}
	if (!strcmp(m, "DELETE"))
		return has_param(req, "uploadId") ? KS3_OP_MPU_ABORT : KS3_OP_DELETE;
	return KS3_OP_OTHER;
}

static bool tier1(enum ks3fs_op op)
{
	switch (op) {
	case KS3_OP_LIST:
	case KS3_OP_PUT:
	case KS3_OP_PUT_PART:
	case KS3_OP_COPY:
	case KS3_OP_COPY_PART:
	case KS3_OP_MPU_CREATE:
	case KS3_OP_MPU_COMPLETE:
		return true;
	default:
		return false;
	}
}

static bool billing_free(enum ks3fs_op op)
{
	return op == KS3_OP_DELETE || op == KS3_OP_MPU_ABORT;
}

void ks3fs_stats_request(struct ks3fs_sb_info *sbi, const struct ks3fs_req *req)
{
	atomic64_inc(&sbi->stats.req[classify(req)]);
}

void ks3fs_stats_response(struct ks3fs_sb_info *sbi, int status, int err)
{
	enum ks3fs_resp_class c;

	if (err)
		c = KS3_RESP_TRANSPORT;
	else if (status >= 500)
		c = KS3_RESP_5XX;
	else if (status >= 400)
		c = KS3_RESP_4XX;
	else if (status >= 300)
		c = KS3_RESP_3XX;
	else
		c = KS3_RESP_2XX;
	atomic64_inc(&sbi->stats.resp[c]);
}

int ks3fs_show_stats(struct seq_file *m, struct dentry *root)
{
	struct ks3fs_stats *st = &KS3_SB(root->d_sb)->stats;
	u64 t1 = 0, t2 = 0, nofee = 0;
	int i;

	seq_puts(m, "\n\tks3fs-stats-version: 1\n\trequests:");
	for (i = 0; i < KS3_OP_MAX; i++) {
		u64 n = atomic64_read(&st->req[i]);

		seq_printf(m, " %s %llu", op_names[i], n);
		if (tier1(i))
			t1 += n;
		else if (billing_free(i))
			nofee += n;
		else
			t2 += n;
	}
	seq_printf(m, "\n\tbilling: tier1 %llu tier2 %llu free %llu\n\tresponses:",
		   t1, t2, nofee);
	for (i = 0; i < KS3_RESP_MAX; i++)
		seq_printf(m, " %s %llu", resp_names[i],
			   (u64)atomic64_read(&st->resp[i]));
	seq_printf(m, "\n\tbytes: sent %llu received %llu\n",
		   (u64)atomic64_read(&st->bytes_sent),
		   (u64)atomic64_read(&st->bytes_recv));
	return 0;
}
