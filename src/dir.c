// SPDX-License-Identifier: GPL-2.0
/*
 * ks3fs namespace operations.
 *
 * Directories are key prefixes ending in '/'.  A directory exists if an
 * explicit "dir/" marker object exists or any key lives under the prefix.
 * mkdir creates a marker; rmdir removes it.  Renaming directories is not
 * atomic in S3, so it returns -EXDEV and mv(1) falls back to copy+delete.
 */
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/pagemap.h>
#include <linux/namei.h>
#include <linux/dcache.h>
#include <linux/wait.h>
#include <linux/kref.h>

#include "ks3fs.h"

char *ks3fs_child_key(struct inode *dir, const struct qstr *name, bool is_dir)
{
	struct ks3fs_inode *di = KS3_I(dir);
	char *key;

	if (name->len > NAME_MAX)
		return ERR_PTR(-ENAMETOOLONG);
	spin_lock(&di->lock);
	key = kasprintf(GFP_ATOMIC, "%s%.*s%s", di->key, name->len, name->name,
			is_dir ? "/" : "");
	spin_unlock(&di->lock);
	if (!key)
		return ERR_PTR(-ENOMEM);
	if (strlen(key) > KS3FS_MAX_KEY) {
		kfree(key);
		return ERR_PTR(-ENAMETOOLONG);
	}
	return key;
}

static void set_dentry_time(struct dentry *dentry)
{
	dentry->d_time = jiffies;
}

/* ---------- listing snapshots and attribute prefetch ---------- */

/*
 * The result of a readdir, kept on the directory for a short while.  A
 * stat of one of its entries soon afterwards (ls -l, find) is taken as a
 * sign that all of them will be stat'ed: their attributes are then fetched
 * with parallel HEADs and instantiated in one go.
 */
struct ks3fs_snap {
	struct kref ref;
	unsigned long time;
	atomic_t prefetched;
	struct ks3fs_listing l;
};

#define SNAP_WINDOW	(10 * HZ)
#define PREFETCH_MAX	20000

static void snap_free(struct kref *ref)
{
	struct ks3fs_snap *snap = container_of(ref, struct ks3fs_snap, ref);

	ks3fs_listing_free(&snap->l);
	kfree(snap);
}

static void snap_put(struct ks3fs_snap *snap)
{
	if (snap)
		kref_put(&snap->ref, snap_free);
}

/* Replace (or with NULL, drop) the directory's snapshot. */
static void dir_set_snap(struct inode *dir, struct ks3fs_snap *snap)
{
	struct ks3fs_inode *di = KS3_I(dir);
	struct ks3fs_snap *old;

	if (snap)
		kref_get(&snap->ref);
	spin_lock(&di->lock);
	old = di->snap;
	di->snap = snap;
	spin_unlock(&di->lock);
	snap_put(old);
}

void ks3fs_dir_forget_snap(struct inode *dir)
{
	dir_set_snap(dir, NULL);
}

static struct ks3fs_snap *dir_get_snap(struct inode *dir)
{
	struct ks3fs_inode *di = KS3_I(dir);
	struct ks3fs_snap *snap;

	spin_lock(&di->lock);
	snap = di->snap;
	if (snap)
		kref_get(&snap->ref);
	spin_unlock(&di->lock);
	return snap;
}

static int snap_find(struct ks3fs_snap *snap, const struct qstr *name)
{
	int lo = 0, hi = snap->l.nr - 1;

	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		int c = strncmp(snap->l.ents[mid].name, name->name, name->len);

		if (!c && snap->l.ents[mid].name[name->len])
			c = 1;
		if (!c)
			return mid;
		if (c < 0)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return -1;
}

/*
 * Instantiate a dentry for an entry whose attributes are known, so that a
 * following stat() needs no round trip.  Mirrors nfs_prime_dcache().
 */
static u64 prime_dcache(struct dentry *parent, const char *name,
			struct ks3fs_attr *attr)
{
	KS3_DALLOC_WQ(wq);
	struct inode *dir = d_inode(parent);
	struct qstr q = QSTR_INIT(name, strlen(name));
	struct dentry *dentry, *res;
	struct inode *inode;
	u64 ino = 0;
	char *key;

