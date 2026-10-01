// SPDX-License-Identifier: GPL-2.0
/*
 * S3 object operations used by the VFS layer.
 */
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/sort.h>

#include "ks3fs.h"

char *ks3fs_full_key(struct ks3fs_sb_info *sbi, const char *rel)
{
	return kasprintf(GFP_NOFS, "%s%s", sbi->prefix, rel);
}

static void log_failure(const char *op, const char *key, int status,
			const char *body, size_t len)
{
	char code[64];

	if (ks3fs_xml_error_code(body, len, code, sizeof(code)))
		strscpy(code, "-", sizeof(code));
	pr_warn_ratelimited("ks3fs: %s '%s' failed: HTTP %d (%s)\n",
			    op, key ?: "", status, code);
}

/* HEAD an object. */
int ks3fs_s3_head(struct ks3fs_sb_info *sbi, const char *key,
		   struct ks3fs_attr *attr)
{
	struct ks3fs_req req = { .method = "HEAD", .key = key };
	struct ks3fs_resp resp;
	int err;

	err = ks3fs_http_simple(sbi, &req, &resp, NULL, NULL);
	if (err)
		return err;
	err = ks3fs_status_to_errno(resp.status);
	if (err)
		return err;
	attr->is_dir = false;
	attr->has_marker = false;
	attr->size = max_t(loff_t, resp.content_length, 0);
	attr->mtime = resp.last_modified;
	strscpy(attr->etag, resp.etag, sizeof(attr->etag));
	attr->meta = resp.meta;
	return 0;
}

struct list_ctx {
	bool hide_orphans;	/* listing the mount root */
	struct ks3fs_listing *l;
	const char *prefix;
	size_t prefix_len;
	int count;		/* raw entries seen, excluding the marker */
};

static int list_cb(void *arg, const char *key, size_t keylen, bool is_prefix,
		   loff_t size, time64_t mtime, const char *etag)
{
	struct list_ctx *c = arg;
	struct ks3fs_listing *l = c->l;
	struct ks3fs_list_entry *e;
	const char *name;
	size_t nlen;

	if (keylen < c->prefix_len || strncmp(key, c->prefix, c->prefix_len))
		return 0;
	name = key + c->prefix_len;
	nlen = keylen - c->prefix_len;
	if (is_prefix && nlen && name[nlen - 1] == '/')
		nlen--;
	if (!nlen) {
		if (!is_prefix)
			l->has_marker = true;
		return 0;
	}
	c->count++;
	if (c->hide_orphans && nlen >= sizeof(KS3FS_ORPHANS) - 1 &&
	    !strncmp(name, KS3FS_ORPHANS, sizeof(KS3FS_ORPHANS) - 1) &&
	    (nlen == sizeof(KS3FS_ORPHANS) - 1 ||
	     name[sizeof(KS3FS_ORPHANS) - 1] == '/'))
		return 0;	/* ks3fs's own hidden directory */
	if (memchr(name, '/', nlen) || memchr(name, '\0', nlen) ||
	    nlen > NAME_MAX ||
	    (nlen == 1 && name[0] == '.') ||
	    (nlen == 2 && name[0] == '.' && name[1] == '.'))
		return 0;	/* not representable as a directory entry */

	if (!l->ents)		/* counting only */
		return 0;
	if (l->nr == l->cap) {
		int ncap = l->cap * 2;
		struct ks3fs_list_entry *n;

		n = kvmalloc_array(ncap, sizeof(*n), GFP_NOFS);
		if (!n)
			return -ENOMEM;
		memcpy(n, l->ents, l->nr * sizeof(*n));
		kvfree(l->ents);
		l->ents = n;
		l->cap = ncap;
	}
	e = &l->ents[l->nr];
	e->name = kmemdup_nul(name, nlen, GFP_NOFS);
	if (!e->name)
		return -ENOMEM;
	e->is_dir = is_prefix;
	e->size = size;
	e->mtime = mtime;
	strscpy(e->etag, etag, sizeof(e->etag));
	l->nr++;
	return 0;
}

