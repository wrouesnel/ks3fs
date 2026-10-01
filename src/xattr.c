// SPDX-License-Identifier: GPL-2.0
/*
 * Extended attributes (user.* and POSIX ACLs), stored in the s3fs-fuse
 * layout: one header,
 *   x-amz-meta-xattr: urlencode({"user.name":"<base64 value>",...})
 * (ACLs as system.posix_acl_access/default, in their xattr form),
 * fetched with the object's HEAD and rewritten with its other metadata.
 * S3 limits user metadata to 2 KiB per object, which bounds the total.
 *
 * The inode keeps the header value as stored (ki->xattr, NULL when there
 * are none, under ki->lock); get/list decode it, set re-encodes it.
 */
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/ctype.h>
#include <linux/hex.h>
#include <linux/xattr.h>
#include <linux/posix_acl.h>
#include <linux/posix_acl_xattr.h>

#include "ks3fs.h"

/* what fits in 2 KiB next to x-amz-meta-{mode,uid,gid,mtime} */
#define XATTR_HDR_MAX	1900

struct xent {
	char *name;		/* full name, "user.foo" */
	u8 *val;
	size_t len;
};

struct xlist {
	int n;
	struct xent e[];
};

static void xlist_free(struct xlist *xl)
{
	int i;

	if (!xl)
		return;
	for (i = 0; i < xl->n; i++) {
		kfree(xl->e[i].name);
		kfree(xl->e[i].val);
	}
	kfree(xl);
}

/* ---------- base64 (RFC 4648, padded) and percent-encoding ---------- */

static const char b64[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t b64_encode(const u8 *src, size_t len, char *dst)
{
	size_t i, o = 0;

	for (i = 0; i < len; i += 3) {
		u32 v = src[i] << 16;

		if (i + 1 < len)
			v |= src[i + 1] << 8;
		if (i + 2 < len)
			v |= src[i + 2];
		dst[o++] = b64[(v >> 18) & 63];
		dst[o++] = b64[(v >> 12) & 63];
		dst[o++] = i + 1 < len ? b64[(v >> 6) & 63] : '=';
		dst[o++] = i + 2 < len ? b64[v & 63] : '=';
	}
	return o;
}

static int b64_val(char c)
{
	const char *p = c ? strchr(b64, c) : NULL;

	return p ? p - b64 : -1;
}

/* Returns the decoded length, or -EINVAL. */
static int b64_decode(const char *src, size_t len, u8 *dst)
{
	u32 v = 0;
	int bits = 0, o = 0;
	size_t i;

	for (i = 0; i < len && src[i] != '='; i++) {
		int d = b64_val(src[i]);

		if (d < 0)
			return -EINVAL;
		v = (v << 6) | d;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			dst[o++] = v >> bits;
		}
	}
	return o;
}

static bool unreserved(char c)
{
	return isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~';
}

static int hexval(char c)
{
	return hex_to_bin(c);
}

/* In-place percent-decoding. */
static int pct_decode(char *s)
{
	char *o = s;

	for (; *s; s++) {
		if (*s == '%') {
			int hi = hexval(s[1]), lo = hi < 0 ? -1 : hexval(s[2]);

			if (hi < 0 || lo < 0)
				return -EINVAL;
			*o++ = hi << 4 | lo;
			s += 2;
		} else {
			*o++ = *s == '+' ? ' ' : *s;
		}
	}
	*o = '\0';
	return 0;
}

/* ---------- header value <-> list ---------- */

/* Parse a JSON string literal at *p (no escapes beyond \" and \\). */
static char *json_str(char **p)
{
	char *s = *p, *start, *o;

	if (*s != '"')
		return NULL;
	start = o = ++s;
	for (; *s && *s != '"'; s++) {
		if (*s == '\\' && s[1])
			s++;
		*o++ = *s;
	}
	if (*s != '"')
		return NULL;
	*o = '\0';
	*p = s + 1;
	return start;
}

static struct xlist *xattr_decode(const char *hdr)
{
	struct xlist *xl;
	char *buf, *p;
	int cap = 0, err = -EINVAL;

