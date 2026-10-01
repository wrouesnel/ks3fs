// SPDX-License-Identifier: GPL-2.0
/*
 * Just enough XML handling for S3 responses (ListObjectsV2 and errors).
 * S3 responses are machine generated and flat, so a tag scanner suffices.
 * Listings are requested with encoding-type=url so keys arrive
 * percent-encoded and are decoded here after entity decoding.
 */
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/time.h>
#include <linux/ctype.h>

#include "ks3fs.h"

/* Find the content of the first <tag>...</tag> in [p, end). */
static const char *find_elem(const char *p, const char *end, const char *tag,
			     size_t *len, const char **after)
{
	size_t tl = strlen(tag);
	const char *s = p;

	while (s < end) {
		const char *open = strnstr(s, "<", end - s);
		const char *cstart, *close;

		if (!open || open + tl + 2 > end)
			return NULL;
		if (!strncmp(open + 1, tag, tl) &&
		    (open[tl + 1] == '>' || open[tl + 1] == ' ')) {
			cstart = strnchr(open, end - open, '>');
			if (!cstart)
				return NULL;
			cstart++;
			if (cstart[-2] == '/') {	/* <Tag/> */
				*len = 0;
				if (after)
					*after = cstart;
				return cstart;
			}
			for (close = cstart; close + tl + 3 <= end; close++) {
				if (close[0] == '<' && close[1] == '/' &&
				    !strncmp(close + 2, tag, tl) &&
				    close[tl + 2] == '>') {
					*len = close - cstart;
					if (after)
						*after = close + tl + 3;
					return cstart;
				}
			}
			return NULL;
		}
		s = open + 1;
	}
	return NULL;
}

static int hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	c = tolower(c);
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	return -1;
}