	q.hash = full_name_hash(parent, q.name, q.len);
	dentry = d_lookup(parent, &q);
	if (!dentry) {
		dentry = ks3_d_alloc_parallel(parent, &q, wq);
		if (IS_ERR(dentry))
			return 0;
		if (d_in_lookup(dentry)) {
			key = ks3fs_child_key(dir, &q, attr->is_dir);
			if (IS_ERR(key)) {
				d_lookup_done(dentry);
				dput(dentry);
				return 0;
			}
			inode = ks3fs_new_inode(dir->i_sb, key, attr);
			kfree(key);
			if (IS_ERR(inode)) {
				d_lookup_done(dentry);
				dput(dentry);
				return 0;
			}
			set_dentry_time(dentry);
			res = d_splice_alias(inode, dentry);
			d_lookup_done(dentry);
			if (res) {
				if (IS_ERR(res)) {
					dput(dentry);
					return 0;
				}
				dput(dentry);
				dentry = res;
			}
		}
	}
	if (d_really_is_positive(dentry))
		ino = d_inode(dentry)->i_ino;
	dput(dentry);
	return ino;
}

struct prefetch {
	struct kref ref;
	struct ks3fs_sb_info *sbi;
	struct ks3fs_snap *snap;
	char *dirkey;
	struct ks3fs_attr *attrs;
	int *status;
};

static void prefetch_free(struct kref *ref)
{
	struct prefetch *pf = container_of(ref, struct prefetch, ref);

	snap_put(pf->snap);
	kfree(pf->dirkey);
	kvfree(pf->attrs);
	kvfree(pf->status);
	kfree(pf);
}

static void prefetch_put(void *ctx)
{
	kref_put(&((struct prefetch *)ctx)->ref, prefetch_free);
}

static int prefetch_one(void *ctx, int i)
{
	struct prefetch *pf = ctx;
	struct ks3fs_list_entry *e = &pf->snap->l.ents[i];
	struct ks3fs_attr *attr = &pf->attrs[i];
	char *key;
	int err;

	key = kasprintf(GFP_KERNEL, "%s%s%s", pf->dirkey, e->name,
			e->is_dir ? "/" : "");
	if (!key) {
		pf->status[i] = -ENOMEM;
		return 0;
	}
	err = ks3fs_s3_head(pf->sbi, key, attr);
	kfree(key);
	if (e->is_dir) {
		/* the listing proved the prefix exists; the marker is optional */
		if (err == -ENOENT) {
			memset(attr, 0, sizeof(*attr));
			err = 0;
		} else if (!err) {
			attr->has_marker = true;
		}
		attr->is_dir = true;
	}
	pf->status[i] = err;
	return 0;	/* per-entry failures just leave it to lookup */
}

/*
 * Fetch the attributes of every entry of @snap in parallel and instantiate
 * them.  If @want is found, its attributes are returned in @found.
 */
static bool prefetch_dir(struct dentry *parent, struct ks3fs_snap *snap,
			 int want, struct ks3fs_attr *found)
{
	struct inode *dir = d_inode(parent);
	struct prefetch *pf;
	bool have = false;
	int i, nr = snap->l.nr, err;

	pf = kzalloc(sizeof(*pf), GFP_KERNEL);
	if (!pf)
		return false;
	kref_init(&pf->ref);
	kref_get(&snap->ref);
	pf->snap = snap;
	pf->sbi = KS3_SB(dir->i_sb);
	pf->dirkey = ks3fs_inode_key(dir);
	pf->attrs = kvcalloc(nr, sizeof(*pf->attrs), GFP_KERNEL);
	pf->status = kvcalloc(nr, sizeof(*pf->status), GFP_KERNEL);
	if (!pf->dirkey || !pf->attrs || !pf->status)
		goto out;

	kref_get(&pf->ref);	/* the run's */
	err = ks3fs_parallel(pf->sbi, nr, prefetch_one, pf, prefetch_put);
	if (err)
		goto out;
	for (i = 0; i < nr; i++) {
		if (pf->status[i])
			continue;
		if (i == want) {
			*found = pf->attrs[i];
			have = true;
		} else {
			prime_dcache(parent, snap->l.ents[i].name, &pf->attrs[i]);
		}
		cond_resched();
	}
out:
	prefetch_put(pf);
	return have;
}

