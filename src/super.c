// SPDX-License-Identifier: GPL-2.0
/*
 * ks3fs superblock, mount options and module glue.
 *
 *   mount -t ks3fs -o addr=IP[,port=N][,host=NAME][,region=R]
 *                     [,access_key=AK,secret_key=SK[,session_token=T]]
 *                     [,vhost][,uid=,gid=,file_mode=,dir_mode=][,ttl=S]
 *                     [,timeout=S][,tls][,hard|soft]
 *                     [,retry_timeout=S]  bucket[/prefix]  /mnt
 */
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/statfs.h>
#include <linux/seq_file.h>
#include <linux/fs_context.h>
#include <linux/fs_parser.h>
#include <linux/inet.h>
#include <linux/backing-dev.h>
#include <linux/pagemap.h>
#include <linux/writeback.h>
#include <linux/xxhash.h>
#include <linux/nsproxy.h>
#include <net/net_namespace.h>

#include "ks3fs.h"

static struct kmem_cache *ks3fs_inode_cachep;

/* ---------- inodes ---------- */

static struct inode *ks3fs_alloc_inode(struct super_block *sb)
{
	struct ks3fs_inode *ki;

	ki = alloc_inode_sb(sb, ks3fs_inode_cachep, GFP_KERNEL);
	if (!ki)
		return NULL;
	spin_lock_init(&ki->lock);
	ki->key = NULL;
	ki->etag[0] = '\0';
	ki->flags = 0;
	ki->attr_time = 0;
	ki->remote_size = 0;
	ki->mpu = NULL;
	ki->stream_next = 0;
	ki->snap = NULL;
	ki->xattr = NULL;
	atomic_set(&ki->opens, 0);
	return &ki->vfs_inode;
}

static void ks3fs_free_inode(struct inode *inode)
{
	struct ks3fs_inode *ki = KS3_I(inode);

	kfree(ki->key);
	kfree(ki->xattr);
	kmem_cache_free(ks3fs_inode_cachep, ki);
}

static void ks3fs_evict_inode(struct inode *inode)
{
	if (test_bit(KS3_I_DIRTY, &KS3_I(inode)->flags) && inode->i_nlink)
		pr_warn("ks3fs: discarding unsent data for '%s'\n",
			KS3_I(inode)->key);
	ks3fs_mpu_forget(inode);
	ks3fs_orphan_delete(inode);	/* normally done by the last release */
	if (S_ISDIR(inode->i_mode))
		ks3fs_dir_forget_snap(inode);
	truncate_inode_pages_final(&inode->i_data);
	clear_inode(inode);
}

static void ks3fs_inode_init_once(void *p)
{
	struct ks3fs_inode *ki = p;

	inode_init_once(&ki->vfs_inode);
}

char *ks3fs_inode_key(struct inode *inode)
{
	struct ks3fs_inode *ki = KS3_I(inode);
	char *k;

	spin_lock(&ki->lock);
	k = kstrdup(ki->key, GFP_ATOMIC);
	spin_unlock(&ki->lock);
	return k;
}

/* The file type an entry has: symlinks are only known from metadata. */
umode_t ks3fs_attr_type(struct ks3fs_sb_info *sbi, const struct ks3fs_attr *attr)
{
	if (attr->is_dir)
		return S_IFDIR;
	if (sbi->meta && attr->meta.has_mode && S_ISLNK(attr->meta.mode))
		return S_IFLNK;
	return S_IFREG;
}

/*
 * Refresh an inode from remote attributes.  Local changes that have not
 * reached the store yet (data or metadata) win over what the store says.
 */