	buf = kstrdup(hdr ?: "", GFP_KERNEL);
	if (!buf)
		return ERR_PTR(-ENOMEM);
	for (p = buf; *p; p++)
		cap += *p == ':' || *p == '%';	/* bound on entries */
	xl = kzalloc(struct_size(xl, e, cap + 1), GFP_KERNEL);
	if (!xl) {
		kfree(buf);
		return ERR_PTR(-ENOMEM);
	}
	if (!hdr || !*hdr)
		goto done;
	if (pct_decode(buf))
		goto bad;
	p = skip_spaces(buf);
	if (*p++ != '{')
		goto bad;
	for (;;) {
		char *name, *val;
		struct xent *e;
		int n;

		p = skip_spaces(p);
		if (*p == '}')
			break;
		name = json_str(&p);
		p = skip_spaces(p);
		if (!name || *p++ != ':')
			goto bad;
		p = skip_spaces(p);
		val = json_str(&p);
		if (!val || xl->n > cap)
			goto bad;
		e = &xl->e[xl->n];
		e->name = kstrdup(name, GFP_KERNEL);
		e->val = kmalloc(strlen(val) + 1, GFP_KERNEL);
		if (!e->name || !e->val) {
			kfree(e->name);
			kfree(e->val);
			err = -ENOMEM;
			goto bad;
		}
		n = b64_decode(val, strlen(val), e->val);
		if (n < 0) {
			kfree(e->name);
			kfree(e->val);
			goto bad;
		}
		e->len = n;
		xl->n++;
		p = skip_spaces(p);
		if (*p == ',')
			p++;
		else if (*p != '}')
			goto bad;
	}
done:
	kfree(buf);
	return xl;
bad:
	kfree(buf);
	xlist_free(xl);
	return ERR_PTR(err);
}

/* Encode @xl as a header value (NULL for an empty list). */
static char *xattr_encode(const struct xlist *xl, int *err)
{
	size_t raw = 2, i;
	char *json, *out, *o;
	int k;

	*err = 0;
	if (!xl->n)
		return NULL;
	for (k = 0; k < xl->n; k++)
		raw += strlen(xl->e[k].name) + 6 + DIV_ROUND_UP(xl->e[k].len, 3) * 4;
	if (raw > XATTR_HDR_MAX) {
		*err = -ENOSPC;
		return NULL;
	}
	json = kmalloc(raw + 1, GFP_KERNEL);
	if (!json) {
		*err = -ENOMEM;
		return NULL;
	}
	o = json;
	*o++ = '{';
	for (k = 0; k < xl->n; k++) {
		o += sprintf(o, "%s\"%s\":\"", k ? "," : "", xl->e[k].name);
		o += b64_encode(xl->e[k].val, xl->e[k].len, o);
		*o++ = '"';
	}
	*o++ = '}';
	*o = '\0';

	out = kmalloc(3 * strlen(json) + 1, GFP_KERNEL);
	if (!out) {
		kfree(json);
		*err = -ENOMEM;
		return NULL;
	}
	o = out;
	for (i = 0; json[i]; i++) {
		if (unreserved(json[i]))
			*o++ = json[i];
		else
			o += sprintf(o, "%%%02X", (u8)json[i]);
	}
	*o = '\0';
	kfree(json);
	if (o - out > XATTR_HDR_MAX) {
		kfree(out);
		*err = -ENOSPC;
		return NULL;
	}
	return out;
}

/* ---------- the inode's set ---------- */

/*
 * Make sure the inode's xattrs are known: inodes instantiated from a
 * listing have not seen the object's headers yet.
 */
int ks3fs_xattr_load(struct inode *inode)
{
	struct ks3fs_sb_info *sbi = KS3_SB(inode->i_sb);
	struct ks3fs_inode *ki = KS3_I(inode);
	struct ks3fs_attr attr;
	char *key;
	int err;

	if (test_bit(KS3_I_XATTR_KNOWN, &ki->flags))
		return 0;
	key = ks3fs_inode_key(inode);
	if (!key)
		return -ENOMEM;
	if (!*key || !test_bit(KS3_I_REMOTE, &ki->flags)) {
		err = 0;	/* no object (yet): nothing stored */
		attr.meta.xattr = NULL;
	} else {
		err = ks3fs_s3_head_xattr(sbi, key, &attr);
		if (err == -ENOENT && S_ISDIR(inode->i_mode)) {
			err = 0;	/* directory without a marker */
			attr.meta.xattr = NULL;
		}
	}
	kfree(key);
	if (err)
		return err;
	spin_lock(&ki->lock);
	if (!test_bit(KS3_I_XATTR_KNOWN, &ki->flags)) {
		swap(ki->xattr, attr.meta.xattr);
		set_bit(KS3_I_XATTR_KNOWN, &ki->flags);
	}
	spin_unlock(&ki->lock);
	kfree(attr.meta.xattr);
	return 0;
}

/* Replace the inode's set from remote metadata (takes @hdr). */
void ks3fs_xattr_update(struct inode *inode, char *hdr)
{
	struct ks3fs_inode *ki = KS3_I(inode);

	spin_lock(&ki->lock);
	swap(ki->xattr, hdr);
	set_bit(KS3_I_XATTR_KNOWN, &ki->flags);
	spin_unlock(&ki->lock);
	kfree(hdr);
	forget_all_cached_acls(inode);	/* may have changed remotely */
}