/*
 * Page through ListObjectsV2 for @prefix, feeding every entry to @cb.
 * @delimiter "/" lists one directory level; NULL lists recursively.
 */
static int s3_list_raw(struct ks3fs_sb_info *sbi, const char *prefix,
		       const char *delimiter, int max_keys, bool first_page,
		       ks3fs_list_cb cb, void *arg)
{
	char *token = NULL;
	char maxk[16];
	int err;

	snprintf(maxk, sizeof(maxk), "%d", max_keys);
	do {
		struct ks3fs_param params[6] = {
			{ "list-type", "2" },
			{ "encoding-type", "url" },
			{ "max-keys", maxk },
			{ "prefix", prefix },
		};
		int np = 4;
		struct ks3fs_req req = { .method = "GET", .params = params };
		struct ks3fs_resp resp;
		char *body = NULL, *next = NULL;
		size_t len = 0;
		bool truncated;

		if (delimiter)
			params[np++] = (struct ks3fs_param){ "delimiter", delimiter };
		if (token)
			params[np++] = (struct ks3fs_param){ "continuation-token", token };
		req.nr_params = np;

		err = ks3fs_http_simple(sbi, &req, &resp, &body, &len);
		if (err)
			break;
		err = ks3fs_status_to_errno(resp.status);
		if (err) {
			log_failure("LIST", prefix, resp.status, body, len);
			kvfree(body);
			break;
		}
		err = ks3fs_xml_parse_list(body, len, cb, arg, &truncated, &next);
		kvfree(body);
		kfree(token);
		token = next;
		if (err || first_page)
			break;
		cond_resched();
	} while (token);
	kfree(token);
	return err;
}

/*
 * One ListObjectsV2 pass over @prefix with delimiter '/'.  If @l->ents is
 * NULL only counts entries, stopping after the first page.
 */
static int s3_list(struct ks3fs_sb_info *sbi, const char *prefix,
		   int max_keys, struct ks3fs_listing *l, int *count)
{
	struct list_ctx c = { .l = l, .prefix = prefix,
			      .prefix_len = strlen(prefix),
			      .hide_orphans = !strcmp(prefix, sbi->prefix) };
	int err;

	err = s3_list_raw(sbi, prefix, "/", max_keys, !l->ents, list_cb, &c);
	if (count)
		*count = c.count;
	return err;
}

struct keys_ctx {
	struct ks3fs_keylist *kl;
	int max;
};

static int keys_cb(void *arg, const char *key, size_t keylen, bool is_prefix,
		   loff_t size, time64_t mtime, const char *etag)
{
	struct keys_ctx *c = arg;
	struct ks3fs_keylist *kl = c->kl;

	if (is_prefix)
		return 0;
	if (kl->nr >= c->max)
		return -E2BIG;
	if (kl->nr == kl->cap) {
		int ncap = kl->cap ? kl->cap * 2 : 256;
		struct ks3fs_keyent *n = kvcalloc(ncap, sizeof(*n), GFP_KERNEL);

		if (!n)
			return -ENOMEM;
		if (kl->ents)
			memcpy(n, kl->ents, kl->nr * sizeof(*n));
		kvfree(kl->ents);
		kl->ents = n;
		kl->cap = ncap;
	}
	kl->ents[kl->nr].key = kmemdup_nul(key, keylen, GFP_KERNEL);
	if (!kl->ents[kl->nr].key)
		return -ENOMEM;
	kl->ents[kl->nr].size = size;
	kl->nr++;
	return 0;
}

/* Every key under @prefix (recursively), at most @max of them. */
int ks3fs_s3_list_keys(struct ks3fs_sb_info *sbi, const char *prefix,
		       int max, struct ks3fs_keylist *kl)
{
	struct keys_ctx c = { .kl = kl, .max = max };
	int err;

	memset(kl, 0, sizeof(*kl));
	err = s3_list_raw(sbi, prefix, NULL, 1000, false, keys_cb, &c);
	if (err)
		ks3fs_keylist_free(kl);
	return err;
}

void ks3fs_keylist_free(struct ks3fs_keylist *kl)
{
	int i;

	for (i = 0; i < kl->nr; i++)
		kfree(kl->ents[i].key);
	kvfree(kl->ents);
	memset(kl, 0, sizeof(*kl));
}