/*
 * Answer a lookup from a prefetch of the whole directory, if a recent
 * listing suggests one is worthwhile (only the first such lookup does it).
 */
static bool lookup_prefetched(struct inode *dir, struct dentry *dentry,
			      struct ks3fs_attr *attr)
{
	struct ks3fs_snap *snap;
	bool have = false;
	int i;

	if (!KS3_SB(dir->i_sb)->meta)
		return false;	/* readdir instantiated everything already */
	snap = dir_get_snap(dir);
	if (!snap)
		return false;
	if (time_before(jiffies, snap->time + SNAP_WINDOW) &&
	    snap->l.nr > 1 && snap->l.nr <= PREFETCH_MAX &&
	    (i = snap_find(snap, &dentry->d_name)) >= 0 &&
	    !atomic_xchg(&snap->prefetched, 1))
		have = prefetch_dir(dentry->d_parent, snap, i, attr);
	/* used up or too old: don't keep the listing around */
	if (atomic_read(&snap->prefetched) ||
	    !time_before(jiffies, snap->time + SNAP_WINDOW))
		ks3fs_dir_forget_snap(dir);
	snap_put(snap);
	return have;
}

static struct dentry *ks3fs_lookup(struct inode *dir, struct dentry *dentry,
				   unsigned int flags)
{
	struct ks3fs_sb_info *sbi = KS3_SB(dir->i_sb);
	struct ks3fs_attr attr;
	struct inode *inode = NULL;
	char *key;
	int err;

	if (IS_ROOT(dentry->d_parent) && dentry->d_name.len == sizeof(KS3FS_ORPHANS) - 1 &&
	    !memcmp(dentry->d_name.name, KS3FS_ORPHANS, dentry->d_name.len))
		return d_splice_alias(NULL, dentry);	/* hidden */

	key = ks3fs_child_key(dir, &dentry->d_name, false);
	if (IS_ERR(key))
		return ERR_CAST(key);

	if (lookup_prefetched(dir, dentry, &attr))
		err = 0;
	else
		err = ks3fs_s3_stat(sbi, key, &attr);
	if (err && err != -ENOENT) {
		kfree(key);
		return ERR_PTR(err);
	}
	if (!err) {
		if (attr.is_dir) {
			char *dkey = kasprintf(GFP_KERNEL, "%s/", key);

			kfree(key);
			if (!dkey)
				return ERR_PTR(-ENOMEM);
			key = dkey;
		}
		inode = ks3fs_new_inode(dir->i_sb, key, &attr);
	}
	kfree(key);
	if (IS_ERR(inode))
		return ERR_CAST(inode);
	set_dentry_time(dentry);
	return d_splice_alias(inode, dentry);
}

/* ---------- readdir ---------- */

/*
 * d_ino for an entry: the cached inode's number if there is one (which
 * may predate a rename), else the hash of its key that a lookup would
 * give it, so readdir always agrees with stat().
 */
static u64 entry_ino(struct dentry *parent, struct inode *dir,
		     struct ks3fs_list_entry *e)
{
	struct qstr q = QSTR_INIT(e->name, strlen(e->name));
	struct dentry *dentry;
	u64 ino = 0;
	char *key;

	q.hash = full_name_hash(parent, q.name, q.len);
	dentry = d_lookup(parent, &q);
	if (dentry) {
		if (d_really_is_positive(dentry))
			ino = d_inode(dentry)->i_ino;
		dput(dentry);
	}
	if (ino)
		return ino;
	key = ks3fs_child_key(dir, &q, e->is_dir);
	if (IS_ERR(key))
		return 1;
	ino = ks3fs_key_ino(key);
	kfree(key);
	return ino;
}

