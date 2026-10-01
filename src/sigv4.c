// SPDX-License-Identifier: GPL-2.0
/*
 * AWS Signature Version 4 request construction for ks3fs.
 *
 * Uses the "sha256" shash (a single shared tfm; per-call descriptors) and
 * builds HMAC on top of it so no keyed tfm state is shared between callers.
 */
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sort.h>
#include <linux/ctype.h>
#include <linux/hex.h>
#include <linux/timekeeping.h>
#include <linux/time.h>
#include <crypto/hash.h>
#include <crypto/sha2.h>

#include "ks3fs.h"

#define EMPTY_SHA256 \
	"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"

static struct crypto_shash *sha256_tfm;

int ks3fs_sigv4_init(void)
{
	sha256_tfm = crypto_alloc_shash("sha256", 0, 0);
	if (IS_ERR(sha256_tfm)) {
		int err = PTR_ERR(sha256_tfm);

		sha256_tfm = NULL;
		return err;
	}
	return 0;
}

void ks3fs_sigv4_exit(void)
{
	if (sha256_tfm)
		crypto_free_shash(sha256_tfm);
}

struct ks3fs_sha256 {
	struct shash_desc desc;	/* must be last: followed by descsize bytes */
};

struct ks3fs_sha256 *ks3fs_sha256_begin(void)
{
	struct ks3fs_sha256 *h;

	h = kmalloc(sizeof(*h) + crypto_shash_descsize(sha256_tfm), GFP_NOFS);
	if (!h)
		return NULL;
	h->desc.tfm = sha256_tfm;
	if (crypto_shash_init(&h->desc)) {
		kfree(h);
		return NULL;
	}
	return h;
}

int ks3fs_sha256_update(struct ks3fs_sha256 *h, const void *data, size_t len)
{
	return crypto_shash_update(&h->desc, data, len);
}

static int sha256_final_raw(struct ks3fs_sha256 *h, u8 out[SHA256_DIGEST_SIZE])
{
	int err = crypto_shash_final(&h->desc, out);

	kfree_sensitive(h);
	return err;
}

int ks3fs_sha256_end(struct ks3fs_sha256 *h, char hex[65])
{
	u8 dig[SHA256_DIGEST_SIZE];
	int err = sha256_final_raw(h, dig);

	if (!err)
		bin2hex(hex, dig, sizeof(dig));
	hex[64] = '\0';
	return err;
}

int ks3fs_sha256_hex(const void *data, size_t len, char hex[65])
{
	u8 dig[SHA256_DIGEST_SIZE];
	int err = crypto_shash_tfm_digest(sha256_tfm, data, len, dig);

	if (err)
		return err;
	bin2hex(hex, dig, sizeof(dig));
	hex[64] = '\0';
	return 0;
}

/* HMAC-SHA256 with a key of at most one block (all SigV4 keys qualify). */
static int ks3_hmac_sha256(const u8 *key, size_t keylen, const void *msg,
		       size_t msglen, u8 out[SHA256_DIGEST_SIZE])
{
	u8 pad[SHA256_BLOCK_SIZE];
	u8 inner[SHA256_DIGEST_SIZE];
	struct ks3fs_sha256 *h;
	int i, err;

	if (keylen > SHA256_BLOCK_SIZE)
		return -EINVAL;

	memset(pad, 0, sizeof(pad));
	memcpy(pad, key, keylen);
	for (i = 0; i < SHA256_BLOCK_SIZE; i++)
		pad[i] ^= 0x36;
	h = ks3fs_sha256_begin();
	if (!h)
		return -ENOMEM;
	err = ks3fs_sha256_update(h, pad, sizeof(pad)) ?:
	      ks3fs_sha256_update(h, msg, msglen);
	if (err) {
		kfree_sensitive(h);
		goto out;
	}
	err = sha256_final_raw(h, inner);
	if (err)
		goto out;

	for (i = 0; i < SHA256_BLOCK_SIZE; i++)
		pad[i] ^= 0x36 ^ 0x5c;
	h = ks3fs_sha256_begin();
	if (!h) {
		err = -ENOMEM;
		goto out;
	}
	err = ks3fs_sha256_update(h, pad, sizeof(pad)) ?:
	      ks3fs_sha256_update(h, inner, sizeof(inner));
	if (err) {
		kfree_sensitive(h);
		goto out;
	}
	err = sha256_final_raw(h, out);
out:
	memzero_explicit(pad, sizeof(pad));
	memzero_explicit(inner, sizeof(inner));
	return err;
}