void ks3fs_apply_attr(struct inode *inode, const struct ks3fs_attr *attr)
{
	struct ks3fs_sb_info *sbi = KS3_SB(inode->i_sb);
	struct ks3fs_inode *ki = KS3_I(inode);
	const struct ks3fs_meta *m = &attr->meta;
	struct timespec64 ts = { .tv_sec = attr->mtime };

	if (!attr->mtime)
		ts = current_time(inode);
	if (!S_ISDIR(inode->i_mode) && !test_bit(KS3_I_DIRTY, &ki->flags)) {
		i_size_write(inode, attr->size);
		inode->i_blocks = DIV_ROUND_UP_ULL(attr->size, 512);
		ki->remote_size = attr->size;
	}
	if (!test_bit(KS3_I_META_DIRTY, &ki->flags) &&
	    !test_bit(KS3_I_DIRTY, &ki->flags)) {
		if (sbi->meta && m->has_mtime)
			ts = m->mtime;
		inode_set_mtime_to_ts(inode, ts);
		inode_set_ctime_to_ts(inode, sbi->meta && m->has_ctime ?
					     m->ctime : ts);
		inode_set_atime_to_ts(inode, ts);
		if (sbi->meta && m->has_mode)
			inode->i_mode = (inode->i_mode & S_IFMT) | (m->mode & 07777);
		if (sbi->meta && m->has_uid)
			inode->i_uid = make_kuid(&init_user_ns, m->uid);
		if (sbi->meta && m->has_gid)
			inode->i_gid = make_kgid(&init_user_ns, m->gid);
		if (sbi->meta && m->has_xattr)
			ks3fs_xattr_update(inode, m->xattr ?
					   kstrdup(m->xattr, GFP_KERNEL) : NULL);
	}
	spin_lock(&ki->lock);
	strscpy(ki->etag, attr->etag, sizeof(ki->etag));
	spin_unlock(&ki->lock);
	if (attr->is_dir ? attr->has_marker : true)
		set_bit(KS3_I_REMOTE, &ki->flags);
	ki->attr_time = jiffies;
}

/*
 * The metadata to store for an inode (everything the store can hold);
 * release it with ks3fs_meta_release().  Rewriting an object replaces all
 * its metadata, so xattrs not seen yet are fetched first.
 */
int ks3fs_inode_meta(struct inode *inode, struct ks3fs_meta *m)
{
	ks3fs_meta_clear(m);
	if (!KS3_SB(inode->i_sb)->meta)
		return 0;
	m->has_mode = m->has_uid = m->has_gid = m->has_mtime = true;
	m->has_ctime = true;
	m->mode = inode->i_mode;
	m->uid = from_kuid_munged(&init_user_ns, inode->i_uid);
	m->gid = from_kgid_munged(&init_user_ns, inode->i_gid);
	m->mtime = inode_get_mtime(inode);
	m->ctime = inode_get_ctime(inode);
	m->has_xattr = true;
	return ks3fs_xattr_header(inode, &m->xattr);
}

/*
 * Inode numbers are a hash of the object key, so readdir can report the
 * same d_ino that stat() will (without a HEAD per entry) and numbers are
 * stable across mounts.
 */
u64 ks3fs_key_ino(const char *key)
{
	return xxh64(key, strlen(key), 0) ?: 1;
}

/*
 * After a rename moved @inode to another key: take that key's number, so
 * that a new object created at the old key (with the old key's number)
 * is not taken for the same file, and the number matches what a fresh
 * mount would give it.
 */
void ks3fs_rehash_ino(struct inode *inode)
{
	char *key = ks3fs_inode_key(inode);

	if (!key)
		return;		/* keep the old number: unlikely to collide */
	remove_inode_hash(inode);
	inode->i_ino = ks3fs_key_ino(key);
	insert_inode_hash(inode);
	kfree(key);
}

struct inode *ks3fs_new_inode(struct super_block *sb, const char *key,
			      const struct ks3fs_attr *attr)
{
	struct ks3fs_sb_info *sbi = KS3_SB(sb);
	struct inode *inode;
	umode_t type = ks3fs_attr_type(sbi, attr);

	inode = new_inode(sb);
	if (!inode)
		return ERR_PTR(-ENOMEM);
	KS3_I(inode)->key = kstrdup(key, GFP_KERNEL);
	if (!KS3_I(inode)->key) {
		iput(inode);
		return ERR_PTR(-ENOMEM);
	}
	inode->i_ino = ks3fs_key_ino(key);
	inode->i_uid = sbi->uid;
	inode->i_gid = sbi->gid;
	set_nlink(inode, 1);	/* dirs too: unknown subdir count, see find(1) */
	switch (type) {
	case S_IFDIR:
		inode->i_mode = S_IFDIR | sbi->dir_mode;
		inode->i_op = &ks3fs_dir_iops;
		inode->i_fop = &ks3fs_dir_fops;
		break;
	case S_IFLNK:
		/* the object body is the target, read through the page cache */
		inode->i_mode = S_IFLNK | 0777;
		inode->i_op = &ks3fs_symlink_iops;
		inode->i_mapping->a_ops = &ks3fs_aops;
		inode_nohighmem(inode);
		break;
	default:
		inode->i_mode = S_IFREG | sbi->file_mode;
		inode->i_op = &ks3fs_file_iops;
		inode->i_fop = &ks3fs_file_fops;
		inode->i_mapping->a_ops = &ks3fs_aops;
		break;
	}
	ks3fs_apply_attr(inode, attr);
	/* only hashed inodes can be marked dirty and reach ->write_inode */
	insert_inode_hash(inode);
	return inode;
}

