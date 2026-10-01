// SPDX-License-Identifier: GPL-2.0
/*
 * Per-object POSIX metadata, stored as user metadata in the s3fs-fuse
 * layout so buckets stay interoperable with it (and with rclone):
 *   x-amz-meta-mode   decimal st_mode, including the file type bits
 *   x-amz-meta-uid    decimal uid     x-amz-meta-gid  decimal gid
 *   x-amz-meta-mtime  seconds since the epoch, optionally with a fraction
 *   x-amz-meta-xattr  extended attributes (see xattr.c)
 */
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/ctype.h>
#include <linux/slab.h>

#include "ks3fs.h"

void ks3fs_meta_clear(struct ks3fs_meta *m)
{
	memset(m, 0, sizeof(*m));
}

/* Free what @m owns (its xattr header) and clear it. */
void ks3fs_meta_release(struct ks3fs_meta *m)
{
	kfree(m->xattr);
	ks3fs_meta_clear(m);
}

static bool parse_u32(const char *val, u32 *out)
{
	return kstrtou32(val, 10, out) == 0;
}

static bool parse_time(const char *val, struct timespec64 *ts)
{
	char buf[32], *dot;
	long long sec;
	u32 nsec = 0;
	size_t i;

	strscpy(buf, val, sizeof(buf));
	dot = strchr(buf, '.');
	if (dot) {
		*dot++ = '\0';
		bool more = true;

		/* up to nine fractional digits, right-padded with zeros */
		for (i = 0; i < 9; i++) {
			nsec *= 10;
			if (more && isdigit(dot[i]))
				nsec += dot[i] - '0';
			else
				more = false;
		}
	}
	if (kstrtoll(buf, 10, &sec))
		return false;
	ts->tv_sec = sec;
	ts->tv_nsec = nsec;
	return true;
}

/* @name is the header name with the "x-amz-meta-" prefix removed. */
void ks3fs_meta_parse_header(struct ks3fs_meta *m, const char *name,
			     const char *val)
{
	u32 v;

	if (!strcasecmp(name, "mode")) {
		if (parse_u32(val, &v)) {
			m->mode = v;
			m->has_mode = true;
		}
	} else if (!strcasecmp(name, "uid")) {
		m->has_uid = parse_u32(val, &m->uid);
	} else if (!strcasecmp(name, "gid")) {
		m->has_gid = parse_u32(val, &m->gid);
	} else if (!strcasecmp(name, "mtime")) {
		m->has_mtime = parse_time(val, &m->mtime);
	}
}