/* ASCII only: the kernel's ctype tables classify Latin-1 bytes as alpha. */
static bool unreserved(unsigned char c)
{
	return (c < 0x80 && isalnum(c)) || c == '-' || c == '_' || c == '.' || c == '~';
}

/*
 * RFC 3986 encoding as SigV4 requires.  Returns the encoded length, or
 * -ENAMETOOLONG if @dst is too small.
 */
int ks3fs_uri_encode(char *dst, size_t dstlen, const char *src, bool keep_slash)
{
	static const char hexd[] = "0123456789ABCDEF";
	size_t o = 0;

	for (; *src; src++) {
		unsigned char c = *src;

		if (unreserved(c) || (keep_slash && c == '/')) {
			if (o + 1 >= dstlen)
				return -ENAMETOOLONG;
			dst[o++] = c;
		} else {
			if (o + 3 >= dstlen)
				return -ENAMETOOLONG;
			dst[o++] = '%';
			dst[o++] = hexd[c >> 4];
			dst[o++] = hexd[c & 15];
		}
	}
	if (o >= dstlen)
		return -ENAMETOOLONG;
	dst[o] = '\0';
	return o;
}

static char *uri_encode_dup(const char *src, bool keep_slash)
{
	size_t len = strlen(src) * 3 + 1;
	char *dst = kmalloc(len, GFP_NOFS);

	if (dst && ks3fs_uri_encode(dst, len, src, keep_slash) < 0) {
		kfree(dst);
		return NULL;
	}
	return dst;
}

struct enc_param {
	char *name;
	char *value;
};

static int cmp_param(const void *a, const void *b)
{
	const struct enc_param *pa = a, *pb = b;
	int r = strcmp(pa->name, pb->name);

	return r ? r : strcmp(pa->value, pb->value);
}

/* A growable string buffer; any failure is sticky and reported at the end. */
struct sbuf {
	char *buf;
	size_t len, cap;
	bool oom;
};

static void sb_add(struct sbuf *s, const char *fmt, ...) __printf(2, 3);
static void sb_add(struct sbuf *s, const char *fmt, ...)
{
	va_list ap;
	int n;

	if (s->oom)
		return;
	for (;;) {
		va_start(ap, fmt);
		n = vsnprintf(s->buf + s->len, s->cap - s->len, fmt, ap);
		va_end(ap);
		if (s->len + n < s->cap) {
			s->len += n;
			return;
		} else {
			size_t ncap = max(s->cap * 2, s->len + n + 256);
			char *nb = krealloc(s->buf, ncap, GFP_NOFS);

			if (!nb) {
				s->oom = true;
				return;
			}
			s->buf = nb;
			s->cap = ncap;
		}
	}
}

static int sb_init(struct sbuf *s, size_t cap)
{
	s->buf = kmalloc(cap, GFP_NOFS);
	s->len = 0;
	s->cap = cap;
	s->oom = !s->buf;
	if (s->buf)
		s->buf[0] = '\0';
	return s->oom ? -ENOMEM : 0;
}

/*
 * The x-amz-* headers of one request.  S3 requires every one of them to be
 * signed, and SigV4 wants them sorted, so they are collected here first.
 */
#define MAX_AMZ	14

struct amz_hdr {
	const char *name;
	const char *value;
};

struct amz_set {
	int n;
	struct amz_hdr h[MAX_AMZ];
	char num[4][40];	/* storage for formatted metadata values */
};

static void amz_add(struct amz_set *set, const char *name, const char *value)
{
	if (!WARN_ON_ONCE(set->n >= MAX_AMZ))
		set->h[set->n++] = (struct amz_hdr){ name, value };
}

static int cmp_amz(const void *a, const void *b)
{
	return strcmp(((const struct amz_hdr *)a)->name,
		      ((const struct amz_hdr *)b)->name);
}