/*
 * Store pending metadata changes (chmod/chown/utimes).  Objects get their
 * metadata replaced in place; a directory without a marker object gets one.
 */
int ks3fs_push_meta(struct inode *inode)
{
	struct ks3fs_sb_info *sbi = KS3_SB(inode->i_sb);
	struct ks3fs_inode *ki = KS3_I(inode);
	struct ks3fs_meta m;
	char etag[KS3FS_ETAG_LEN] = "";
	char *key;
	int err;

	/*
	 * With data waiting to be uploaded the upload carries the metadata
	 * (under i_rwsem); a separate rewrite now would change the object's
	 * ETag under an open multipart upload's server-side part copies.
	 */
	if (test_bit(KS3_I_DIRTY, &ki->flags))
		return 0;
	if (!test_and_clear_bit(KS3_I_META_DIRTY, &ki->flags))
		return 0;
	if (!inode->i_nlink)
		return 0;
	key = ks3fs_inode_key(inode);
	if (!key) {
		err = -ENOMEM;
		goto fail;
	}
	if (!*key) {		/* the bucket root cannot carry metadata */
		kfree(key);
		return 0;
	}
	err = ks3fs_inode_meta(inode, &m);
	if (err) {
		kfree(key);
		goto fail;
	}
	if (S_ISDIR(inode->i_mode) && !test_bit(KS3_I_REMOTE, &ki->flags))
		err = ks3fs_s3_put_buf(sbi, key, NULL, 0, &m, NULL);
	else if (i_size_read(inode) > KS3FS_MAX_PUT)	/* too big for CopyObject */
		err = ks3fs_s3_copy_large(sbi, key, key, i_size_read(inode),
					  &m, etag);
	else
		err = ks3fs_s3_set_meta(sbi, key, &m, etag);
	ks3fs_meta_release(&m);
	kfree(key);
	if (err)
		goto fail;
	set_bit(KS3_I_REMOTE, &ki->flags);
	if (etag[0]) {
		spin_lock(&ki->lock);
		strscpy(ki->etag, etag, sizeof(ki->etag));
		spin_unlock(&ki->lock);
	}
	return 0;
fail:
	set_bit(KS3_I_META_DIRTY, &ki->flags);
	return err;
}

/*
 * Without BDI_CAP_WRITEBACK the flusher is never woken for us, so pending
 * metadata (->write_inode) is pushed by this per-mount work item instead.
 */
static void ks3fs_meta_work(struct work_struct *work)
{
	struct ks3fs_sb_info *sbi =
		container_of(to_delayed_work(work), struct ks3fs_sb_info, meta_work);

	try_to_writeback_inodes_sb(sbi->sb, WB_REASON_PERIODIC);
}

void ks3fs_schedule_meta_writeback(struct inode *inode)
{
	mark_inode_dirty(inode);
	queue_delayed_work(system_unbound_wq, &KS3_SB(inode->i_sb)->meta_work,
			   KS3FS_META_DELAY);
}

static int ks3fs_write_inode(struct inode *inode, struct writeback_control *wbc)
{
	int err = ks3fs_push_meta(inode);

	if (err) {
		pr_warn_ratelimited("ks3fs: storing metadata of '%s' failed: %d\n",
				    KS3_I(inode)->key, err);
		ks3fs_schedule_meta_writeback(inode);	/* try again later */
	}
	return err;
}

/* Keep unused inodes with unsaved metadata until writeback has stored it. */
static int ks3fs_drop_inode(struct inode *inode)
{
	return !inode->i_nlink ||
	       !test_bit(KS3_I_META_DIRTY, &KS3_I(inode)->flags);
}

/*
 * Call @fn for every cached inode whose key starts with @prefix (without
 * holding any spinlock during the call).  Stops at the first error.
 */
