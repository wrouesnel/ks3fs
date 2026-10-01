/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ks3fs - an in-kernel filesystem backed by an S3-compatible object store.
 */
#ifndef _KS3FS_H
#define _KS3FS_H

#include <linux/fs.h>
#include <linux/net.h>
#include <linux/socket.h>
#include <linux/in.h>
#include <linux/in6.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/time64.h>
#include <linux/uidgid.h>
#include <linux/workqueue.h>

#include "compat.h"

#define KS3FS_MAGIC		0x6b733366	/* "ks3f" */
#define KS3FS_MAX_KEY		1024		/* S3 key length limit (bytes) */
#define KS3FS_ETAG_LEN		80
#define KS3FS_MAX_PUT		(5ULL << 30)	/* single PUT/COPY limit */
#define KS3FS_MPU_MAX_PARTS	10000
#define KS3FS_MAX_OBJECT	(5ULL << 40)	/* S3 object size limit */
#define KS3FS_PART_MIN		(5ULL << 20)	/* smallest non-final part */
#define KS3FS_MAX_XML		(16 << 20)	/* cap on buffered XML bodies */
#define KS3FS_MAX_DIR_MOVE	100000	/* objects; beyond that mv(1) copies */
#define KS3FS_ORPHANS		".ks3fs-orphans"	/* at the mount root */

/* ---------- mount-wide state ---------- */

struct ks3fs_conn;

struct ks3fs_sb_info {
	/* endpoint */
	struct sockaddr_storage addr;
	int addrlen;
	struct net *net;
	char *host;		/* Host header value (already includes bucket if vhost) */
	char *bucket;
	char *prefix;		/* "" or "some/prefix/" */
	char *region;
	bool vhost;		/* virtual-hosted style addressing */

	/* transport security */
	bool tls;
	char *tls_peername;	/* name the server certificate must match */

	/* credentials; NULL access_key means anonymous requests */
	char *access_key;
	char *secret_key;
	char *session_token;
	struct key *creds_key;	/* creds_key=: logon key read per request */
	char *creds_name;

	/* presentation */
	kuid_t uid;
	kgid_t gid;
	umode_t file_mode;
	umode_t dir_mode;
	unsigned long ttl;	/* metadata cache lifetime, jiffies */
	unsigned long timeout;	/* socket timeout, jiffies */
	unsigned long retry_timeout; /* soft mounts: give up after, jiffies */
	bool hard;		/* retry until killed */
	bool meta;		/* store POSIX metadata in x-amz-meta-* */
	u64 part_size;		/* multipart upload part size */
	int parallel;		/* concurrent requests for bulk operations */
	unsigned long state;	/* KS3_SB_* bits */

	/* idle keep-alive connections */
	spinlock_t conn_lock;
	struct list_head idle_conns;
	int nr_idle;


	/* lazy metadata writeback (see super.c) */
	struct super_block *sb;
	struct delayed_work meta_work;
};

#define KS3FS_META_DELAY	(5 * HZ)

enum {
	KS3_SB_OUTAGE,		/* "server not responding" has been logged */
};

static inline struct ks3fs_sb_info *KS3_SB(struct super_block *sb)
{
	return sb->s_fs_info;
}

/* ---------- inode state ---------- */

enum {
	KS3_I_DIRTY,		/* local content differs from the remote object */
	KS3_I_REMOTE,		/* an object (or dir marker) exists remotely */
	KS3_I_META_DIRTY,	/* mode/owner/times not yet stored */
	KS3_I_ORPHAN,		/* unlinked while open: object moved aside */
	KS3_I_XATTR_KNOWN,	/* @xattr reflects the object's headers */
	KS3_I_TMPFILE,		/* O_TMPFILE not linked yet: no object */
};

struct ks3fs_inode {
	struct inode vfs_inode;
	spinlock_t lock;	/* protects key and etag */
	char *key;		/* full object key; dirs end in '/', root may be "" */
	char etag[KS3FS_ETAG_LEN];
	unsigned long flags;
	unsigned long attr_time; /* jiffies when attributes were last fetched */
	loff_t remote_size;	/* bytes of i_size backed by the remote object */
	struct ks3fs_mpu *mpu;	/* open multipart upload, under i_rwsem */
	int stream_next;	/* next part the write path may stream out */
	atomic_t opens;		/* open files (silly rename on unlink) */
	struct ks3fs_snap *snap; /* dirs: recent listing, under @lock */
	char *xattr;		/* x-amz-meta-xattr value or NULL, under @lock */
};

static inline struct ks3fs_inode *KS3_I(struct inode *inode)
{
	return container_of(inode, struct ks3fs_inode, vfs_inode);
}