/* A copy of the stored header value, for an upload or metadata rewrite. */
int ks3fs_xattr_header(struct inode *inode, char **out)
{
	struct ks3fs_inode *ki = KS3_I(inode);
	int err = ks3fs_xattr_load(inode);

	*out = NULL;
	if (err)
		return err;
	spin_lock(&ki->lock);
	if (ki->xattr)
		*out = kstrdup(ki->xattr, GFP_ATOMIC);
	err = ki->xattr && !*out ? -ENOMEM : 0;
	spin_unlock(&ki->lock);
	return err;
}

static struct xlist *inode_xlist(struct inode *inode)
{
	char *hdr;
	struct xlist *xl;
	int err = ks3fs_xattr_header(inode, &hdr);

	if (err)
		return ERR_PTR(err);
	xl = xattr_decode(hdr);
	kfree(hdr);
	return xl;
}

static int find(const struct xlist *xl, const char *name)
{
	int i;

	for (i = 0; i < xl->n; i++)
		if (!strcmp(xl->e[i].name, name))
			return i;
	return -1;
}

/* A copy of xattr @full's value in *@val; returns its length or -errno. */
static int xattr_get_full(struct inode *inode, const char *full, void **val)
{
	struct xlist *xl = inode_xlist(inode);
	int i, ret;

	*val = NULL;
	if (IS_ERR(xl))
		return PTR_ERR(xl);
	i = find(xl, full);
	if (i < 0) {
		ret = -ENODATA;
	} else {
		ret = xl->e[i].len;
		*val = kmemdup(xl->e[i].val, xl->e[i].len ?: 1, GFP_KERNEL);
		if (!*val)
			ret = -ENOMEM;
	}
	xlist_free(xl);
	return ret;
}

/* Set (or with @value NULL, remove) xattr @full; stored lazily, as chmod. */
static int xattr_set_full(struct inode *inode, const char *full,
			  const void *value, size_t size, int flags)
{
	struct ks3fs_inode *ki = KS3_I(inode);
	struct xlist *xl, *nl;
	char *hdr;
	int i, err;

	/* the bucket root has no object to keep them on */
	if (!KS3_SB(inode->i_sb)->meta || inode == d_inode(inode->i_sb->s_root))
		return -EOPNOTSUPP;
	if (size > XATTR_HDR_MAX)
		return -E2BIG;
	xl = inode_xlist(inode);
	if (IS_ERR(xl))
		return PTR_ERR(xl);
	i = find(xl, full);
	err = -ENODATA;
	if (i < 0 && (flags & XATTR_REPLACE))
		goto out;
	err = -EEXIST;
	if (i >= 0 && (flags & XATTR_CREATE))
		goto out;
	err = 0;
	if (i < 0 && !value)
		goto out;	/* removing what is not there */

	/* the new list, sharing the entries' strings with the old one */
	err = -ENOMEM;
	nl = kzalloc(struct_size(nl, e, xl->n + 1), GFP_KERNEL);
	if (!nl)
		goto out;
	for (i = 0; i < xl->n; i++)
		if (strcmp(xl->e[i].name, full))
			nl->e[nl->n++] = xl->e[i];
	if (value)
		nl->e[nl->n++] = (struct xent){ (char *)full, (u8 *)value, size };
	hdr = xattr_encode(nl, &err);
	kfree(nl);
	if (err)
		goto out;

	spin_lock(&ki->lock);
	swap(ki->xattr, hdr);
	spin_unlock(&ki->lock);
	kfree(hdr);
	inode_set_ctime_current(inode);
	set_bit(KS3_I_META_DIRTY, &ki->flags);
	ks3fs_schedule_meta_writeback(inode);
out:
	xlist_free(xl);
	return err;
}

static int ks3fs_xattr_get(const struct xattr_handler *h,
			   struct dentry *unused, struct inode *inode,
			   const char *name, void *buffer, size_t size)
{
	char *full = kasprintf(GFP_KERNEL, "%s%s", h->prefix, name);
	void *val;
	int ret;

	if (!full)
		return -ENOMEM;
	ret = xattr_get_full(inode, full, &val);
	if (ret > 0 && size) {
		if (size < ret)
			ret = -ERANGE;
		else
			memcpy(buffer, val, ret);
	}
	kfree(val);
	kfree(full);
	return ret;
}

static int ks3fs_xattr_set(const struct xattr_handler *h,
			   struct mnt_idmap *idmap, struct dentry *unused,
			   struct inode *inode, const char *name,
			   const void *value, size_t size, int flags)
{
	char *full = kasprintf(GFP_KERNEL, "%s%s", h->prefix, name);
	int err;

	if (!full)
		return -ENOMEM;
	err = xattr_set_full(inode, full, value, size, flags);
	kfree(full);
	return err;
}