int ks3fs_for_each_cached(struct super_block *sb, const char *prefix,
			  int (*fn)(struct inode *, void *), void *arg)
{
	struct inode *inode, *toput = NULL;
	size_t pl = strlen(prefix);
	int err = 0;

	spin_lock(&sb->s_inode_list_lock);
	list_for_each_entry(inode, &sb->s_inodes, i_sb_list) {
		struct ks3fs_inode *ki = KS3_I(inode);
		bool match;

		spin_lock(&ki->lock);
		match = ki->key && !strncmp(ki->key, prefix, pl);
		spin_unlock(&ki->lock);
		/* igrab() skips inodes that are being freed */
		if (!match || !igrab(inode))
			continue;
		spin_unlock(&sb->s_inode_list_lock);

		iput(toput);
		toput = inode;
		err = fn(inode, arg);
		cond_resched();

		spin_lock(&sb->s_inode_list_lock);
		if (err)
			break;
	}
	spin_unlock(&sb->s_inode_list_lock);
	iput(toput);
	return err;
}

struct rekey {
	const char *from, *to;
};

static int rekey_one(struct inode *inode, void *arg)
{
	struct ks3fs_inode *ki = KS3_I(inode);
	struct rekey *rk = arg;
	size_t fl = strlen(rk->from);
	char *old = ks3fs_inode_key(inode), *new;

	if (!old)
		return -ENOMEM;
	new = kasprintf(GFP_KERNEL, "%s%s", rk->to, old + fl);
	if (!new) {
		kfree(old);
		return -ENOMEM;
	}
	spin_lock(&ki->lock);
	if (ki->key && !strcmp(ki->key, old))
		swap(ki->key, new);
	spin_unlock(&ki->lock);
	kfree(old);
	kfree(new);
	ks3fs_rehash_ino(inode);
	return 0;
}

/* After a directory rename: every cached inode below moves with it. */
void ks3fs_rekey_subtree(struct super_block *sb, const char *from,
			 const char *to)
{
	struct rekey rk = { from, to };

	if (ks3fs_for_each_cached(sb, from, rekey_one, &rk))
		pr_warn("ks3fs: could not rename every cached entry below '%s'\n",
			from);
}

/* ---------- super operations ---------- */

static int ks3fs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	buf->f_type = KS3FS_MAGIC;
	buf->f_bsize = PAGE_SIZE;
	buf->f_frsize = PAGE_SIZE;
	buf->f_blocks = 1ULL << 40;
	buf->f_bfree = 1ULL << 40;
	buf->f_bavail = 1ULL << 40;
	buf->f_files = 1ULL << 32;
	buf->f_ffree = 1ULL << 32;
	buf->f_namelen = NAME_MAX;
	return 0;
}

static int ks3fs_show_options(struct seq_file *m, struct dentry *root)
{
	struct ks3fs_sb_info *sbi = KS3_SB(root->d_sb);

	if (sbi->addr.ss_family == AF_INET6) {
		struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)&sbi->addr;

		seq_printf(m, ",addr=%pI6c,port=%u", &s6->sin6_addr,
			   ntohs(s6->sin6_port));
	} else {
		struct sockaddr_in *s4 = (struct sockaddr_in *)&sbi->addr;

		seq_printf(m, ",addr=%pI4,port=%u", &s4->sin_addr,
			   ntohs(s4->sin_port));
	}
	seq_show_option(m, "host", sbi->host);
	seq_show_option(m, "region", sbi->region);
	if (sbi->vhost)
		seq_puts(m, ",vhost");
	if (sbi->tls)
		seq_puts(m, ",tls");
	if (!sbi->meta)
		seq_puts(m, ",nometa");
	seq_printf(m, ",part_size=%llu,parallel=%d", sbi->part_size >> 20,
		   sbi->parallel);
	if (sbi->hard)
		seq_puts(m, ",hard");
	else
		seq_printf(m, ",soft,retry_timeout=%u",
			   jiffies_to_msecs(sbi->retry_timeout) / 1000);
	if (sbi->access_key)
		seq_show_option(m, "access_key", sbi->access_key);
	if (sbi->creds_name)
		seq_show_option(m, "creds_key", sbi->creds_name);
	/* secret_key and session_token are deliberately never shown */
	seq_printf(m, ",uid=%u,gid=%u,file_mode=%o,dir_mode=%o,ttl=%u",
		   from_kuid_munged(&init_user_ns, sbi->uid),
		   from_kgid_munged(&init_user_ns, sbi->gid),
		   sbi->file_mode, sbi->dir_mode,
		   jiffies_to_msecs(sbi->ttl) / 1000);
	return 0;
}

static const struct super_operations ks3fs_sops = {
	.alloc_inode	= ks3fs_alloc_inode,
	.free_inode	= ks3fs_free_inode,
	.evict_inode	= ks3fs_evict_inode,
	.statfs		= ks3fs_statfs,
	.show_options	= ks3fs_show_options,
	.drop_inode	= ks3fs_drop_inode,
	.write_inode	= ks3fs_write_inode,	/* nothing to keep uncached */
};