/* Decode XML entities, then optionally %XX / '+' URL encoding.  In place ok. */
static size_t decode(char *dst, const char *src, size_t len, bool url)
{
	size_t i = 0, o = 0;

	while (i < len) {
		if (src[i] == '&') {
			static const struct { const char *e; char c; } ents[] = {
				{ "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' },
				{ "&quot;", '"' }, { "&apos;", '\'' },
			};
			int k;
			bool done = false;

			for (k = 0; k < ARRAY_SIZE(ents); k++) {
				size_t el = strlen(ents[k].e);

				if (i + el <= len && !strncmp(src + i, ents[k].e, el)) {
					dst[o++] = ents[k].c;
					i += el;
					done = true;
					break;
				}
			}
			if (!done && i + 3 < len && src[i + 1] == '#') {
				const char *semi = strnchr(src + i, len - i, ';');
				unsigned int v;
				char num[12];
				size_t nl;

				if (semi && (nl = semi - (src + i + 2)) < sizeof(num)) {
					memcpy(num, src + i + 2, nl);
					num[nl] = '\0';
					if ((num[0] == 'x' ? kstrtouint(num + 1, 16, &v) :
					     kstrtouint(num, 10, &v)) == 0 && v < 0x80) {
						dst[o++] = v;
						i = semi - src + 1;
						done = true;
					}
				}
			}
			if (!done)
				dst[o++] = src[i++];
		} else {
			dst[o++] = src[i++];
		}
	}
	if (!url)
		return o;

	len = o;
	for (i = 0, o = 0; i < len; i++) {
		if (dst[i] == '%' && i + 2 < len &&
		    hexval(dst[i + 1]) >= 0 && hexval(dst[i + 2]) >= 0) {
			dst[o++] = hexval(dst[i + 1]) << 4 | hexval(dst[i + 2]);
			i += 2;
		} else if (dst[i] == '+') {
			dst[o++] = ' ';
		} else {
			dst[o++] = dst[i];
		}
	}
	return o;
}

static bool elem_str(const char *p, const char *end, const char *tag,
		     char *out, size_t outlen, bool url)
{
	size_t len;
	const char *v = find_elem(p, end, tag, &len, NULL);

	if (!v || len >= outlen)
		return false;
	len = decode(out, v, len, url);
	out[len] = '\0';
	return true;
}

/* "2025-12-23T01:30:30.000Z" */
time64_t ks3fs_parse_iso8601(const char *s, size_t len)
{
	unsigned int y, mo, d, h, mi, sec;
	char buf[32];

	if (len >= sizeof(buf))
		len = sizeof(buf) - 1;
	memcpy(buf, s, len);
	buf[len] = '\0';
	if (sscanf(buf, "%u-%u-%uT%u:%u:%u", &y, &mo, &d, &h, &mi, &sec) != 6)
		return 0;
	return mktime64(y, mo, d, h, mi, sec);
}

/* RFC 7231 IMF-fixdate: "Wed, 30 Sep 2026 13:28:35 GMT" */
time64_t ks3fs_parse_http_date(const char *s)
{
	static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
	unsigned int d, y, h, mi, sec;
	char mon[4];
	const char *m;

	s = strchr(s, ',');
	if (!s)
		return 0;
	if (sscanf(s + 1, " %u %3s %u %u:%u:%u", &d, mon, &y, &h, &mi, &sec) != 6)
		return 0;
	mon[3] = '\0';
	m = strstr(months, mon);
	if (!m || strlen(mon) != 3)
		return 0;
	return mktime64(y, (m - months) / 3 + 1, d, h, mi, sec);
}

int ks3fs_xml_parse_list(const char *xml, size_t len, ks3fs_list_cb cb,
			 void *arg, bool *truncated, char **next_token)
{
	const char *end = xml + len, *p;
	char *key;
	char tmp[32];
	size_t elen;
	const char *v, *after;
	bool url;
	int err = 0;

	*truncated = false;
	*next_token = NULL;

	if (!strnstr(xml, "<ListBucketResult", len))
		return -EPROTO;

	/* some servers ignore encoding-type=url; only decode if they say so */
	url = elem_str(xml, end, "EncodingType", tmp, sizeof(tmp), false) &&
	      !strcmp(tmp, "url");

	key = kmalloc(KS3FS_MAX_KEY * 3 + 1, GFP_NOFS);
	if (!key)
		return -ENOMEM;

	/* objects */
	for (p = xml; (v = find_elem(p, end, "Contents", &elen, &after)); p = after) {
		const char *ce = v + elen;
		char etag[KS3FS_ETAG_LEN] = "";
		loff_t size = 0;
		time64_t mtime = 0;
		const char *tv;
		size_t tl;

		if (!elem_str(v, ce, "Key", key, KS3FS_MAX_KEY * 3 + 1, url))
			continue;
		if (elem_str(v, ce, "Size", tmp, sizeof(tmp), false) &&
		    kstrtoll(tmp, 10, &size))
			size = 0;
		tv = find_elem(v, ce, "LastModified", &tl, NULL);
		if (tv)
			mtime = ks3fs_parse_iso8601(tv, tl);
		elem_str(v, ce, "ETag", etag, sizeof(etag), false);
		err = cb(arg, key, strlen(key), false, size, mtime, etag);
		if (err)
			goto out;
	}

	/* common prefixes ("subdirectories") */
	for (p = xml; (v = find_elem(p, end, "CommonPrefixes", &elen, &after)); p = after) {
		if (!elem_str(v, v + elen, "Prefix", key, KS3FS_MAX_KEY * 3 + 1, url))
			continue;
		err = cb(arg, key, strlen(key), true, 0, 0, "");
		if (err)
			goto out;
	}

	if (elem_str(xml, end, "IsTruncated", tmp, sizeof(tmp), false) &&
	    !strcmp(tmp, "true")) {
		v = find_elem(xml, end, "NextContinuationToken", &elen, NULL);
		if (v) {
			*next_token = kmalloc(elen + 1, GFP_NOFS);
			if (!*next_token) {
				err = -ENOMEM;
				goto out;
			}
			elen = decode(*next_token, v, elen, false);
			(*next_token)[elen] = '\0';
			*truncated = true;
		}
	}
out:
	kfree(key);
	return err;
}

/* Entity-decoded text of the first <tag> element, or -ENOENT. */
int ks3fs_xml_get(const char *xml, size_t len, const char *tag, char *out,
		  size_t outlen)
{
	if (!xml || !elem_str(xml, xml + len, tag, out, outlen, false))
		return -ENOENT;
	return 0;
}

int ks3fs_xml_error_code(const char *xml, size_t len, char *code, size_t codelen)
{
	if (!xml || !elem_str(xml, xml + len, "Code", code, codelen, false))
		return -ENOENT;
	return 0;
}