/* ---------- POSIX ACLs ---------- */

struct posix_acl *ks3fs_get_acl(struct inode *inode, int type, bool rcu)
{
	struct posix_acl *acl;
	void *val;
	int len;

	if (rcu)
		return ERR_PTR(-ECHILD);	/* may need a HEAD */
	len = xattr_get_full(inode, posix_acl_xattr_name(type), &val);
	if (len == -ENODATA)
		return NULL;
	if (len < 0)
		return ERR_PTR(len);
	acl = posix_acl_from_xattr(&init_user_ns, val, len);
	kfree(val);
	return acl;
}

int ks3fs_set_acl(struct mnt_idmap *idmap, struct dentry *dentry,
		  struct posix_acl *acl, int type)
{
	struct inode *inode = d_inode(dentry);
	umode_t mode = inode->i_mode;
	void *val = NULL;
	size_t len = 0;
	int err;

	if (type == ACL_TYPE_DEFAULT && !S_ISDIR(inode->i_mode))
		return acl ? -EACCES : 0;
	if (type == ACL_TYPE_ACCESS && acl) {
		/* the mode follows; an ACL the mode alone expresses is dropped */
		err = posix_acl_update_mode(idmap, inode, &mode, &acl);
		if (err)
			return err;
	}
	if (acl) {
		val = ks3_acl_to_xattr(acl, &len);
		if (!val)
			return -ENOMEM;
	}
	err = xattr_set_full(inode, posix_acl_xattr_name(type), val, len, 0);
	kfree(val);
	if (err)
		return err;
	if (mode != inode->i_mode) {
		inode->i_mode = mode;
		set_bit(KS3_I_META_DIRTY, &KS3_I(inode)->flags);
		ks3fs_schedule_meta_writeback(inode);
	}
	set_cached_acl(inode, type, acl);
	return 0;
}

/*
 * The xattr header for a new object with the ACLs posix_acl_create()
 * derived for it (NULL if none).  Consumes the ACL references.
 */
int ks3fs_acl_header(struct posix_acl *default_acl, struct posix_acl *acl,
		     char **hdr)
{
	struct xlist *xl;
	int err = 0;

	*hdr = NULL;
	xl = kzalloc(struct_size(xl, e, 2), GFP_KERNEL);
	if (!xl) {
		err = -ENOMEM;
		goto out;
	}
	if (acl) {
		xl->e[xl->n].name = kstrdup(XATTR_NAME_POSIX_ACL_ACCESS, GFP_KERNEL);
		xl->e[xl->n].val = ks3_acl_to_xattr(acl, &xl->e[xl->n].len);
		xl->n++;
	}
	if (default_acl) {
		xl->e[xl->n].name = kstrdup(XATTR_NAME_POSIX_ACL_DEFAULT, GFP_KERNEL);
		xl->e[xl->n].val = ks3_acl_to_xattr(default_acl,
						   &xl->e[xl->n].len);
		xl->n++;
	}
	if (xl->n && (!xl->e[0].name || !xl->e[0].val ||
		      (xl->n > 1 && (!xl->e[1].name || !xl->e[1].val))))
		err = -ENOMEM;
	else
		*hdr = xattr_encode(xl, &err);
	xlist_free(xl);
out:
	posix_acl_release(acl);
	posix_acl_release(default_acl);
	return err;
}

ssize_t ks3fs_listxattr(struct dentry *dentry, char *buf, size_t size)
{
	struct xlist *xl = inode_xlist(d_inode(dentry));
	ssize_t total = 0;
	size_t n;
	int i;

	if (IS_ERR(xl))
		return PTR_ERR(xl);
	for (i = 0; i < xl->n; i++) {
		if (strncmp(xl->e[i].name, XATTR_USER_PREFIX,
			    XATTR_USER_PREFIX_LEN) &&
		    strcmp(xl->e[i].name, XATTR_NAME_POSIX_ACL_ACCESS) &&
		    strcmp(xl->e[i].name, XATTR_NAME_POSIX_ACL_DEFAULT))
			continue;	/* another namespace (other clients) */
		n = strlen(xl->e[i].name) + 1;
		if (size) {
			if (total + n > size) {
				total = -ERANGE;
				break;
			}
			memcpy(buf + total, xl->e[i].name, n);
		}
		total += n;
	}
	xlist_free(xl);
	return total;
}

static const struct xattr_handler ks3fs_user_xattr = {
	.prefix	= XATTR_USER_PREFIX,
	.get	= ks3fs_xattr_get,
	.set	= ks3fs_xattr_set,
};

const struct xattr_handler * const ks3fs_xattr_handlers[] = {
	&ks3fs_user_xattr,
	NULL,
};