static int ks3fs_readdir(struct file *file, struct dir_context *ctx)
{
	struct inode *dir = file_inode(file);
	struct ks3fs_snap *snap = file->private_data;
	struct ks3fs_listing *l;
	int i, err;

	if (ctx->pos == 0 && snap) {	/* rewinddir: take a fresh snapshot */
		snap_put(snap);
		file->private_data = snap = NULL;
	}
	if (!snap) {
		char *key = ks3fs_inode_key(dir);

		if (!key)
			return -ENOMEM;
		snap = kzalloc(sizeof(*snap), GFP_KERNEL);
		if (!snap) {
			kfree(key);
			return -ENOMEM;
		}
		kref_init(&snap->ref);
		err = ks3fs_s3_list_dir(KS3_SB(dir->i_sb), key, &snap->l);
		kfree(key);
		if (err) {
			kfree(snap);
			return err;
		}
		snap->time = jiffies;
		file->private_data = snap;
		dir_set_snap(dir, snap);
	}
	l = &snap->l;

	if (!dir_emit_dots(file, ctx))
		return 0;
	for (i = ctx->pos - 2; i < l->nr; i++) {
		struct ks3fs_list_entry *e = &l->ents[i];
		unsigned int type = e->is_dir ? DT_DIR : DT_REG;
		u64 ino = 0;

		/*
		 * A listing carries no metadata: with metadata enabled an
		 * object may be a symlink and every mode needs a HEAD, so
		 * objects are left to lookup (which may prefetch them all).
		 */
		if (!KS3_SB(dir->i_sb)->meta) {
			struct ks3fs_attr attr = {
				.is_dir = e->is_dir, .size = e->size,
				.mtime = e->mtime,
			};

			strscpy(attr.etag, e->etag, sizeof(attr.etag));
			ino = prime_dcache(file->f_path.dentry, e->name, &attr);
		} else if (!e->is_dir) {
			type = DT_UNKNOWN;
		}
		if (!ino)
			ino = entry_ino(file->f_path.dentry, dir, e);
		if (!dir_emit(ctx, e->name, strlen(e->name), ino, type))
			return 0;
		ctx->pos++;
	}
	return 0;
}

static int ks3fs_dir_fsync(struct file *file, loff_t start, loff_t end,
			   int datasync)
{
	return ks3fs_push_meta(file_inode(file));
}

static int ks3fs_dir_release(struct inode *inode, struct file *file)
{
	snap_put(file->private_data);
	return 0;
}

static loff_t ks3fs_dir_llseek(struct file *file, loff_t offset, int whence)
{
	/* positions are indexes into the snapshot */
	return generic_file_llseek_size(file, offset, whence, LLONG_MAX, LLONG_MAX);
}

/* ---------- modifications ---------- */

/*
 * Metadata for a new entry: the caller's fsuid, and its fsgid unless the
 * parent is setgid (the usual inode_init_owner() rules, minus idmaps).
 */
static struct ks3fs_meta *new_meta(struct inode *dir, umode_t mode,
				   struct ks3fs_meta *m)
{
	if (!KS3_SB(dir->i_sb)->meta)
		return NULL;
	ks3fs_meta_clear(m);
	m->has_mode = m->has_uid = m->has_gid = m->has_mtime = true;
	m->mode = mode;
	m->uid = from_kuid_munged(&init_user_ns, current_fsuid());
	if (dir->i_mode & S_ISGID) {
		m->gid = from_kgid_munged(&init_user_ns, dir->i_gid);
		if (S_ISDIR(mode))
			m->mode |= S_ISGID;
	} else {
		m->gid = from_kgid_munged(&init_user_ns, current_fsgid());
	}
	ktime_get_real_ts64(&m->mtime);
	m->mtime.tv_nsec = 0;	/* the store keeps whole seconds */
	return m;
}

static int ks3fs_create(struct mnt_idmap *idmap, struct inode *dir,
			struct dentry *dentry, umode_t mode KS3_CREATE_EXCL)
{
	struct ks3fs_sb_info *sbi = KS3_SB(dir->i_sb);
	struct ks3fs_attr attr = {};
	struct inode *inode;
	char *key;
	int err;

	if (!S_ISREG(mode))
		return -EPERM;
	key = ks3fs_child_key(dir, &dentry->d_name, false);
	if (IS_ERR(key))
		return PTR_ERR(key);