static void ks3fs_free_sbi(struct ks3fs_sb_info *sbi)
{
	if (!sbi)
		return;
	ks3fs_http_shutdown(sbi);
	if (sbi->net)
		put_net(sbi->net);
	kfree(sbi->host);
	kfree(sbi->tls_peername);
	kfree(sbi->bucket);
	kfree(sbi->prefix);
	kfree(sbi->region);
	kfree(sbi->access_key);
	ks3fs_creds_release(sbi);
	kfree(sbi->creds_name);
	kfree_sensitive(sbi->secret_key);
	kfree_sensitive(sbi->session_token);
	kfree(sbi);
}

/* ---------- mount ---------- */

enum {
	Opt_addr, Opt_port, Opt_host, Opt_region, Opt_access_key,
	Opt_secret_key, Opt_session_token, Opt_vhost, Opt_uid, Opt_gid,
	Opt_file_mode, Opt_dir_mode, Opt_ttl, Opt_timeout, Opt_tls,
	Opt_hard, Opt_soft, Opt_retry_timeout, Opt_nometa, Opt_part_size,
	Opt_parallel, Opt_creds_key,
};

static const struct fs_parameter_spec ks3fs_param_specs[] = {
	fsparam_string("addr",		Opt_addr),
	fsparam_u32("port",		Opt_port),
	fsparam_string("host",		Opt_host),
	fsparam_string("region",	Opt_region),
	fsparam_string("access_key",	Opt_access_key),
	fsparam_string("secret_key",	Opt_secret_key),
	fsparam_string("session_token",	Opt_session_token),
	fsparam_flag("vhost",		Opt_vhost),
	fsparam_u32("uid",		Opt_uid),
	fsparam_u32("gid",		Opt_gid),
	fsparam_u32oct("file_mode",	Opt_file_mode),
	fsparam_u32oct("dir_mode",	Opt_dir_mode),
	fsparam_u32("ttl",		Opt_ttl),
	fsparam_u32("timeout",		Opt_timeout),
	fsparam_flag("tls",		Opt_tls),
	fsparam_flag("hard",		Opt_hard),
	fsparam_flag("soft",		Opt_soft),
	fsparam_u32("retry_timeout",	Opt_retry_timeout),
	fsparam_flag("nometa",		Opt_nometa),
	fsparam_u32("part_size",	Opt_part_size),
	fsparam_u32("parallel",		Opt_parallel),
	fsparam_string("creds_key",	Opt_creds_key),
	{}
};

struct ks3fs_fs_context {
	char *addr;
	unsigned int port;
	char *host;
	char *region;
	char *access_key;
	char *secret_key;
	char *session_token;
	char *creds_key;
	bool vhost;
	kuid_t uid;
	kgid_t gid;
	umode_t file_mode, dir_mode;
	unsigned int ttl, timeout, retry_timeout, part_size, parallel;
	bool tls, hard, nometa;
};