static void amz_collect(struct ks3fs_sb_info *sbi, struct ks3fs_req *req,
			struct amz_set *set, const char *payload,
			const char *amzdate, const struct ks3fs_creds *cr)
{
	const struct ks3fs_meta *m = req->meta;

	set->n = 0;
	if (cr) {
		amz_add(set, "x-amz-content-sha256", payload);
		amz_add(set, "x-amz-date", amzdate);
		if (cr->token)
			amz_add(set, "x-amz-security-token", cr->token);
	}
	if (req->copy_source)
		amz_add(set, "x-amz-copy-source", req->copy_source);
	if (req->copy_range)
		amz_add(set, "x-amz-copy-source-range", req->copy_range);
	if (req->copy_if_match)
		amz_add(set, "x-amz-copy-source-if-match", req->copy_if_match);
	if (m && m->has_mode) {
		snprintf(set->num[0], sizeof(set->num[0]), "%u", m->mode);
		amz_add(set, "x-amz-meta-mode", set->num[0]);
	}
	if (m && m->has_uid) {
		snprintf(set->num[1], sizeof(set->num[1]), "%u", m->uid);
		amz_add(set, "x-amz-meta-uid", set->num[1]);
	}
	if (m && m->has_gid) {
		snprintf(set->num[2], sizeof(set->num[2]), "%u", m->gid);
		amz_add(set, "x-amz-meta-gid", set->num[2]);
	}
	if (m && m->has_mtime) {
		if (m->mtime.tv_nsec)
			snprintf(set->num[3], sizeof(set->num[3]), "%lld.%09ld",
				 (long long)m->mtime.tv_sec, m->mtime.tv_nsec);
		else
			snprintf(set->num[3], sizeof(set->num[3]), "%lld",
				 (long long)m->mtime.tv_sec);
		amz_add(set, "x-amz-meta-mtime", set->num[3]);
	}
	if (m && m->xattr)
		amz_add(set, "x-amz-meta-xattr", m->xattr);
	if (req->meta_replace)
		amz_add(set, "x-amz-metadata-directive", "REPLACE");
	sort(set->h, set->n, sizeof(set->h[0]), cmp_amz, NULL);
}

/* Scratch for signing, kept off the stack (it is deep in I/O paths). */
struct sign_scratch {
	u8 k1[SHA256_DIGEST_SIZE], k2[SHA256_DIGEST_SIZE];
	char creq_hash[65], sig[65];
	char datestamp[9];
};

/* Compute the SigV4 signature and append the Authorization header. */
static noinline int sigv4_sign(struct ks3fs_sb_info *sbi,
			       struct ks3fs_req *req, struct sbuf *hdr,
			       const char *path, const char *query,
			       const char *payload, const char *amzdate,
			       const struct amz_set *set,
			       const struct ks3fs_creds *cr)
{
	struct sbuf creq = {}, sts = {}, signed_hdrs = {};
	struct sign_scratch *sc;
	char *kinit = NULL;
	int i, err = -ENOMEM;

	sc = kzalloc(sizeof(*sc), GFP_NOFS);
	if (!sc)
		return -ENOMEM;
	memcpy(sc->datestamp, amzdate, 8);
	if (sb_init(&creq, 1024) || sb_init(&sts, 256) ||
	    sb_init(&signed_hdrs, 256))
		goto out;

	/* canonical headers: host, then the (sorted) x-amz-* set */
	sb_add(&creq, "%s\n%s\n%s\nhost:%s\n", req->method, path, query,
	       sbi->host);
	sb_add(&signed_hdrs, "host");
	for (i = 0; i < set->n; i++) {
		sb_add(&creq, "%s:%s\n", set->h[i].name, set->h[i].value);
		sb_add(&signed_hdrs, ";%s", set->h[i].name);
	}
	sb_add(&creq, "\n%s\n%s", signed_hdrs.buf, payload);
	if (creq.oom || signed_hdrs.oom)
		goto out;

	err = ks3fs_sha256_hex(creq.buf, creq.len, sc->creq_hash);
	if (err)
		goto out;
	err = -ENOMEM;
	sb_add(&sts, "AWS4-HMAC-SHA256\n%s\n%s/%s/s3/aws4_request\n%s",
	       amzdate, sc->datestamp, sbi->region, sc->creq_hash);
	if (sts.oom)
		goto out;

	/* kSigning = HMAC(HMAC(HMAC(HMAC("AWS4"+secret, date), region), "s3"), "aws4_request") */
	kinit = kasprintf(GFP_NOFS, "AWS4%s", cr->sk);
	if (!kinit)
		goto out;
	err = ks3_hmac_sha256(kinit, strlen(kinit), sc->datestamp, 8, sc->k1);
	err = err ?: ks3_hmac_sha256(sc->k1, 32, sbi->region,
				     strlen(sbi->region), sc->k2);
	err = err ?: ks3_hmac_sha256(sc->k2, 32, "s3", 2, sc->k1);
	err = err ?: ks3_hmac_sha256(sc->k1, 32, "aws4_request", 12, sc->k2);
	err = err ?: ks3_hmac_sha256(sc->k2, 32, sts.buf, sts.len, sc->k1);
	if (err)
		goto out;
	bin2hex(sc->sig, sc->k1, 32);

	sb_add(hdr, "Authorization: AWS4-HMAC-SHA256 Credential=%s/%s/%s/s3/aws4_request, SignedHeaders=%s, Signature=%s\r\n",
	       cr->ak, sc->datestamp, sbi->region, signed_hdrs.buf,
	       sc->sig);
	err = hdr->oom ? -ENOMEM : 0;
out:
	kfree_sensitive(kinit);
	kfree_sensitive(sc);
	kfree(creq.buf);
	kfree(sts.buf);
	kfree(signed_hdrs.buf);
	return err;
}