	/* create the object now so it is visible and permissions are checked */
	err = ks3fs_s3_put_buf(sbi, key, NULL, 0,
			       new_meta(dir, S_IFREG | mode, &attr.meta),
			       attr.etag);
	if (err)
		goto out;
	inode = ks3fs_new_inode(dir->i_sb, key, &attr);
	if (IS_ERR(inode)) {
		err = PTR_ERR(inode);
		goto out;
	}
	set_dentry_time(dentry);
	d_instantiate(dentry, inode);
	ks3fs_dir_forget_snap(dir);
	inode_set_mtime_to_ts(dir, inode_set_ctime_current(dir));
out:
	kfree(key);
	return err;
}

static KS3_MKDIR_RET ks3fs_mkdir(struct mnt_idmap *idmap, struct inode *dir,
				 struct dentry *dentry, umode_t mode)
{
	struct ks3fs_sb_info *sbi = KS3_SB(dir->i_sb);
	struct ks3fs_attr attr = { .is_dir = true, .has_marker = true };
	struct inode *inode;
	char *key;
	int err;

	key = ks3fs_child_key(dir, &dentry->d_name, true);
	if (IS_ERR(key))
		return KS3_MKDIR_RETURN(PTR_ERR(key));
	err = ks3fs_s3_put_buf(sbi, key, NULL, 0,
			       new_meta(dir, S_IFDIR | mode, &attr.meta), NULL);
	if (!err) {
		inode = ks3fs_new_inode(dir->i_sb, key, &attr);
		if (IS_ERR(inode)) {
			err = PTR_ERR(inode);
		} else {
			set_dentry_time(dentry);
			d_instantiate(dentry, inode);
			ks3fs_dir_forget_snap(dir);
		}
	}
	kfree(key);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 15, 0)
	return err ? ERR_PTR(err) : NULL;
#else
	return err;
#endif
}

/* A symlink is an object whose body is the target (s3fs-fuse layout). */
static int ks3fs_symlink(struct mnt_idmap *idmap, struct inode *dir,
			 struct dentry *dentry, const char *target)
{
	struct ks3fs_sb_info *sbi = KS3_SB(dir->i_sb);
	struct ks3fs_attr attr = {};
	size_t len = strlen(target);
	struct inode *inode;
	char *key;
	int err;

	if (!sbi->meta)
		return -EPERM;	/* nothing would mark the object as a link */
	if (len > PAGE_SIZE - 1)
		return -ENAMETOOLONG;
	key = ks3fs_child_key(dir, &dentry->d_name, false);
	if (IS_ERR(key))
		return PTR_ERR(key);
	err = ks3fs_s3_put_buf(sbi, key, target, len,
			       new_meta(dir, S_IFLNK | 0777, &attr.meta),
			       attr.etag);
	if (err)
		goto out;
	attr.size = len;
	attr.mtime = attr.meta.mtime.tv_sec;
	inode = ks3fs_new_inode(dir->i_sb, key, &attr);
	if (IS_ERR(inode)) {
		err = PTR_ERR(inode);
		goto out;
	}
	set_dentry_time(dentry);
	d_instantiate(dentry, inode);
	ks3fs_dir_forget_snap(dir);
	inode_set_mtime_to_ts(dir, inode_set_ctime_current(dir));
out:
	kfree(key);
	return err;
}

static int ks3fs_unlink(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = d_inode(dentry);
	char *key = ks3fs_inode_key(inode);
	int err;

	if (!key)
		return -ENOMEM;
	/* the VFS holds the inode lock: keep data of open files readable */
	err = ks3fs_orphan(inode);
	if (!err)
		err = ks3fs_s3_delete(KS3_SB(dir->i_sb), key);
	kfree(key);
	if (err)
		return err;
	clear_nlink(inode);	/* also stops any pending upload */
	ks3fs_dir_forget_snap(dir);
	inode_set_mtime_to_ts(dir, inode_set_ctime_current(dir));
	return 0;
}

static int ks3fs_rmdir(struct inode *dir, struct dentry *dentry)
{
	struct ks3fs_sb_info *sbi = KS3_SB(dir->i_sb);
	struct inode *inode = d_inode(dentry);
	char *key = ks3fs_inode_key(inode);
	int err;

	if (!key)
		return -ENOMEM;
	err = ks3fs_s3_dir_is_empty(sbi, key);
	if (err == 0)
		err = -ENOTEMPTY;
	else if (err > 0)
		err = ks3fs_s3_delete(sbi, key);
	kfree(key);
	if (err)
		return err;
	clear_nlink(inode);
	ks3fs_dir_forget_snap(dir);
	inode_set_mtime_to_ts(dir, inode_set_ctime_current(dir));
	return 0;
}