static int ks3fs_parse_param(struct fs_context *fc, struct fs_parameter *param)
{
	struct ks3fs_fs_context *ctx = fc->fs_private;
	struct fs_parse_result result;
	char **strp = NULL;
	int opt;

	opt = fs_parse(fc, ks3fs_param_specs, param, &result);
	if (opt < 0)
		return opt;

	switch (opt) {
	case Opt_addr:		strp = &ctx->addr; break;
	case Opt_host:		strp = &ctx->host; break;
	case Opt_region:	strp = &ctx->region; break;
	case Opt_access_key:	strp = &ctx->access_key; break;
	case Opt_secret_key:	strp = &ctx->secret_key; break;
	case Opt_session_token:	strp = &ctx->session_token; break;
	case Opt_creds_key:	strp = &ctx->creds_key; break;
	case Opt_port:
		if (!result.uint_32 || result.uint_32 > 65535)
			return invalfc(fc, "invalid port");
		ctx->port = result.uint_32;
		break;
	case Opt_vhost:
		ctx->vhost = true;
		break;
	case Opt_uid:
		ctx->uid = make_kuid(current_user_ns(), result.uint_32);
		if (!uid_valid(ctx->uid))
			return invalfc(fc, "invalid uid");
		break;
	case Opt_gid:
		ctx->gid = make_kgid(current_user_ns(), result.uint_32);
		if (!gid_valid(ctx->gid))
			return invalfc(fc, "invalid gid");
		break;
	case Opt_file_mode:
		ctx->file_mode = result.uint_32 & 07777;
		break;
	case Opt_dir_mode:
		ctx->dir_mode = result.uint_32 & 07777;
		break;
	case Opt_ttl:
		ctx->ttl = result.uint_32;
		break;
	case Opt_timeout:
		if (!result.uint_32)
			return invalfc(fc, "timeout must be > 0");
		ctx->timeout = result.uint_32;
		break;
	case Opt_tls:
		ctx->tls = true;
		break;
	case Opt_hard:
		ctx->hard = true;
		break;
	case Opt_soft:
		ctx->hard = false;
		break;
	case Opt_retry_timeout:
		ctx->retry_timeout = result.uint_32;
		break;
	case Opt_nometa:
		ctx->nometa = true;
		break;
	case Opt_parallel:
		if (!result.uint_32 || result.uint_32 > 128)
			return invalfc(fc, "parallel must be 1..128");
		ctx->parallel = result.uint_32;
		break;
	case Opt_part_size:
		/* MiB; S3 wants >= 5 MiB for all but the last part, <= 5 GiB */
		if (result.uint_32 < 5 || result.uint_32 > 5120)
			return invalfc(fc, "part_size must be 5..5120 (MiB)");
		ctx->part_size = result.uint_32;
		break;
	}
	if (strp) {
		if (opt == Opt_secret_key || opt == Opt_session_token)
			kfree_sensitive(*strp);
		else
			kfree(*strp);
		*strp = param->string;
		param->string = NULL;
	}
	return 0;
}

static int parse_addr(struct ks3fs_sb_info *sbi, const char *addr, u16 port)
{
	struct sockaddr_in *s4 = (struct sockaddr_in *)&sbi->addr;
	struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)&sbi->addr;

	memset(&sbi->addr, 0, sizeof(sbi->addr));
	if (in4_pton(addr, -1, (u8 *)&s4->sin_addr, -1, NULL)) {
		s4->sin_family = AF_INET;
		s4->sin_port = htons(port);
		sbi->addrlen = sizeof(*s4);
		return 0;
	}
	if (in6_pton(addr, -1, (u8 *)&s6->sin6_addr, -1, NULL)) {
		s6->sin6_family = AF_INET6;
		s6->sin6_port = htons(port);
		sbi->addrlen = sizeof(*s6);
		return 0;
	}
	return -EINVAL;
}