static int cmp_ent(const void *a, const void *b)
{
	const struct ks3fs_list_entry *x = a, *y = b;
	int r = strcmp(x->name, y->name);

	/* for duplicate names, order the file first so it wins */
	return r ? r : (int)x->is_dir - (int)y->is_dir;
}

void ks3fs_listing_free(struct ks3fs_listing *l)
{
	int i;

	for (i = 0; i < l->nr; i++)
		kfree(l->ents[i].name);
	kvfree(l->ents);
	l->ents = NULL;
	l->nr = l->cap = 0;
}

/* List the immediate children of @dirkey (a full key ending in '/' or ""). */
int ks3fs_s3_list_dir(struct ks3fs_sb_info *sbi, const char *dirkey,
		      struct ks3fs_listing *out)
{
	int i, j, err;

	memset(out, 0, sizeof(*out));
	out->cap = 64;
	out->ents = kvmalloc_array(out->cap, sizeof(*out->ents), GFP_NOFS);
	if (!out->ents)
		return -ENOMEM;

	err = s3_list(sbi, dirkey, 1000, out, NULL);
	if (err) {
		ks3fs_listing_free(out);
		return err;
	}

	/* S3 returns objects and prefixes separately; merge and dedupe. */
	sort(out->ents, out->nr, sizeof(*out->ents), cmp_ent, NULL);
	for (i = 0, j = 0; i < out->nr; i++) {
		if (j && !strcmp(out->ents[j - 1].name, out->ents[i].name)) {
			kfree(out->ents[i].name);
			continue;
		}
		out->ents[j++] = out->ents[i];
	}
	out->nr = j;
	return 0;
}

/*
 * Look up @key (a full key without trailing '/').  A plain object wins over
 * a same-named "directory" prefix.
 */
int ks3fs_s3_stat(struct ks3fs_sb_info *sbi, const char *key,
		  struct ks3fs_attr *attr)
{
	struct ks3fs_listing l = {};
	char *dirkey;
	int count = 0, err;

	err = ks3fs_s3_head(sbi, key, attr);
	if (err != -ENOENT)
		return err;

	dirkey = kasprintf(GFP_NOFS, "%s/", key);
	if (!dirkey)
		return -ENOMEM;
	err = s3_list(sbi, dirkey, 2, &l, &count);
	kfree(dirkey);
	if (err)
		return err;
	if (!count && !l.has_marker)
		return -ENOENT;

	memset(attr, 0, sizeof(*attr));
	attr->is_dir = true;
	attr->has_marker = l.has_marker;
	if (l.has_marker && sbi->meta) {
		/* the marker object carries the directory's mode and times */
		struct ks3fs_attr marker;

		dirkey = kasprintf(GFP_NOFS, "%s/", key);
		if (!dirkey)
			return -ENOMEM;
		err = ks3fs_s3_head(sbi, dirkey, &marker);
		kfree(dirkey);
		if (!err) {
			attr->meta = marker.meta;
			attr->mtime = marker.mtime;
		} else if (err != -ENOENT) {
			return err;
		}
	}
	return 0;
}

/* Check that @dirkey can be listed (bucket exists, credentials work). */
int ks3fs_s3_probe(struct ks3fs_sb_info *sbi, const char *dirkey,
		   bool *has_marker)
{
	struct ks3fs_listing l = {};
	int err;

	err = s3_list(sbi, dirkey, 1, &l, NULL);
	*has_marker = l.has_marker;
	return err;
}

/* Returns 1 if empty, 0 if not, -errno on failure. */
int ks3fs_s3_dir_is_empty(struct ks3fs_sb_info *sbi, const char *dirkey)
{
	struct ks3fs_listing l = {};
	int count = 0, err;

	err = s3_list(sbi, dirkey, 2, &l, &count);
	if (err)
		return err;
	return count == 0;
}

struct mem_body {
	const void *buf;
	size_t len;
};