/* ---------- directory rename ---------- */

struct dir_move {
	struct kref ref;
	struct ks3fs_sb_info *sbi;
	struct ks3fs_keylist kl;
	char *src, *dst;	/* prefixes, both ending in '/' */
	size_t srclen;
	bool *copied;
};

static void dir_move_free(struct kref *ref)
{
	struct dir_move *dm = container_of(ref, struct dir_move, ref);

	ks3fs_keylist_free(&dm->kl);
	kvfree(dm->copied);
	kfree(dm->src);
	kfree(dm->dst);
	kfree(dm);
}

static void dir_move_put(void *ctx)
{
	kref_put(&((struct dir_move *)ctx)->ref, dir_move_free);
}

/* Run one phase over every key; the run holds a reference of its own. */
static int dir_move_phase(struct dir_move *dm, int (*fn)(void *, int))
{
	kref_get(&dm->ref);
	return ks3fs_parallel(dm->sbi, dm->kl.nr, fn, dm, dir_move_put);
}

static char *moved_key(struct dir_move *dm, int i)
{
	return kasprintf(GFP_KERNEL, "%s%s", dm->dst,
			 dm->kl.ents[i].key + dm->srclen);
}

static int dm_copy(void *ctx, int i)
{
	struct dir_move *dm = ctx;
	struct ks3fs_keyent *e = &dm->kl.ents[i];
	char etag[KS3FS_ETAG_LEN];
	char *dst = moved_key(dm, i);
	int err;

	if (!dst)
		return -ENOMEM;
	if (e->size == 0 && dst[strlen(dst) - 1] == '/') {
		/*
		 * A directory marker: recreate it rather than copy it, as some
		 * servers (versitygw) refuse CopyObject onto a "dir/" key.
		 */
		struct ks3fs_attr attr;

		err = ks3fs_s3_head(dm->sbi, e->key, &attr);
		if (!err)
			err = ks3fs_s3_put_buf(dm->sbi, dst, NULL, 0, &attr.meta,
					       NULL);
	} else if (e->size > KS3FS_MAX_PUT)
		err = ks3fs_s3_copy_large(dm->sbi, e->key, dst, e->size, NULL,
					  etag);
	else
		err = ks3fs_s3_copy(dm->sbi, e->key, dst, etag);
	kfree(dst);
	if (!err)
		dm->copied[i] = true;
	return err;
}

static int dm_undo(void *ctx, int i)
{
	struct dir_move *dm = ctx;
	char *dst;

	if (!dm->copied[i])
		return 0;
	dst = moved_key(dm, i);
	if (dst) {
		ks3fs_s3_delete(dm->sbi, dst);	/* best effort */
		kfree(dst);
	}
	return 0;
}

static int dm_delete(void *ctx, int i)
{
	struct dir_move *dm = ctx;

	return ks3fs_s3_delete(dm->sbi, dm->kl.ents[i].key);
}

/* Refuse (-EBUSY) if a file below has unsaved data; store pending metadata. */
static int dm_prepare_one(struct inode *inode, void *arg)
{
	struct ks3fs_inode *ki = KS3_I(inode);

	if (test_bit(KS3_I_DIRTY, &ki->flags) || ki->mpu)
		return -EBUSY;
	return ks3fs_push_meta(inode);
}

/*
 * S3 has no prefix rename: copy every object below the directory to the
 * new prefix, then delete the originals.  A failure while copying undoes
 * the copies; the namespace only changes once everything is copied.
 */
static int rename_dir(struct inode *old_dir, struct dentry *old_dentry,
		      struct inode *new_dir, struct dentry *new_dentry)
{
	struct ks3fs_sb_info *sbi = KS3_SB(old_dir->i_sb);
	struct inode *inode = d_inode(old_dentry);
	struct inode *target = d_inode(new_dentry);
	struct dir_move *dm;
	bool locked;
	int err;