static int ks3fs_fill_super(struct super_block *sb, struct fs_context *fc)
{
	struct ks3fs_fs_context *ctx = fc->fs_private;
	struct ks3fs_sb_info *sbi;
	struct ks3fs_attr root_attr = { .is_dir = true };
	bool has_marker = false;
	struct inode *root;
	const char *slash;
	int err;

	if (!fc->source || !*fc->source)
		return invalfc(fc, "source must be bucket[/prefix]");
	if (!ctx->addr)
		return invalfc(fc, "addr= is required (IPv4/IPv6 literal)");
	if (!!ctx->access_key != !!ctx->secret_key)
		return invalfc(fc, "access_key and secret_key go together");
	if (ctx->creds_key && ctx->access_key)
		return invalfc(fc, "creds_key replaces access_key/secret_key");

	sbi = kzalloc(sizeof(*sbi), GFP_KERNEL);
	if (!sbi)
		return -ENOMEM;
	sb->s_fs_info = sbi;
	spin_lock_init(&sbi->conn_lock);
	sbi->sb = sb;
	INIT_DELAYED_WORK(&sbi->meta_work, ks3fs_meta_work);
	INIT_LIST_HEAD(&sbi->idle_conns);

	if (!ctx->port)
		ctx->port = ctx->tls ? 443 : 80;
	err = parse_addr(sbi, ctx->addr, ctx->port);
	if (err)
		return invalfc(fc, "addr must be an IPv4 or IPv6 literal");
	sbi->net = get_net(current->nsproxy->net_ns);

	slash = strchr(fc->source, '/');
	if (slash) {
		const char *p = slash + 1;
		size_t plen = strlen(p);

		while (plen && p[plen - 1] == '/')
			plen--;
		sbi->bucket = kmemdup_nul(fc->source, slash - fc->source,
					  GFP_KERNEL);
		sbi->prefix = plen ? kasprintf(GFP_KERNEL, "%.*s/", (int)plen, p)
				   : kstrdup("", GFP_KERNEL);
	} else {
		sbi->bucket = kstrdup(fc->source, GFP_KERNEL);
		sbi->prefix = kstrdup("", GFP_KERNEL);
	}
	if (!sbi->bucket || !sbi->prefix)
		return -ENOMEM;
	if (!*sbi->bucket)
		return invalfc(fc, "empty bucket name");

	sbi->vhost = ctx->vhost;
	if (ctx->host)
		sbi->host = sbi->vhost ?
			kasprintf(GFP_KERNEL, "%s.%s", sbi->bucket, ctx->host) :
			kstrdup(ctx->host, GFP_KERNEL);
	else if (ctx->port != (ctx->tls ? 443 : 80))
		sbi->host = kasprintf(GFP_KERNEL,
				      sbi->addr.ss_family == AF_INET6 ?
				      "[%s]:%u" : "%s:%u", ctx->addr, ctx->port);
	else
		sbi->host = kstrdup(ctx->addr, GFP_KERNEL);
	sbi->region = kstrdup(ctx->region ?: "us-east-1", GFP_KERNEL);
	if (!sbi->host || !sbi->region)
		return -ENOMEM;
	if (ctx->creds_key) {
		err = ks3fs_creds_lookup(sbi, ctx->creds_key);
		if (err)
			return invalfc(fc, "no usable logon key \"ks3fs:%s\" (%d)",
				       ctx->creds_key, err) ?: err;
		sbi->creds_name = ctx->creds_key;
		ctx->creds_key = NULL;
	}
	sbi->access_key = ctx->access_key;
	sbi->secret_key = ctx->secret_key;
	sbi->session_token = ctx->session_token;
	ctx->access_key = ctx->secret_key = ctx->session_token = NULL;

	sbi->uid = ctx->uid;
	sbi->gid = ctx->gid;
	sbi->file_mode = ctx->file_mode;
	sbi->dir_mode = ctx->dir_mode;
	sbi->ttl = secs_to_jiffies_compat(ctx->ttl);
	sbi->timeout = secs_to_jiffies_compat(ctx->timeout);
	sbi->retry_timeout = secs_to_jiffies_compat(ctx->retry_timeout);
	sbi->hard = ctx->hard;
	sbi->meta = !ctx->nometa;
	sbi->part_size = (u64)ctx->part_size << 20;
	sbi->parallel = ctx->parallel;
	sbi->tls = ctx->tls;
	if (sbi->tls) {
		/* the certificate must match the host name, without any port */
		const char *h = sbi->host, *end;

		if (*h == '[') {
			h++;
			end = strchr(h, ']');
		} else {
			end = strrchr(h, ':');
			if (end && strchr(h, ':') != end)
				end = NULL;	/* bare IPv6 literal */
		}
		sbi->tls_peername = end ? kmemdup_nul(h, end - h, GFP_KERNEL)
					: kstrdup(h, GFP_KERNEL);
		if (!sbi->tls_peername)
			return -ENOMEM;
		/* tlshd verifies certificates against DNS names only */
		if (in4_pton(sbi->tls_peername, -1, (u8 *)&(__be32){0}, -1, NULL) ||
		    strchr(sbi->tls_peername, ':'))
			return invalfc(fc, "tls needs host=<DNS name> matching the server certificate");
	}

	sb->s_magic = KS3FS_MAGIC;
	sb->s_op = &ks3fs_sops;
	sb->s_xattr = ks3fs_xattr_handlers;
	/* ACLs need somewhere to keep the mode they are checked against */
	if (sbi->meta)
		sb->s_flags |= SB_POSIXACL;
	ks3_set_d_op(sb, &ks3fs_dops);
	sb->s_maxbytes = MAX_LFS_FILESIZE;
	sb->s_blocksize = PAGE_SIZE;
	sb->s_blocksize_bits = PAGE_SHIFT;
	sb->s_time_gran = NSEC_PER_SEC;
	sb->s_flags |= SB_NOSEC;	/* no suid bits to strip */

	err = super_setup_bdi(sb);
	if (err)
		return err;
	/*
	 * Objects cannot be written back a page at a time: dirty data stays
	 * pinned until the whole object is uploaded on close/fsync.  Like
	 * tmpfs, opt out of writeback dirty accounting, which the page cache
	 * would otherwise expect for every dirty folio (and undo on truncate).
	 */
	sb->s_bdi->capabilities &= ~BDI_CAP_WRITEBACK;
#ifdef BDI_CAP_WRITEBACK_ACCT	/* folded into BDI_CAP_WRITEBACK in newer kernels */
	sb->s_bdi->capabilities &= ~BDI_CAP_WRITEBACK_ACCT;
#endif
	sb->s_bdi->ra_pages = (4 << 20) >> PAGE_SHIFT;
	sb->s_bdi->io_pages = sb->s_bdi->ra_pages;

	/* Probe the bucket so a bad endpoint/credential fails the mount. */
	err = ks3fs_s3_probe(sbi, sbi->prefix, &has_marker);
	if (err) {
		errorfc(fc, "cannot list %s/%s: %d", sbi->bucket, sbi->prefix,
			err);
		return err;
	}
	root_attr.has_marker = has_marker;

	root = ks3fs_new_inode(sb, sbi->prefix, &root_attr);
	if (IS_ERR(root))
		return PTR_ERR(root);
	sb->s_root = d_make_root(root);
	if (!sb->s_root)
		return -ENOMEM;
	return 0;
}