/* ---------- per-object metadata (s3fs-fuse compatible x-amz-meta-*) ---------- */

struct ks3fs_meta {
	bool has_mode, has_uid, has_gid, has_mtime, has_ctime;
	bool has_xattr;		/* @xattr is known (NULL: there are none) */
	umode_t mode;		/* including S_IFMT */
	u32 uid, gid;		/* in the initial user namespace */
	struct timespec64 mtime, ctime;
	char *xattr;		/* x-amz-meta-xattr value, owned (kmalloc) */
};

void ks3fs_meta_clear(struct ks3fs_meta *m);
void ks3fs_meta_release(struct ks3fs_meta *m);
void ks3fs_meta_parse_header(struct ks3fs_meta *m, const char *name,
			     const char *val);

/* Attributes of a remote entry, from HEAD or a listing. */
struct ks3fs_attr {
	bool is_dir;
	bool has_marker;	/* dir: an explicit "dir/" object exists */
	loff_t size;
	time64_t mtime;
	char etag[KS3FS_ETAG_LEN];
	struct ks3fs_meta meta;	/* from HEAD; empty for listings */
};

/* ---------- HTTP ---------- */

/* Retry budget for one logical operation (see http.c). */
struct ks3fs_retry {
	unsigned long start;
	unsigned int attempt;
};

void ks3fs_retry_init(struct ks3fs_retry *r);
bool ks3fs_retry(struct ks3fs_sb_info *sbi, struct ks3fs_retry *r, int err);
int ks3fs_retry_giveup(int err);
void ks3fs_retry_ok(struct ks3fs_sb_info *sbi);

struct ks3fs_param {
	const char *name;
	const char *value;	/* NULL for a valueless param such as "uploads" */
};

struct ks3fs_req {
	const char *method;
	const char *key;	/* object key, or NULL for bucket-level request */
	struct ks3fs_param *params;
	int nr_params;
	const char *copy_source;/* x-amz-copy-source value (signed) */
	const char *copy_range;	/* x-amz-copy-source-range (UploadPartCopy) */
	const char *copy_if_match; /* x-amz-copy-source-if-match */
	const struct ks3fs_meta *meta;	/* sent as x-amz-meta-* */
	bool meta_replace;	/* CopyObject: x-amz-metadata-directive: REPLACE */
	bool want_xattr;	/* response: keep x-amz-meta-xattr */
	const char *if_match;
	const char *range;	/* "bytes=a-b" */
	loff_t body_len;
	const char *payload_sha256; /* hex digest or UNSIGNED-PAYLOAD; NULL: empty body */
	const void *body;	/* small in-memory body for ks3fs_http_simple() */
};

struct ks3fs_resp {
	int status;
	loff_t content_length;	/* -1 if absent */
	bool chunked;
	bool close;		/* server will close the connection */
	bool head;		/* no body expected */
	char etag[KS3FS_ETAG_LEN];
	time64_t last_modified;
	loff_t range_start;	/* from Content-Range, -1 if absent */
	loff_t total_size;	/* from Content-Range, -1 if absent */
	struct ks3fs_meta meta;	/* x-amz-meta-* headers */
	bool want_xattr;	/* keep x-amz-meta-xattr (meta.xattr) */

	/* body decoder state */
	loff_t remaining;	/* bytes left in body (or current chunk) */
	bool body_done;
};

struct ks3fs_conn *ks3fs_http_start(struct ks3fs_sb_info *sbi,
				    struct ks3fs_req *req,
				    struct ks3fs_resp *resp,
				    int (*send_body)(struct ks3fs_conn *, void *),
				    void *body_arg, struct ks3fs_retry *r);
int ks3fs_http_send(struct ks3fs_conn *conn, const void *buf, size_t len);
ssize_t ks3fs_http_read_body(struct ks3fs_conn *conn, struct ks3fs_resp *resp,
			     void *buf, size_t len);
void ks3fs_http_finish(struct ks3fs_sb_info *sbi, struct ks3fs_conn *conn,
		       struct ks3fs_resp *resp);
void ks3fs_http_shutdown(struct ks3fs_sb_info *sbi);
int ks3fs_http_simple(struct ks3fs_sb_info *sbi, struct ks3fs_req *req,
		      struct ks3fs_resp *resp, char **body, size_t *body_len);
int ks3fs_http_request(struct ks3fs_sb_info *sbi, struct ks3fs_req *req,
		       struct ks3fs_resp *resp, char **body, size_t *body_len,
		       struct ks3fs_retry *r);
int ks3fs_status_to_errno(int status);

/* ---------- credentials (creds.c) ---------- */

struct ks3fs_creds {
	const char *ak, *sk, *token;
	char *buf;		/* copy of a keyring payload, if any */
};

