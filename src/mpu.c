// SPDX-License-Identifier: GPL-2.0
/*
 * S3 multipart upload primitives, and multipart copy for objects too big
 * for a single CopyObject (5 GiB).
 */
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/kref.h>

#include "ks3fs.h"

static void mpu_log(const char *op, const char *key, int status,
		    const char *body, size_t len)
{
	char code[64];

	if (ks3fs_xml_error_code(body, len, code, sizeof(code)))
		strscpy(code, "-", sizeof(code));
	pr_warn_ratelimited("ks3fs: %s '%s' failed: HTTP %d (%s)\n", op, key,
			    status, code);
}

/* Some multipart calls report errors in a 200 response body. */
static int mpu_status(const char *op, const char *key,
		      struct ks3fs_resp *resp, const char *body, size_t len)
{
	int err = ks3fs_status_to_errno(resp->status);

	if (!err && body && strnstr(body, "<Error>", len))
		err = -EIO;
	if (err)
		mpu_log(op, key, resp->status, body, len);
	return err;
}

char *ks3fs_copy_source(struct ks3fs_sb_info *sbi, const char *key)
{
	size_t enclen = strlen(key) * 3 + 1;
	char *enc, *src;

	enc = kmalloc(enclen, GFP_NOFS);
	if (!enc)
		return NULL;
	if (ks3fs_uri_encode(enc, enclen, key, true) < 0) {
		kfree(enc);
		return NULL;
	}
	src = kasprintf(GFP_NOFS, "/%s/%s", sbi->bucket, enc);
	kfree(enc);
	return src;
}

int ks3fs_mpu_create(struct ks3fs_sb_info *sbi, const char *key,
		     const struct ks3fs_meta *meta, char **upload_id)
{
	struct ks3fs_param p = { "uploads", NULL };
	struct ks3fs_req req = {
		.method = "POST", .key = key, .params = &p, .nr_params = 1,
		.meta = meta,
	};
	struct ks3fs_resp resp;
	char *body = NULL, *id;
	size_t len = 0;
	int err;

	err = ks3fs_http_simple(sbi, &req, &resp, &body, &len);
	if (err)
		return err;
	err = mpu_status("CreateMultipartUpload", key, &resp, body, len);
	if (!err) {
		id = kmalloc(1024, GFP_NOFS);
		if (!id)
			err = -ENOMEM;
		else if (ks3fs_xml_get(body, len, "UploadId", id, 1024) || !*id)
			err = -EPROTO;
		if (err)
			kfree(id);
		else
			*upload_id = id;
	}
	kvfree(body);
	return err;
}

int ks3fs_mpu_upload_part(struct ks3fs_sb_info *sbi, const char *key,
			  const char *upload_id, int partno, loff_t len,
			  int (*send_body)(struct ks3fs_conn *, void *),
			  void *arg, char *etag_out)
{
	char pn[16];
	struct ks3fs_param params[2] = {
		{ "partNumber", pn }, { "uploadId", upload_id },
	};
	struct ks3fs_req req = {
		.method = "PUT", .key = key, .params = params, .nr_params = 2,
		.body_len = len, .payload_sha256 = "UNSIGNED-PAYLOAD",
	};
	struct ks3fs_resp resp;
	struct ks3fs_conn *conn;
	int err;

	snprintf(pn, sizeof(pn), "%d", partno);
	conn = ks3fs_http_start(sbi, &req, &resp, send_body, arg, NULL);
	if (IS_ERR(conn))
		return PTR_ERR(conn);
	err = ks3fs_status_to_errno(resp.status);
	if (err)
		mpu_log("UploadPart", key, resp.status, NULL, 0);
	else if (!resp.etag[0])
		err = -EPROTO;
	else
		strscpy(etag_out, resp.etag, KS3FS_ETAG_LEN);
	ks3fs_http_finish(sbi, conn, &resp);
	return err;
}

/* Copy [start, end) of object @src into part @partno (server side). */
int ks3fs_mpu_copy_part(struct ks3fs_sb_info *sbi, const char *key,
			const char *upload_id, int partno, const char *src,
			loff_t start, loff_t end, const char *if_match,
			char *etag_out)
{
	char pn[16], range[64];
	struct ks3fs_param params[2] = {
		{ "partNumber", pn }, { "uploadId", upload_id },
	};
	struct ks3fs_req req = {
		.method = "PUT", .key = key, .params = params, .nr_params = 2,
		.copy_range = range, .copy_if_match = if_match, .slow = true,
	};
	struct ks3fs_resp resp;
	char *body = NULL;
	size_t len = 0;
	int err;

	snprintf(pn, sizeof(pn), "%d", partno);
	snprintf(range, sizeof(range), "bytes=%lld-%lld", start, end - 1);
	req.copy_source = ks3fs_copy_source(sbi, src);
	if (!req.copy_source)
		return -ENOMEM;
	err = ks3fs_http_simple(sbi, &req, &resp, &body, &len);
	kfree(req.copy_source);
	if (err)
		return err;
	err = mpu_status("UploadPartCopy", key, &resp, body, len);
	if (!err && ks3fs_xml_get(body, len, "ETag", etag_out, KS3FS_ETAG_LEN))
		err = -EPROTO;
	kvfree(body);
	return err;
}