/*
 * Build the complete request head (request line + headers + blank line),
 * signing it if the mount has credentials.
 */
int ks3fs_build_request(struct ks3fs_sb_info *sbi, struct ks3fs_req *req,
			char **out, size_t *outlen)
{
	struct enc_param *ep = NULL;
	struct sbuf path = {}, query = {}, hdr = {};
	struct amz_set *set = NULL;
	char *enc_key = NULL;
	const char *payload = req->payload_sha256 ?: EMPTY_SHA256;
	bool sign = ks3fs_signed(sbi);
	struct ks3fs_creds cr;
	char amzdate[17];
	struct tm tm;
	int i, err = -ENOMEM;

	if (sign) {
		err = ks3fs_creds_get(sbi, &cr);
		if (err)
			return err;
	}
	err = -ENOMEM;
	time64_to_tm(ktime_get_real_seconds(), 0, &tm);
	snprintf(amzdate, sizeof(amzdate), "%04ld%02d%02dT%02d%02d%02dZ",
		 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
		 tm.tm_hour, tm.tm_min, tm.tm_sec);

	/* canonical URI */
	if (sb_init(&path, 256))
		goto out;
	if (!sbi->vhost)
		sb_add(&path, "/%s", sbi->bucket);
	if (req->key) {
		enc_key = uri_encode_dup(req->key, true);
		if (!enc_key)
			goto out;
		sb_add(&path, "/%s", enc_key);
	} else if (sbi->vhost) {
		sb_add(&path, "/");
	}

	/* canonical query string, sorted */
	if (sb_init(&query, 256))
		goto out;
	if (req->nr_params) {
		ep = kcalloc(req->nr_params, sizeof(*ep), GFP_NOFS);
		if (!ep)
			goto out;
		for (i = 0; i < req->nr_params; i++) {
			ep[i].name = uri_encode_dup(req->params[i].name, false);
			ep[i].value = uri_encode_dup(req->params[i].value ?: "",
						     false);
			if (!ep[i].name || !ep[i].value)
				goto out;
		}
		sort(ep, req->nr_params, sizeof(*ep), cmp_param, NULL);
		for (i = 0; i < req->nr_params; i++)
			sb_add(&query, "%s%s=%s", i ? "&" : "", ep[i].name,
			       ep[i].value);
	}

	if (sb_init(&hdr, 1024))
		goto out;
	sb_add(&hdr, "%s %s%s%s HTTP/1.1\r\nHost: %s\r\n", req->method,
	       path.buf, query.len ? "?" : "", query.buf, sbi->host);
	sb_add(&hdr, "User-Agent: ks3fs/0.3\r\n");
	if (req->range)
		sb_add(&hdr, "Range: %s\r\n", req->range);
	if (req->if_match)
		sb_add(&hdr, "If-Match: %s\r\n", req->if_match);
	if (req->body_len || !strcmp(req->method, "PUT") ||
	    !strcmp(req->method, "POST"))
		sb_add(&hdr, "Content-Length: %lld\r\n", req->body_len);

	set = kmalloc(sizeof(*set), GFP_NOFS);
	if (!set)
		goto out;
	amz_collect(sbi, req, set, payload, amzdate, sign ? &cr : NULL);
	for (i = 0; i < set->n; i++)
		sb_add(&hdr, "%s: %s\r\n", set->h[i].name, set->h[i].value);
	if (sign) {
		err = sigv4_sign(sbi, req, &hdr, path.buf, query.buf, payload,
				 amzdate, set, &cr);
		if (err)
			goto out;
		err = -ENOMEM;
	}
	sb_add(&hdr, "\r\n");
	if (hdr.oom)
		goto out;

	*out = hdr.buf;
	*outlen = hdr.len;
	hdr.buf = NULL;
	err = 0;
out:
	if (ep) {
		for (i = 0; i < req->nr_params; i++) {
			kfree(ep[i].name);
			kfree(ep[i].value);
		}
		kfree(ep);
	}
	if (sign)
		ks3fs_creds_put(&cr);
	kfree(set);
	kfree(enc_key);
	kfree(path.buf);
	kfree(query.buf);
	kfree(hdr.buf);
	return err;
}