static int send_mem(struct ks3fs_conn *conn, void *arg)
{
	struct mem_body *mb = arg;

	return ks3fs_http_send(conn, mb->buf, mb->len);
}

/* PUT a small in-memory object (possibly empty) with metadata. */
int ks3fs_s3_put_buf(struct ks3fs_sb_info *sbi, const char *key,
		     const void *buf, size_t len,
		     const struct ks3fs_meta *meta, char *etag_out)
{
	struct mem_body mb = { .buf = buf, .len = len };
	struct ks3fs_req req = {
		.method = "PUT", .key = key, .body_len = len, .meta = meta,
	};
	struct ks3fs_resp resp;
	struct ks3fs_conn *conn;
	char hash[65];
	int err;

	if (len) {
		err = ks3fs_sha256_hex(buf, len, hash);
		if (err)
			return err;
		req.payload_sha256 = hash;
	}
	conn = ks3fs_http_start(sbi, &req, &resp, len ? send_mem : NULL, &mb,
				NULL);
	if (IS_ERR(conn))
		return PTR_ERR(conn);
	err = ks3fs_status_to_errno(resp.status);
	if (err)
		log_failure("PUT", key, resp.status, NULL, 0);
	else if (etag_out)
		strscpy(etag_out, resp.etag, KS3FS_ETAG_LEN);
	ks3fs_http_finish(sbi, conn, &resp);
	return err;
}

int ks3fs_s3_delete(struct ks3fs_sb_info *sbi, const char *key)
{
	struct ks3fs_req req = { .method = "DELETE", .key = key };
	struct ks3fs_resp resp;
	char *body = NULL;
	size_t len = 0;
	int err;

	err = ks3fs_http_simple(sbi, &req, &resp, &body, &len);
	if (err)
		return err;
	err = ks3fs_status_to_errno(resp.status);
	if (err == -ENOENT)
		err = 0;
	if (err)
		log_failure("DELETE", key, resp.status, body, len);
	kvfree(body);
	return err;
}

static int s3_copy(struct ks3fs_sb_info *sbi, const char *src,
		   const char *dst, const struct ks3fs_meta *meta,
		   char *etag_out)
{
	struct ks3fs_req req = {
		.method = "PUT", .key = dst,
		/* without a replacement the source's metadata is copied */
		.meta = meta, .meta_replace = meta != NULL,
	};
	struct ks3fs_resp resp;
	char *enc, *source, *body = NULL;
	size_t enclen = strlen(src) * 3 + 1, len = 0;
	int err;

	enc = kmalloc(enclen, GFP_NOFS);
	if (!enc)
		return -ENOMEM;
	err = ks3fs_uri_encode(enc, enclen, src, true);
	if (err < 0) {
		kfree(enc);
		return err;
	}
	source = kasprintf(GFP_NOFS, "/%s/%s", sbi->bucket, enc);
	kfree(enc);
	if (!source)
		return -ENOMEM;
	req.copy_source = source;

	err = ks3fs_http_simple(sbi, &req, &resp, &body, &len);
	kfree(source);
	if (err)
		return err;
	err = ks3fs_status_to_errno(resp.status);
	/* CopyObject can fail with a 200 status and an <Error> body */
	if (!err && body && strnstr(body, "<Error>", len))
		err = -EIO;
	if (err) {
		log_failure("COPY", dst, resp.status, body, len);
	} else if (etag_out &&
		   ks3fs_xml_get(body, len, "ETag", etag_out, KS3FS_ETAG_LEN)) {
		/* entity-decoded: MinIO writes the quotes as &#34;, AWS as &quot; */
		etag_out[0] = '\0';
	}
	kvfree(body);
	return err;
}

int ks3fs_s3_copy(struct ks3fs_sb_info *sbi, const char *src,
		  const char *dst, char *etag_out)
{
	return s3_copy(sbi, src, dst, NULL, etag_out);
}

/* Replace an object's metadata in place (a copy onto itself). */
int ks3fs_s3_set_meta(struct ks3fs_sb_info *sbi, const char *key,
		      const struct ks3fs_meta *meta, char *etag_out)
{
	return s3_copy(sbi, key, key, meta, etag_out);
}