/*
 * After a retried completion: is the object there, with the size the
 * upload makes?  A server can answer the retry while it is still putting
 * the object together from the first attempt (versitygw does), so wait
 * for it rather than take the answer on trust.
 */
static int mpu_verify(struct ks3fs_sb_info *sbi, const char *key,
		      loff_t size, char *etag_out)
{
	unsigned long deadline = jiffies + KS3FS_SLOW_TIMEOUT;
	struct ks3fs_attr attr;
	int err;

	for (;;) {
		err = ks3fs_s3_head(sbi, key, &attr);
		if (!err && attr.size == size) {
			strscpy(etag_out, attr.etag, KS3FS_ETAG_LEN);
			return 0;
		}
		if (err && err != -ENOENT)
			return err;
		if (time_after(jiffies, deadline)) {
			pr_warn_ratelimited("ks3fs: '%s' is still not %lld bytes after completing its upload\n",
					    key, size);
			return -EIO;
		}
		if (schedule_timeout_killable(HZ))
			return -EINTR;
	}
}

int ks3fs_mpu_complete(struct ks3fs_sb_info *sbi, const char *key,
		       const char *upload_id, char (*etags)[KS3FS_ETAG_LEN],
		       int nr, loff_t size, char *etag_out)
{
	struct ks3fs_param p = { "uploadId", upload_id };
	struct ks3fs_req req = {
		.method = "POST", .key = key, .params = &p, .nr_params = 1,
		/*
		 * the server assembles the object first; a retry while it is
		 * still at it may be answered at once (versitygw) before the
		 * object exists
		 */
		.slow = true,
	};
	struct ks3fs_retry r;
	struct ks3fs_resp resp;
	char *xml, *body = NULL;
	size_t cap = 64 + (size_t)nr * (48 + KS3FS_ETAG_LEN), o, len = 0;
	int i, err;

	xml = kvmalloc(cap, GFP_NOFS);
	if (!xml)
		return -ENOMEM;
	o = scnprintf(xml, cap, "<CompleteMultipartUpload>");
	for (i = 0; i < nr; i++)
		o += scnprintf(xml + o, cap - o,
			       "<Part><PartNumber>%d</PartNumber><ETag>%s</ETag></Part>",
			       i + 1, etags[i]);
	o += scnprintf(xml + o, cap - o, "</CompleteMultipartUpload>");
	req.body = xml;
	req.body_len = o;

	ks3fs_retry_init(&r);
	err = ks3fs_http_request(sbi, &req, &resp, &body, &len, &r);
	kvfree(xml);
	if (err)
		return err;
	/*
	 * If an earlier attempt completed the upload but its response was
	 * lost, the retry finds no such upload: the object is already there.
	 */
	if (resp.status == 404 && r.sent > 1 && body &&
	    strnstr(body, "NoSuchUpload", len)) {
		kvfree(body);
		return mpu_verify(sbi, key, size, etag_out);
	}
	err = mpu_status("CompleteMultipartUpload", key, &resp, body, len);
	if (!err && ks3fs_xml_get(body, len, "ETag", etag_out, KS3FS_ETAG_LEN))
		etag_out[0] = '\0';
	kvfree(body);
	if (!err && r.sent > 1)
		err = mpu_verify(sbi, key, size, etag_out);
	return err;
}

void ks3fs_mpu_abort(struct ks3fs_sb_info *sbi, const char *key,
		     const char *upload_id)
{
	struct ks3fs_param p = { "uploadId", upload_id };
	struct ks3fs_req req = {
		.method = "DELETE", .key = key, .params = &p, .nr_params = 1,
	};
	struct ks3fs_resp resp;

	if (!ks3fs_http_simple(sbi, &req, &resp, NULL, NULL) &&
	    resp.status >= 300 && resp.status != 404)
		mpu_log("AbortMultipartUpload", key, resp.status, NULL, 0);
}

/* ---------- parallel UploadPartCopy ---------- */

struct copy_job {
	struct kref ref;
	struct ks3fs_sb_info *sbi;
	char *key, *upload_id, *src, *if_match;
	u64 part;
	loff_t size;
	int *idx;			/* part index of each job */
	char (*etags)[KS3FS_ETAG_LEN];	/* result of each job */
};