int ks3fs_creds_lookup(struct ks3fs_sb_info *sbi, const char *name);
void ks3fs_creds_release(struct ks3fs_sb_info *sbi);
int ks3fs_creds_get(struct ks3fs_sb_info *sbi, struct ks3fs_creds *c);
void ks3fs_creds_put(struct ks3fs_creds *c);

static inline bool ks3fs_signed(struct ks3fs_sb_info *sbi)
{
	return sbi->access_key || sbi->creds_key;
}

/* ---------- TLS ---------- */

int ks3fs_tls_handshake(struct ks3fs_sb_info *sbi, struct socket *sock);
int ks3fs_tls_recv(struct socket *sock, void *buf, size_t len);

/* ---------- SigV4 ---------- */

int ks3fs_sigv4_init(void);
void ks3fs_sigv4_exit(void);
int ks3fs_uri_encode(char *dst, size_t dstlen, const char *src, bool keep_slash);
int ks3fs_build_request(struct ks3fs_sb_info *sbi, struct ks3fs_req *req,
			char **out, size_t *outlen);
int ks3fs_sha256_hex(const void *data, size_t len, char hex[65]);
struct ks3fs_sha256;
struct ks3fs_sha256 *ks3fs_sha256_begin(void);
int ks3fs_sha256_update(struct ks3fs_sha256 *h, const void *data, size_t len);
int ks3fs_sha256_end(struct ks3fs_sha256 *h, char hex[65]);

/* ---------- XML ---------- */

struct ks3fs_list_entry {
	char *name;		/* component name, no trailing '/' */
	bool is_dir;
	loff_t size;
	time64_t mtime;
	char etag[KS3FS_ETAG_LEN];
};

struct ks3fs_listing {
	struct ks3fs_list_entry *ents;
	int nr, cap;
	bool has_marker;	/* the prefix itself ("dir/") exists as an object */
};

typedef int (*ks3fs_list_cb)(void *arg, const char *key, size_t keylen,
			     bool is_prefix, loff_t size, time64_t mtime,
			     const char *etag);
int ks3fs_xml_parse_list(const char *xml, size_t len, ks3fs_list_cb cb,
			 void *arg, bool *truncated, char **next_token);
int ks3fs_xml_error_code(const char *xml, size_t len, char *code, size_t codelen);
int ks3fs_xml_get(const char *xml, size_t len, const char *tag, char *out,
		  size_t outlen);
time64_t ks3fs_parse_iso8601(const char *s, size_t len);
time64_t ks3fs_parse_http_date(const char *s);

/* ---------- S3 operations ---------- */

int ks3fs_s3_stat(struct ks3fs_sb_info *sbi, const char *key,
		  struct ks3fs_attr *attr);
int ks3fs_s3_head(struct ks3fs_sb_info *sbi, const char *key,
		  struct ks3fs_attr *attr);
int ks3fs_s3_head_xattr(struct ks3fs_sb_info *sbi, const char *key,
			struct ks3fs_attr *attr);

struct ks3fs_keyent {
	char *key;
	loff_t size;
};

struct ks3fs_keylist {
	struct ks3fs_keyent *ents;
	int nr, cap;
};

int ks3fs_s3_list_keys(struct ks3fs_sb_info *sbi, const char *prefix,
		       int max, struct ks3fs_keylist *kl);
void ks3fs_keylist_free(struct ks3fs_keylist *kl);

/* ---------- concurrency (par.c) ---------- */

int ks3fs_parallel(struct ks3fs_sb_info *sbi, int nr,
		   int (*fn)(void *ctx, int i), void *ctx,
		   void (*release)(void *ctx));

/* ---------- multipart (mpu.c) ---------- */

char *ks3fs_copy_source(struct ks3fs_sb_info *sbi, const char *key);
int ks3fs_mpu_create(struct ks3fs_sb_info *sbi, const char *key,
		     const struct ks3fs_meta *meta, char **upload_id);
int ks3fs_mpu_upload_part(struct ks3fs_sb_info *sbi, const char *key,
			  const char *upload_id, int partno, loff_t len,
			  int (*send_body)(struct ks3fs_conn *, void *),
			  void *arg, char *etag_out);
int ks3fs_mpu_copy_part(struct ks3fs_sb_info *sbi, const char *key,
			const char *upload_id, int partno, const char *src,
			loff_t start, loff_t end, const char *if_match,
			char *etag_out);
int ks3fs_mpu_complete(struct ks3fs_sb_info *sbi, const char *key,
		       const char *upload_id, char (*etags)[KS3FS_ETAG_LEN],
		       int nr, char *etag_out);