	dm = kzalloc(sizeof(*dm), GFP_KERNEL);
	if (!dm)
		return -ENOMEM;
	kref_init(&dm->ref);
	dm->sbi = sbi;
	dm->src = ks3fs_inode_key(inode);
	dm->dst = ks3fs_child_key(new_dir, &new_dentry->d_name, true);
	if (IS_ERR(dm->dst)) {
		err = PTR_ERR(dm->dst);
		dm->dst = NULL;
		goto out;
	}
	if (!dm->src || !*dm->src) {
		err = -EINVAL;
		goto out;
	}
	dm->srclen = strlen(dm->src);
	if (!strncmp(dm->dst, dm->src, dm->srclen)) {
		err = -EINVAL;	/* into its own subtree (the VFS checks too) */
		goto out;
	}
	if (target) {
		err = ks3fs_s3_dir_is_empty(sbi, dm->dst);
		if (err <= 0) {
			err = err ?: -ENOTEMPTY;
			goto out;
		}
	}

	/*
	 * Keep entries from appearing directly inside it while we copy.  For
	 * a move to another parent the VFS already holds this lock (6.5+).
	 */
	locked = old_dir == new_dir;
	if (locked)
		inode_lock_nested(inode, I_MUTEX_CHILD);
	err = ks3fs_for_each_cached(inode->i_sb, dm->src, dm_prepare_one, NULL);
	if (err == -EBUSY)
		err = -EXDEV;	/* files being written below: let mv(1) copy */
	if (!err)
		err = ks3fs_s3_list_keys(sbi, dm->src, KS3FS_MAX_DIR_MOVE, &dm->kl);
	if (err == -E2BIG)
		err = -EXDEV;
	if (err)
		goto unlock;
	dm->copied = kvcalloc(max(dm->kl.nr, 1), sizeof(bool), GFP_KERNEL);
	if (!dm->copied) {
		err = -ENOMEM;
		goto unlock;
	}

	err = dir_move_phase(dm, dm_copy);
	if (err) {
		dir_move_phase(dm, dm_undo);
		goto unlock;
	}
	err = dir_move_phase(dm, dm_delete);
	if (err) {
		/* everything exists at the new name; some old keys linger */
		pr_warn("ks3fs: rename of '%s' to '%s': removing old objects failed: %d\n",
			dm->src, dm->dst, err);
		goto unlock;
	}

	ks3fs_rekey_subtree(inode->i_sb, dm->src, dm->dst);
	ks3fs_dir_forget_snap(old_dir);
	ks3fs_dir_forget_snap(new_dir);
	if (target)
		clear_nlink(target);
	inode_set_mtime_to_ts(old_dir, inode_set_ctime_current(old_dir));
	if (new_dir != old_dir)
		inode_set_mtime_to_ts(new_dir, inode_set_ctime_current(new_dir));
	set_dentry_time(new_dentry);
unlock:
	if (locked)
		inode_unlock(inode);
out:
	dir_move_put(dm);
	return err;
}

static int ks3fs_rename(struct mnt_idmap *idmap, struct inode *old_dir,
			struct dentry *old_dentry, struct inode *new_dir,
			struct dentry *new_dentry, unsigned int flags)
{
	struct ks3fs_sb_info *sbi = KS3_SB(old_dir->i_sb);
	struct inode *inode = d_inode(old_dentry);
	struct inode *target = d_inode(new_dentry);
	struct ks3fs_inode *ki = KS3_I(inode);
	char *src = NULL, *dst = NULL;
	char etag[KS3FS_ETAG_LEN];
	int err;

	if (flags & ~RENAME_NOREPLACE)
		return -EINVAL;
	if (S_ISDIR(inode->i_mode))
		return rename_dir(old_dir, old_dentry, new_dir, new_dentry);

	/* the VFS holds the source inode lock for non-directories */
	err = ks3fs_upload_locked(inode);
	if (!err && target)	/* ...and the target's: it is about to go */
		err = ks3fs_orphan(target);
	if (err)
		return err;