static void copy_job_free(struct kref *ref)
{
	struct copy_job *j = container_of(ref, struct copy_job, ref);

	kfree(j->key);
	kfree(j->upload_id);
	kfree(j->src);
	kfree(j->if_match);
	kvfree(j->idx);
	kvfree(j->etags);
	kfree(j);
}

static void copy_job_put(void *ctx)
{
	kref_put(&((struct copy_job *)ctx)->ref, copy_job_free);
}

static int copy_one(void *ctx, int i)
{
	struct copy_job *j = ctx;
	loff_t start = (loff_t)j->idx[i] * j->part;

	return ks3fs_mpu_copy_part(j->sbi, j->key, j->upload_id, j->idx[i] + 1,
				   j->src, start,
				   min_t(loff_t, start + j->part, j->size),
				   j->if_match, j->etags[i]);
}

/*
 * Copy parts @idx[0..nr) of @src (part size @part, object size @size) into
 * the upload, concurrently; ETags land in @out[part index].
 */
int ks3fs_mpu_copy_parts(struct ks3fs_sb_info *sbi, const char *key,
			 const char *upload_id, const char *src,
			 const char *if_match, u64 part, loff_t size,
			 const int *idx, int nr, char (*out)[KS3FS_ETAG_LEN])
{
	struct copy_job *j;
	int i, err;

	if (!nr)
		return 0;
	j = kzalloc(sizeof(*j), GFP_KERNEL);
	if (!j)
		return -ENOMEM;
	kref_init(&j->ref);
	j->sbi = sbi;
	j->part = part;
	j->size = size;
	j->key = kstrdup(key, GFP_KERNEL);
	j->upload_id = kstrdup(upload_id, GFP_KERNEL);
	j->src = kstrdup(src, GFP_KERNEL);
	j->if_match = if_match ? kstrdup(if_match, GFP_KERNEL) : NULL;
	j->idx = kvmemdup(idx, nr * sizeof(*idx), GFP_KERNEL);
	j->etags = kvcalloc(nr, KS3FS_ETAG_LEN, GFP_KERNEL);
	if (!j->key || !j->upload_id || !j->src || (if_match && !j->if_match) ||
	    !j->idx || !j->etags) {
		copy_job_put(j);
		return -ENOMEM;
	}
	kref_get(&j->ref);	/* the run's; ours outlives it for the results */
	err = ks3fs_parallel(sbi, nr, copy_one, j, copy_job_put);
	if (!err)
		for (i = 0; i < nr; i++)
			memcpy(out[idx[i]], j->etags[i], KS3FS_ETAG_LEN);
	copy_job_put(j);
	return err;
}

/*
 * Copy an object of any size with UploadPartCopy.  @meta replaces the
 * metadata; NULL keeps the source's (fetched with a HEAD).
 */
int ks3fs_s3_copy_large(struct ks3fs_sb_info *sbi, const char *src,
			const char *dst, loff_t size,
			const struct ks3fs_meta *meta, char *etag_out)
{
	char (*etags)[KS3FS_ETAG_LEN] = NULL;
	struct ks3fs_attr attr;
	int *idx = NULL;
	char *id = NULL;
	u64 part;
	int i, nr, err;

	if (!meta) {
		err = ks3fs_s3_head(sbi, src, &attr);
		if (err)
			return err;
		meta = &attr.meta;
	}
	/*
	 * Servers copy a part before answering (RGW needs >30s for 256 MiB on
	 * slow storage): keep parts at the mount's part size, in parallel.
	 */
	part = max_t(u64, sbi->part_size,
		     DIV_ROUND_UP_ULL(size, KS3FS_MPU_MAX_PARTS));
	part = round_up(part, 1 << 20);
	nr = DIV_ROUND_UP_ULL(size, part);
	if (!nr || part > KS3FS_MAX_PUT)
		return -EFBIG;

	etags = kvcalloc(nr, KS3FS_ETAG_LEN, GFP_NOFS);
	idx = kvmalloc_array(nr, sizeof(*idx), GFP_NOFS);
	if (!etags || !idx) {
		err = -ENOMEM;
		goto out;
	}
	for (i = 0; i < nr; i++)
		idx[i] = i;
	err = ks3fs_mpu_create(sbi, dst, meta, &id);
	if (err)
		goto out;
	err = ks3fs_mpu_copy_parts(sbi, dst, id, src, NULL, part, size, idx,
				   nr, etags);
	if (!err)
		err = ks3fs_mpu_complete(sbi, dst, id, etags, nr, size,
					 etag_out);
	if (err && id)
		ks3fs_mpu_abort(sbi, dst, id);
out:
	kfree(id);
	kvfree(idx);
	kvfree(etags);
	return err;
}