static int ks3fs_get_tree(struct fs_context *fc)
{
	return get_tree_nodev(fc, ks3fs_fill_super);
}

static void ks3fs_free_fc(struct fs_context *fc)
{
	struct ks3fs_fs_context *ctx = fc->fs_private;

	if (!ctx)
		return;
	kfree(ctx->addr);
	kfree(ctx->host);
	kfree(ctx->region);
	kfree(ctx->creds_key);
	kfree(ctx->access_key);
	kfree_sensitive(ctx->secret_key);
	kfree_sensitive(ctx->session_token);
	kfree(ctx);
}

static int ks3fs_reconfigure(struct fs_context *fc)
{
	/* only generic flags such as ro/rw may change on remount */
	sync_filesystem(fc->root->d_sb);
	return 0;
}

static const struct fs_context_operations ks3fs_context_ops = {
	.free		= ks3fs_free_fc,
	.parse_param	= ks3fs_parse_param,
	.get_tree	= ks3fs_get_tree,
	.reconfigure	= ks3fs_reconfigure,
};

static int ks3fs_init_fs_context(struct fs_context *fc)
{
	struct ks3fs_fs_context *ctx;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	ctx->port = 0;		/* 80, or 443 with tls */
	ctx->retry_timeout = 60;
	ctx->part_size = 16;
	ctx->parallel = 16;
	ctx->uid = current_fsuid();
	ctx->gid = current_fsgid();
	ctx->file_mode = 0644;
	ctx->dir_mode = 0755;
	ctx->ttl = 1;
	ctx->timeout = 30;
	fc->fs_private = ctx;
	fc->ops = &ks3fs_context_ops;
	return 0;
}

static void ks3fs_kill_sb(struct super_block *sb)
{
	struct ks3fs_sb_info *sbi = KS3_SB(sb);

	/* unmount's sync_filesystem() stores whatever metadata is pending */
	if (sbi)
		cancel_delayed_work_sync(&sbi->meta_work);
	kill_anon_super(sb);
	ks3fs_free_sbi(sbi);
}

static struct file_system_type ks3fs_fs_type = {
	.owner			= THIS_MODULE,
	.name			= "ks3fs",
	.init_fs_context	= ks3fs_init_fs_context,
	.parameters		= ks3fs_param_specs,
	.kill_sb		= ks3fs_kill_sb,
};
MODULE_ALIAS_FS("ks3fs");

static int __init ks3fs_init(void)
{
	int err;

	err = ks3fs_sigv4_init();
	if (err)
		return err;
	ks3fs_inode_cachep = kmem_cache_create("ks3fs_inode_cache",
					       sizeof(struct ks3fs_inode), 0,
					       SLAB_RECLAIM_ACCOUNT | SLAB_ACCOUNT,
					       ks3fs_inode_init_once);
	if (!ks3fs_inode_cachep) {
		ks3fs_sigv4_exit();
		return -ENOMEM;
	}
	err = register_filesystem(&ks3fs_fs_type);
	if (err) {
		kmem_cache_destroy(ks3fs_inode_cachep);
		ks3fs_sigv4_exit();
	}
	return err;
}

static void __exit ks3fs_exit(void)
{
	unregister_filesystem(&ks3fs_fs_type);
	rcu_barrier();
	kmem_cache_destroy(ks3fs_inode_cachep);
	ks3fs_sigv4_exit();
}

module_init(ks3fs_init);
module_exit(ks3fs_exit);
MODULE_DESCRIPTION("In-kernel S3 object store filesystem");
MODULE_LICENSE("GPL");
MODULE_VERSION("0.3.0");