	src = ks3fs_inode_key(inode);
	dst = ks3fs_child_key(new_dir, &new_dentry->d_name, false);
	if (!src || IS_ERR(dst)) {
		err = !src ? -ENOMEM : PTR_ERR(dst);
		dst = NULL;
		goto out;
	}
	if (i_size_read(inode) > KS3FS_MAX_PUT)	/* too big for CopyObject */
		err = ks3fs_s3_copy_large(sbi, src, dst, i_size_read(inode),
					  NULL, etag);
	else
		err = ks3fs_s3_copy(sbi, src, dst, etag);
	if (err)
		goto out;
	err = ks3fs_s3_delete(sbi, src);
	if (err) {
		/* keep the namespace consistent with the store */
		ks3fs_s3_delete(sbi, dst);
		goto out;
	}

	spin_lock(&ki->lock);
	swap(ki->key, dst);
	if (etag[0])
		strscpy(ki->etag, etag, sizeof(ki->etag));
	spin_unlock(&ki->lock);
	if (target)
		clear_nlink(target);
	ks3fs_dir_forget_snap(old_dir);
	ks3fs_dir_forget_snap(new_dir);
	inode_set_ctime_current(inode);
	inode_set_mtime_to_ts(old_dir, inode_set_ctime_current(old_dir));
	if (new_dir != old_dir)
		inode_set_mtime_to_ts(new_dir, inode_set_ctime_current(new_dir));
	set_dentry_time(new_dentry);
out:
	kfree(src);
	kfree(dst);
	return err;
}

/* ---------- revalidation ---------- */

static bool attrs_fresh(struct ks3fs_sb_info *sbi, unsigned long when)
{
	return time_before(jiffies, when + sbi->ttl);
}

static int ks3fs_d_revalidate(KS3_REVALIDATE_ARGS)
{
	struct ks3fs_sb_info *sbi = KS3_SB(dentry->d_sb);
	struct inode *inode = d_inode_rcu(dentry);
	struct ks3fs_inode *ki;
	struct ks3fs_attr attr;
	char *key;
	size_t kl;
	int err;

	if (!inode)	/* negative: trust it for one ttl */
		return attrs_fresh(sbi, dentry->d_time) ? 1 : 0;

	ki = KS3_I(inode);
	if (IS_ROOT(dentry) || test_bit(KS3_I_DIRTY, &ki->flags) ||
	    attrs_fresh(sbi, ki->attr_time))
		return 1;
	if (flags & LOOKUP_RCU)
		return -ECHILD;

	key = ks3fs_inode_key(inode);
	if (!key)
		return 1;
	kl = strlen(key);
	if (kl && key[kl - 1] == '/')
		key[kl - 1] = '\0';
	err = ks3fs_s3_stat(sbi, key, &attr);
	kfree(key);
	if (err == -ENOENT)
		return 0;
	if (err)
		return 1;	/* transient failure: keep what we have */
	if (ks3fs_attr_type(sbi, &attr) != (inode->i_mode & S_IFMT))
		return 0;	/* replaced by something of another type */

	if (!attr.is_dir) {
		bool changed;

		spin_lock(&ki->lock);
		changed = strcmp(ki->etag, attr.etag) != 0;
		spin_unlock(&ki->lock);
		if (changed && !test_bit(KS3_I_DIRTY, &ki->flags) &&
		    !mapping_mapped(inode->i_mapping)) {
			ks3fs_apply_attr(inode, &attr);
			invalidate_inode_pages2(inode->i_mapping);
		} else if (!changed) {
			ks3fs_apply_attr(inode, &attr);	/* metadata may differ */
		}
	} else {
		ks3fs_apply_attr(inode, &attr);
	}
	ki->attr_time = jiffies;
	return 1;
}

const struct dentry_operations ks3fs_dops = {
	.d_revalidate	= ks3fs_d_revalidate,
};

const struct inode_operations ks3fs_dir_iops = {
	.lookup		= ks3fs_lookup,
	.create		= ks3fs_create,
	.mkdir		= ks3fs_mkdir,
	.unlink		= ks3fs_unlink,
	.symlink	= ks3fs_symlink,
	.rmdir		= ks3fs_rmdir,
	.rename		= ks3fs_rename,
	.setattr	= ks3fs_setattr,
	.getattr	= ks3fs_getattr,
};

const struct file_operations ks3fs_dir_fops = {
	.llseek		= ks3fs_dir_llseek,
	.read		= generic_read_dir,
	.iterate_shared	= ks3fs_readdir,
	.release	= ks3fs_dir_release,
	.fsync		= ks3fs_dir_fsync,
};