void ks3fs_mpu_abort(struct ks3fs_sb_info *sbi, const char *key,
		     const char *upload_id);
int ks3fs_mpu_copy_parts(struct ks3fs_sb_info *sbi, const char *key,
			 const char *upload_id, const char *src,
			 const char *if_match, u64 part, loff_t size,
			 const int *idx, int nr, char (*out)[KS3FS_ETAG_LEN]);
int ks3fs_s3_copy_large(struct ks3fs_sb_info *sbi, const char *src,
			const char *dst, loff_t size,
			const struct ks3fs_meta *meta, char *etag_out);
int ks3fs_s3_list_dir(struct ks3fs_sb_info *sbi, const char *dirkey,
		      struct ks3fs_listing *out);
void ks3fs_listing_free(struct ks3fs_listing *l);
int ks3fs_s3_probe(struct ks3fs_sb_info *sbi, const char *dirkey,
		   bool *has_marker);
int ks3fs_s3_dir_is_empty(struct ks3fs_sb_info *sbi, const char *dirkey);
int ks3fs_s3_put_buf(struct ks3fs_sb_info *sbi, const char *key,
		     const void *buf, size_t len,
		     const struct ks3fs_meta *meta, char *etag_out);
int ks3fs_s3_set_meta(struct ks3fs_sb_info *sbi, const char *key,
		      const struct ks3fs_meta *meta, char *etag_out);
int ks3fs_s3_delete(struct ks3fs_sb_info *sbi, const char *key);
int ks3fs_s3_copy(struct ks3fs_sb_info *sbi, const char *src,
		  const char *dst, char *etag_out);
char *ks3fs_full_key(struct ks3fs_sb_info *sbi, const char *rel);

/* ---------- inodes, dirs, files ---------- */

extern const struct inode_operations ks3fs_dir_iops;
extern const struct file_operations ks3fs_dir_fops;
extern const struct inode_operations ks3fs_file_iops;
extern const struct inode_operations ks3fs_symlink_iops;
extern const struct file_operations ks3fs_file_fops;
extern const struct address_space_operations ks3fs_aops;
extern const struct dentry_operations ks3fs_dops;

void ks3fs_dir_forget_snap(struct inode *dir);
u64 ks3fs_key_ino(const char *key);
struct inode *ks3fs_new_inode(struct super_block *sb, const char *key,
			      const struct ks3fs_attr *attr);
void ks3fs_apply_attr(struct inode *inode, const struct ks3fs_attr *attr);
void ks3fs_rehash_ino(struct inode *inode);
umode_t ks3fs_attr_type(struct ks3fs_sb_info *sbi, const struct ks3fs_attr *attr);
char *ks3fs_inode_key(struct inode *inode);
int ks3fs_inode_meta(struct inode *inode, struct ks3fs_meta *m);
int ks3fs_push_meta(struct inode *inode);
int ks3fs_for_each_cached(struct super_block *sb, const char *prefix,
			  int (*fn)(struct inode *, void *), void *arg);
void ks3fs_rekey_subtree(struct super_block *sb, const char *from,
			 const char *to);
void ks3fs_schedule_meta_writeback(struct inode *inode);
char *ks3fs_child_key(struct inode *dir, const struct qstr *name, bool is_dir);
int ks3fs_upload(struct inode *inode);
int ks3fs_upload_locked(struct inode *inode);
int ks3fs_orphan(struct inode *inode);
void ks3fs_orphan_delete(struct inode *inode);
void ks3fs_mpu_discard(struct inode *inode);
void ks3fs_mpu_forget(struct inode *inode);
u64 ks3fs_max_object(struct ks3fs_sb_info *sbi);
int ks3fs_setattr(struct mnt_idmap *idmap, struct dentry *dentry,
		  struct iattr *attr);
int ks3fs_getattr(struct mnt_idmap *idmap, const struct path *path,
		  struct kstat *stat, u32 mask, unsigned int flags);

/* xattr.c */
extern const struct xattr_handler * const ks3fs_xattr_handlers[];
ssize_t ks3fs_listxattr(struct dentry *dentry, char *buf, size_t size);
int ks3fs_xattr_load(struct inode *inode);
void ks3fs_xattr_update(struct inode *inode, char *hdr);
int ks3fs_xattr_header(struct inode *inode, char **out);
struct posix_acl *ks3fs_get_acl(struct inode *inode, int type, bool rcu);
int ks3fs_set_acl(struct mnt_idmap *idmap, struct dentry *dentry,
		  struct posix_acl *acl, int type);
int ks3fs_acl_header(struct posix_acl *default_acl, struct posix_acl *acl,
		     char **hdr);

#endif /* _KS3FS_H */
