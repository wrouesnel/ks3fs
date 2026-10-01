// SPDX-License-Identifier: GPL-2.0
/*
 * ks3fs file data.
 *
 * Reads go through the page cache and are filled with ranged GETs (guarded
 * by If-Match on the ETag we saw).  Writes land in the page cache; dirty
 * folios are pinned (there is no page-by-page writeback to an object store)
 * and the whole object is PUT on flush/fsync/last close.  Shared writable
 * mmaps dirty folios through ->page_mkwrite; uploads write-protect each
 * folio before sending it, so later stores fault again and are kept.
 */
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/highmem.h>
#include <linux/uio.h>
#include <linux/writeback.h>
#include <linux/rmap.h>
#if __has_include(<linux/folio_batch.h>)	/* pagevec.h went away in 7.3 */
#include <linux/folio_batch.h>
#else
#include <linux/pagevec.h>
#endif
#include <linux/sched/signal.h>
#include <linux/bvec.h>
#include <linux/falloc.h>
#include <linux/posix_acl.h>

#include "ks3fs.h"

static bool range_frozen(struct inode *inode, loff_t pos, loff_t len);

/* ---------- ranged GET streaming ---------- */

struct get_stream {
	struct ks3fs_sb_info *sbi;
	struct inode *inode;
	struct ks3fs_conn *conn;
	struct ks3fs_resp resp;
	struct ks3fs_retry retry;	/* shared by every (re)connect */
	loff_t pos;			/* next byte the stream delivers */
	loff_t end;			/* exclusive */
};

/* (Re)issue the GET for [gs->pos, gs->end). */
static int get_open(struct get_stream *gs)
{
	struct ks3fs_inode *ki = KS3_I(gs->inode);
	struct ks3fs_req req = { .method = "GET" };
	char range[64], etag[KS3FS_ETAG_LEN];
	char *key;
	int err;

	key = ks3fs_inode_key(gs->inode);
	if (!key)
		return -ENOMEM;
	spin_lock(&ki->lock);
	strscpy(etag, ki->etag, sizeof(etag));
	spin_unlock(&ki->lock);

	snprintf(range, sizeof(range), "bytes=%lld-%lld", gs->pos, gs->end - 1);
	req.key = key;
	req.range = range;
	/* never splice bytes from two versions of the object */
	if (etag[0])
		req.if_match = etag;

	gs->conn = ks3fs_http_start(gs->sbi, &req, &gs->resp, NULL, NULL,
				    &gs->retry);
	if (IS_ERR(gs->conn)) {
		err = PTR_ERR(gs->conn);
		gs->conn = NULL;
		goto out;
	}
	err = ks3fs_status_to_errno(gs->resp.status);
	if (!err && gs->resp.status == 200 && gs->pos != 0)
		err = -EIO;	/* server ignored Range; don't stream the world */
	if (!err && gs->resp.status == 206 && gs->resp.range_start != gs->pos)
		err = -EIO;
	if (err == -ESTALE)	/* object changed under us: refetch attrs soon */
		ki->attr_time = 0;
	if (err) {
		pr_warn_ratelimited("ks3fs: GET %s range %s: HTTP %d\n",
				    key, range, gs->resp.status);
		ks3fs_http_finish(gs->sbi, gs->conn, &gs->resp);
		gs->conn = NULL;
	}
out:
	kfree(key);
	return err;
}

/* Start streaming [start, start+len) of the inode's object. */
static int get_begin(struct inode *inode, loff_t start, loff_t len,
		     struct get_stream *gs)
{
	gs->sbi = KS3_SB(inode->i_sb);
	gs->inode = inode;
	gs->conn = NULL;
	gs->pos = start;
	gs->end = start + len;
	ks3fs_retry_init(&gs->retry);
	return get_open(gs);
}

/*
 * Read exactly @len bytes from the stream.  If the connection breaks the
 * GET is resumed from the current position on a new connection.
 */
static int get_read(struct get_stream *gs, void *buf, size_t len)
{
	while (len) {
		ssize_t n = -EPROTO;
		int err;

		if (gs->conn)
			n = ks3fs_http_read_body(gs->conn, &gs->resp, buf, len);
		if (n > 0) {
			buf += n;
			len -= n;
			gs->pos += n;
			continue;
		}
		err = n ? n : -EPROTO;	/* 0 here means a short body */
		if (gs->conn)
			ks3fs_http_finish(gs->sbi, gs->conn, &gs->resp);
		gs->conn = NULL;
		if (!ks3fs_retry(gs->sbi, &gs->retry, err))
			return ks3fs_retry_giveup(err);
		err = get_open(gs);
		if (err)
			return err;
	}
	return 0;
}

static void get_end(struct get_stream *gs)
{
	if (gs->conn)
		ks3fs_http_finish(gs->sbi, gs->conn, &gs->resp);
	gs->conn = NULL;
}

/*
 * Fill @folio: bytes below @avail come from the stream, the rest is zeroed.
 * @avail is relative to the folio start.
 */
static int fill_folio(struct get_stream *gs, struct folio *folio, size_t avail)
{
	size_t fsize = folio_size(folio), off = 0;
	int err = 0;

	avail = min(avail, fsize);
	while (off < fsize) {
		size_t chunk = min_t(size_t, PAGE_SIZE - offset_in_page(off),
				     fsize - off);
		char *p = kmap_local_folio(folio, off);

		if (off < avail) {
			size_t n = min(chunk, avail - off);

			err = get_read(gs, p, n);
			if (!err && n < chunk)
				memset(p + n, 0, chunk - n);
		} else {
			memset(p, 0, chunk);
		}
		kunmap_local(p);
		if (err)
			return err;
		off += chunk;
	}
	flush_dcache_folio(folio);
	return 0;
}

/* Bytes of the file that must come from the store (rest reads as zero). */
static loff_t remote_extent(struct inode *inode)
{
	return min(i_size_read(inode), KS3_I(inode)->remote_size);
}

/* Synchronously fill a locked folio; does not unlock it. */
static int read_folio_sync(struct inode *inode, struct folio *folio)
{
	loff_t pos = folio_pos(folio), end = remote_extent(inode);
	struct get_stream gs;
	int err;

	if (pos >= end) {
		folio_zero_range(folio, 0, folio_size(folio));
		return 0;
	}
	err = get_begin(inode, pos, min_t(loff_t, folio_size(folio), end - pos),
			&gs);
	if (err)
		return err;
	err = fill_folio(&gs, folio, end - pos);
	get_end(&gs);
	return err;
}

static int ks3fs_read_folio(struct file *file, struct folio *folio)
{
	struct inode *inode = folio->mapping->host;
	int err;

	/* never fill a frozen range from the old object: that would be stale */
	if (range_frozen(inode, folio_pos(folio), folio_size(folio)))
		err = -EIO;
	else
		err = read_folio_sync(inode, folio);

	if (!err)
		folio_mark_uptodate(folio);
	folio_unlock(folio);
	return err;
}

/*
 * Parallel readahead: a large window is split into contiguous chunks that
 * are fetched concurrently (one ranged GET each), which is what makes
 * sequential reads fast over high-latency links.  The job owns the locked
 * folios (with references of its own): its release, after every worker is
 * done, marks them uptodate and unlocks them, so a killed reader can never
 * unlock folios that are still being filled.
 */
#define RA_CHUNK_MIN	(256 << 10)	/* up to 16 requests per 4 MiB window */

struct ra_job {
	struct inode *inode;
	loff_t end;			/* remote_extent() */
	int nr_folios, nr_chunks;
	struct folio **folios;
	int *first;			/* first folio of each chunk (+1 sentinel) */
	int *err;			/* per chunk */
};

static int ra_chunk(void *ctx, int c)
{
	struct ra_job *j = ctx;
	int i = j->first[c], last = j->first[c + 1];
	loff_t start = folio_pos(j->folios[i]);
	loff_t stop = min(folio_pos(j->folios[last - 1]) +
			  (loff_t)folio_size(j->folios[last - 1]), j->end);
	struct get_stream gs = { .conn = NULL };
	int err = 0;

	if (start < j->end)
		err = get_begin(j->inode, start, stop - start, &gs);
	for (; i < last && !err; i++) {
		struct folio *folio = j->folios[i];
		loff_t fpos = folio_pos(folio);

		if (fpos >= j->end)
			folio_zero_range(folio, 0, folio_size(folio));
		else
			err = fill_folio(&gs, folio, j->end - fpos);
	}
	get_end(&gs);
	j->err[c] = err;
	return 0;	/* a failed chunk just leaves its folios to read_folio */
}

static void ra_release(void *ctx)
{
	struct ra_job *j = ctx;
	int c, i;

	for (c = 0; c < j->nr_chunks; c++)
		for (i = j->first[c]; i < j->first[c + 1]; i++) {
			struct folio *folio = j->folios[i];

			if (!j->err[c])
				folio_mark_uptodate(folio);
			folio_unlock(folio);
			folio_put(folio);
		}
	kvfree(j->folios);
	kfree(j->first);
	kfree(j->err);
	kfree(j);
}

/* Returns false if the window could not be set up for parallel fetching. */
static bool readahead_parallel(struct readahead_control *rac, loff_t len)
{
	struct inode *inode = rac->mapping->host;
	struct ks3fs_sb_info *sbi = KS3_SB(inode->i_sb);
	int nchunks = min_t(loff_t, sbi->parallel, len / RA_CHUNK_MIN);
	unsigned int max = readahead_count(rac);
	struct folio *folio;
	struct ra_job *j;
	loff_t per, acc = 0;
	int c = 0;

	if (nchunks < 2)
		return false;
	j = kzalloc(sizeof(*j), GFP_KERNEL);
	if (!j)
		return false;
	j->folios = kvmalloc_array(max, sizeof(*j->folios), GFP_KERNEL);
	j->first = kcalloc(nchunks + 1, sizeof(int), GFP_KERNEL);
	j->err = kcalloc(nchunks, sizeof(int), GFP_KERNEL);
	if (!j->folios || !j->first || !j->err) {
		kvfree(j->folios);
		kfree(j->first);
		kfree(j->err);
		kfree(j);
		return false;
	}
	j->inode = inode;
	j->end = remote_extent(inode);

	/* take every folio (locked, with our own reference), cut into chunks */
	per = DIV_ROUND_UP_ULL(readahead_length(rac), nchunks);
	while ((folio = readahead_folio(rac))) {
		folio_get(folio);
		if (acc >= per && c + 1 < nchunks) {
			j->first[++c] = j->nr_folios;
			acc = 0;
		}
		j->folios[j->nr_folios++] = folio;
		acc += folio_size(folio);
	}
	j->nr_chunks = c + 1;
	j->first[j->nr_chunks] = j->nr_folios;

	/* ra_release() finishes the folios whatever happens (even -EINTR) */
	ks3fs_parallel(sbi, j->nr_chunks, ra_chunk, j, ra_release);
	return true;
}

static void ks3fs_readahead(struct readahead_control *rac)
{
	struct inode *inode = rac->mapping->host;
	loff_t start = readahead_pos(rac), end = remote_extent(inode);
	loff_t len = min_t(loff_t, readahead_length(rac), end - start);
	struct get_stream gs = { .conn = NULL };
	struct folio *folio;
	int err = 0;

	if (range_frozen(inode, start, readahead_length(rac)))
		err = -EIO;	/* leave it to read_folio (and its caller) */
	else if (len >= 2 * RA_CHUNK_MIN && readahead_parallel(rac, len))
		return;
	else if (len > 0)
		err = get_begin(inode, start, len, &gs);

	while ((folio = readahead_folio(rac))) {
		if (!err) {
			loff_t fpos = folio_pos(folio);

			if (fpos >= end)
				folio_zero_range(folio, 0, folio_size(folio));
			else
				err = fill_folio(&gs, folio, end - fpos);
			if (!err)
				folio_mark_uptodate(folio);
		}
		folio_unlock(folio);
	}
	get_end(&gs);
}

/* ---------- buffered writes ---------- */

static int ks3fs_write_begin(KS3_WB_CTX, struct address_space *mapping,
			     loff_t pos, unsigned int len, KS3_WB_FOLIO_ARG,
			     void **fsdata)
{
	struct inode *inode = mapping->host;
	struct folio *folio;
	int err;

	if (pos + len > ks3fs_max_object(KS3_SB(inode->i_sb)))
		return -EFBIG;

	folio = __filemap_get_folio(mapping, pos >> PAGE_SHIFT, FGP_WRITEBEGIN,
				    mapping_gfp_mask(mapping));
	if (IS_ERR(folio))
		return PTR_ERR(folio);

	if (!folio_test_uptodate(folio) && len != folio_size(folio)) {
		/* partial write: existing bytes must come from the store */
		if (folio_pos(folio) >= remote_extent(inode)) {
			size_t from = offset_in_folio(folio, pos);

			folio_zero_segments(folio, 0, from, from + len,
					    folio_size(folio));
		} else {
			err = read_folio_sync(inode, folio);
			if (err) {
				folio_unlock(folio);
				folio_put(folio);
				return err;
			}
			folio_mark_uptodate(folio);
		}
	}
	KS3_WB_SET_FOLIO(folio);
	return 0;
}

static int ks3fs_write_end(KS3_WB_CTX, struct address_space *mapping,
			   loff_t pos, unsigned int len, unsigned int copied,
			   KS3_WE_FOLIO_ARG, void *fsdata)
{
	struct folio *f = KS3_WE_GET_FOLIO();
	struct inode *inode = mapping->host;

	if (!folio_test_uptodate(f)) {
		/* only full-folio writes get here; retry a short copy */
		if (copied < len) {
			copied = 0;
			goto out;
		}
		folio_mark_uptodate(f);
	}
	if (pos + copied > inode->i_size) {
		i_size_write(inode, pos + copied);
		inode->i_blocks = DIV_ROUND_UP_ULL(pos + copied, 512);
	}
	if (copied) {
		folio_mark_dirty(f);
		set_bit(KS3_I_DIRTY, &KS3_I(inode)->flags);
	}
out:
	folio_unlock(f);
	folio_put(f);
	return copied;
}

const struct address_space_operations ks3fs_aops = {
	.read_folio	= ks3fs_read_folio,
	.readahead	= ks3fs_readahead,
	.write_begin	= ks3fs_write_begin,
	.write_end	= ks3fs_write_end,
	.dirty_folio	= noop_dirty_folio,
};

/* ---------- upload ---------- */

/*
 * Small files are uploaded with one PUT on close/fsync.  Once a file
 * reaches two parts, completed parts behind the write cursor are streamed
 * out as S3 multipart parts while the file is still being written, and
 * their page cache folios become clean (reclaimable); the upload is
 * completed on close/fsync, with parts the writer never touched copied
 * server side from the previous version of the object.
 *
 * Uploaded parts exist nowhere readable until completion, so they are
 * frozen: a write or read that touches one completes the upload first,
 * and read_folio() refuses to fill such a range from the old object.
 */
struct ks3fs_mpu {
	char *upload_id;
	char *key;
	u64 part_size;
	int cap;				/* entries in etags/uploaded */
	char (*etags)[KS3FS_ETAG_LEN];		/* "" = not uploaded yet */
	/*
	 * Parts uploaded from the page cache: frozen until completion.  The
	 * bitmap (and the ks3fs_inode's mpu pointer) change under ki->lock,
	 * so read paths without i_rwsem can check it.
	 */
	unsigned long *uploaded;
	loff_t base_size;			/* remote object at start */
	char base_etag[KS3FS_ETAG_LEN];
};

u64 ks3fs_max_object(struct ks3fs_sb_info *sbi)
{
	return min_t(u64, KS3FS_MAX_OBJECT,
		     sbi->part_size * KS3FS_MPU_MAX_PARTS);
}

static bool range_frozen(struct inode *inode, loff_t pos, loff_t len)
{
	struct ks3fs_inode *ki = KS3_I(inode);
	struct ks3fs_mpu *mpu;
	bool frozen = false;
	u64 k, last;

	if (len <= 0)
		return false;
	spin_lock(&ki->lock);
	mpu = ki->mpu;
	if (mpu && mpu->cap) {
		last = min_t(u64, div64_u64(pos + len - 1, mpu->part_size),
			     mpu->cap - 1);
		for (k = div64_u64(pos, mpu->part_size); k <= last && !frozen; k++)
			frozen = test_bit(k, mpu->uploaded);
	}
	spin_unlock(&ki->lock);
	return frozen;
}

static void freeze_part(struct inode *inode, int k)
{
	struct ks3fs_inode *ki = KS3_I(inode);

	spin_lock(&ki->lock);
	set_bit(k, ki->mpu->uploaded);
	spin_unlock(&ki->lock);
}

/* Detach the upload from the inode (readers see it under ki->lock). */
static struct ks3fs_mpu *mpu_detach(struct inode *inode)
{
	struct ks3fs_inode *ki = KS3_I(inode);
	struct ks3fs_mpu *mpu;

	spin_lock(&ki->lock);
	mpu = ki->mpu;
	ki->mpu = NULL;
	spin_unlock(&ki->lock);
	ki->stream_next = 0;
	return mpu;
}

struct put_body {
	struct inode *inode;
	loff_t start, end;
};

/*
 * Shared writable mmaps: before a folio is sent, write-protect its
 * mappings and clear PG_checked.  ->page_mkwrite sets PG_checked (and
 * PG_dirty) on the next store, so a folio still unchecked after the send
 * holds exactly what was stored and may be cleaned.
 */
static void protect_folio(struct address_space *mapping, struct folio *folio)
{
	if (!folio_mapped(folio) && !folio_test_checked(folio))
		return;
	folio_lock(folio);
	if (folio->mapping == mapping) {
		if (folio_mkclean(folio))
			folio_mark_dirty(folio);
		folio_clear_checked(folio);
	}
	folio_unlock(folio);
}

/* Send [start, end) of the file from the page cache. */
static int send_file_body(struct ks3fs_conn *conn, void *arg)
{
	struct put_body *pb = arg;
	struct address_space *mapping = pb->inode->i_mapping;
	loff_t pos = pb->start;
	int err = 0;

	while (pos < pb->end) {
		struct folio *folio;
		size_t off, n;

		if (fatal_signal_pending(current))
			return -EINTR;
		folio = read_mapping_folio(mapping, pos >> PAGE_SHIFT, NULL);
		if (IS_ERR(folio))
			return PTR_ERR(folio);
		protect_folio(mapping, folio);
		off = offset_in_folio(folio, pos);
		n = min_t(loff_t, folio_size(folio) - off, pb->end - pos);
		while (n && !err) {
			size_t chunk = min_t(size_t, n,
					     PAGE_SIZE - offset_in_page(off));
			char *p = kmap_local_folio(folio, off);

			err = ks3fs_http_send(conn, p, chunk);
			kunmap_local(p);
			off += chunk;
			pos += chunk;
			n -= chunk;
		}
		folio_put(folio);
		if (err)
			return err;
		cond_resched();
	}
	return 0;
}

/*
 * Mark the (now stored) folios in [start, end) clean and reclaimable,
 * except those stored to through an mmap since they were sent (or that
 * were never sent since: an uploaded part written through an mmap).
 * Returns whether any was kept dirty.
 */
static bool clean_folios(struct address_space *mapping, loff_t start,
			 loff_t end)
{
	pgoff_t idx, last;
	bool kept = false;

	if (end <= start)
		return false;
	last = (end - 1) >> PAGE_SHIFT;
	for (idx = start >> PAGE_SHIFT; idx <= last; idx++) {
		struct folio *folio = filemap_get_folio(mapping, idx);

		if (IS_ERR_OR_NULL(folio))
			continue;
		folio_lock(folio);
		if (folio->mapping == mapping) {
			if (folio_test_checked(folio))
				kept = true;
			else
				folio_clear_dirty(folio);
		}
		folio_unlock(folio);
		idx = folio->index + folio_nr_pages(folio) - 1;
		folio_put(folio);
		cond_resched();
	}
	return kept;
}

/* Does [start, end) hold data written since the object was stored? */
static bool range_has_dirty(struct address_space *mapping, loff_t start,
			    loff_t end)
{
	struct folio_batch fbatch;
	pgoff_t index = start >> PAGE_SHIFT, last = (end - 1) >> PAGE_SHIFT;
	bool dirty = false;
	unsigned int i, n;

	folio_batch_init(&fbatch);
	while (!dirty && (n = filemap_get_folios(mapping, &index, last, &fbatch))) {
		for (i = 0; i < n && !dirty; i++)
			dirty = folio_test_dirty(fbatch.folios[i]);
		folio_batch_release(&fbatch);
		cond_resched();
	}
	return dirty;
}

static void mpu_free(struct ks3fs_mpu *mpu)
{
	if (!mpu)
		return;
	kfree(mpu->upload_id);
	kfree(mpu->key);
	kvfree(mpu->etags);
	kvfree(mpu->uploaded);
	kfree(mpu);
}

/* Drop an open upload without contacting the store (inode teardown). */
void ks3fs_mpu_forget(struct inode *inode)
{
	struct ks3fs_mpu *mpu = mpu_detach(inode);

	if (mpu)
		pr_warn("ks3fs: dropping unfinished upload of '%s'\n", mpu->key);
	mpu_free(mpu);
}

/* Abort the open upload, if any (caller holds i_rwsem). */
void ks3fs_mpu_discard(struct inode *inode)
{
	struct ks3fs_mpu *mpu = mpu_detach(inode);

	if (!mpu)
		return;
	ks3fs_mpu_abort(KS3_SB(inode->i_sb), mpu->key, mpu->upload_id);
	mpu_free(mpu);
}

static int mpu_start(struct inode *inode)
{
	struct ks3fs_sb_info *sbi = KS3_SB(inode->i_sb);
	struct ks3fs_inode *ki = KS3_I(inode);
	struct ks3fs_mpu *mpu;
	struct ks3fs_meta meta;
	int err;

	mpu = kzalloc(sizeof(*mpu), GFP_KERNEL);
	if (!mpu)
		return -ENOMEM;
	mpu->part_size = sbi->part_size;
	mpu->key = ks3fs_inode_key(inode);
	if (!mpu->key) {
		mpu_free(mpu);
		return -ENOMEM;
	}
	mpu->base_size = test_bit(KS3_I_REMOTE, &ki->flags) ? ki->remote_size : 0;
	spin_lock(&ki->lock);
	strscpy(mpu->base_etag, ki->etag, sizeof(mpu->base_etag));
	spin_unlock(&ki->lock);

	err = ks3fs_inode_meta(inode, &meta);
	if (!err)
		err = ks3fs_mpu_create(sbi, mpu->key, &meta, &mpu->upload_id);
	ks3fs_meta_release(&meta);
	if (err) {
		mpu_free(mpu);
		return err;
	}
	clear_bit(KS3_I_META_DIRTY, &ki->flags);	/* carried by the upload */
	spin_lock(&ki->lock);
	ki->mpu = mpu;
	spin_unlock(&ki->lock);
	return 0;
}

static int mpu_grow(struct inode *inode, int nr)
{
	struct ks3fs_inode *ki = KS3_I(inode);
	struct ks3fs_mpu *mpu = ki->mpu;
	char (*n)[KS3FS_ETAG_LEN], (*old)[KS3FS_ETAG_LEN];
	unsigned long *bits, *oldbits;
	int cap;

	if (nr <= mpu->cap)
		return 0;
	if (nr > KS3FS_MPU_MAX_PARTS)
		return -EFBIG;
	cap = min(max(nr, mpu->cap * 2), KS3FS_MPU_MAX_PARTS);
	n = kvcalloc(cap, KS3FS_ETAG_LEN, GFP_KERNEL);
	bits = kvcalloc(BITS_TO_LONGS(cap), sizeof(long), GFP_KERNEL);
	if (!n || !bits) {
		kvfree(n);
		kvfree(bits);
		return -ENOMEM;
	}
	if (mpu->cap) {
		memcpy(n, mpu->etags, (size_t)mpu->cap * KS3FS_ETAG_LEN);
		bitmap_copy(bits, mpu->uploaded, mpu->cap);
	}
	spin_lock(&ki->lock);
	old = mpu->etags;
	oldbits = mpu->uploaded;
	mpu->etags = n;
	mpu->uploaded = bits;
	mpu->cap = cap;
	spin_unlock(&ki->lock);
	kvfree(old);
	kvfree(oldbits);
	return 0;
}

/* Upload part @k ([start, end)) from the page cache, then let it go. */
static int mpu_send_part(struct inode *inode, int k, loff_t start, loff_t end)
{
	struct ks3fs_mpu *mpu = KS3_I(inode)->mpu;
	struct put_body pb = { .inode = inode, .start = start, .end = end };
	int err;

	err = ks3fs_mpu_upload_part(KS3_SB(inode->i_sb), mpu->key,
				    mpu->upload_id, k + 1, end - start,
				    send_file_body, &pb, mpu->etags[k]);
	if (err)
		return err;
	freeze_part(inode, k);
	clean_folios(inode->i_mapping, start, end);
	return 0;
}

/*
 * Write path: stream out every completed, modified part behind @cursor.
 * Failures are not fatal (the data stays dirty in the page cache and is
 * retried at the next opportunity or on close), so this returns nothing.
 */
static void mpu_stream(struct inode *inode, loff_t cursor)
{
	struct ks3fs_sb_info *sbi = KS3_SB(inode->i_sb);
	struct ks3fs_inode *ki = KS3_I(inode);
	loff_t size = i_size_read(inode);
	u64 part = ki->mpu ? ki->mpu->part_size : sbi->part_size;
	int k, k_end = div64_u64(min(cursor, size), part);
	int err = 0;

	if (!ki->mpu && size < 2 * part)
		return;
	for (k = ki->stream_next; k < k_end && !err; k++) {
		loff_t start = (loff_t)k * part, end = start + part;

		if (ki->mpu && k < ki->mpu->cap && ki->mpu->etags[k][0])
			continue;
		if (!range_has_dirty(inode->i_mapping, start, end))
			continue;	/* untouched: copied server side at commit */
		if (!ki->mpu)
			err = mpu_start(inode);
		if (!err)
			err = mpu_grow(inode, k + 1);
		if (!err)
			err = mpu_send_part(inode, k, start, end);
		if (!err)
			ki->stream_next = k + 1;
	}
	if (err)
		pr_warn_ratelimited("ks3fs: streaming part of '%s' failed: %d (retried on close)\n",
				    ki->key, err);
}

/* Complete the upload: fill in every missing part, then commit. */
static int mpu_commit(struct inode *inode)
{
	struct ks3fs_sb_info *sbi = KS3_SB(inode->i_sb);
	struct ks3fs_inode *ki = KS3_I(inode);
	loff_t size = i_size_read(inode);
	char etag[KS3FS_ETAG_LEN];
	struct ks3fs_mpu *mpu;
	int k, nr, nr_copies = 0, err;
	int *copies;

	if (size > ks3fs_max_object(sbi))
		return -EFBIG;
	if (!ki->mpu) {
		err = mpu_start(inode);
		if (err)
			return err;
	}
	mpu = ki->mpu;
	nr = max_t(int, 1, DIV_ROUND_UP_ULL(size, mpu->part_size));
	err = mpu_grow(inode, nr);
	if (err)
		return err;

	copies = kvmalloc_array(nr, sizeof(*copies), GFP_KERNEL);
	if (!copies)
		return -ENOMEM;
	for (k = 0; k < nr; k++) {
		loff_t start = (loff_t)k * mpu->part_size;
		loff_t end = min_t(loff_t, start + mpu->part_size, size);

		if (mpu->etags[k][0])
			continue;
		if (end <= mpu->base_size && mpu->base_etag[0] &&
		    !range_has_dirty(inode->i_mapping, start, end)) {
			/* unchanged: the store copies it from the old object */
			copies[nr_copies++] = k;
			continue;
		}
		err = mpu_send_part(inode, k, start, end);
		if (err)
			goto out;
	}
	err = ks3fs_mpu_copy_parts(sbi, mpu->key, mpu->upload_id, mpu->key,
				   mpu->base_etag, mpu->part_size,
				   mpu->base_size, copies, nr_copies, mpu->etags);
out:
	kvfree(copies);
	if (err)
		return err;
	err = ks3fs_mpu_complete(sbi, mpu->key, mpu->upload_id, mpu->etags, nr,
				 etag);
	if (err)
		return err;

	spin_lock(&ki->lock);
	strscpy(ki->etag, etag, sizeof(ki->etag));
	spin_unlock(&ki->lock);
	if (!etag[0])
		ki->attr_time = 0;	/* refetch the ETag on next lookup */
	mpu_free(mpu_detach(inode));
	return 0;
}

/* One-shot PUT of the whole file. */
static int put_whole(struct inode *inode, loff_t size)
{
	struct ks3fs_inode *ki = KS3_I(inode);
	struct ks3fs_sb_info *sbi = KS3_SB(inode->i_sb);
	struct put_body pb = { .inode = inode, .start = 0, .end = size };
	struct ks3fs_req req = {
		.method = "PUT",
		.body_len = size,
		/* single pass over the page cache; integrity is TCP's job */
		.payload_sha256 = "UNSIGNED-PAYLOAD",
	};
	struct ks3fs_resp resp;
	struct ks3fs_conn *conn;
	struct ks3fs_meta meta;
	char *key;
	int err;

	key = ks3fs_inode_key(inode);
	if (!key)
		return -ENOMEM;
	req.key = key;
	/* the new object carries the current metadata too */
	err = ks3fs_inode_meta(inode, &meta);
	if (err)
		goto out;
	req.meta = &meta;
	clear_bit(KS3_I_META_DIRTY, &ki->flags);
	conn = ks3fs_http_start(sbi, &req, &resp, send_file_body, &pb, NULL);
	if (IS_ERR(conn)) {
		err = PTR_ERR(conn);
		goto out;
	}
	err = ks3fs_status_to_errno(resp.status);
	if (err) {
		pr_warn_ratelimited("ks3fs: PUT '%s' failed: HTTP %d\n", key,
				    resp.status);
	} else {
		spin_lock(&ki->lock);
		strscpy(ki->etag, resp.etag, sizeof(ki->etag));
		spin_unlock(&ki->lock);
	}
	ks3fs_http_finish(sbi, conn, &resp);
out:
	if (err && sbi->meta)
		set_bit(KS3_I_META_DIRTY, &ki->flags);
	ks3fs_meta_release(&meta);
	kfree(key);
	return err;
}

/* Store the file's current contents.  Caller holds the inode lock. */
int ks3fs_upload_locked(struct inode *inode)
{
	struct ks3fs_inode *ki = KS3_I(inode);
	struct ks3fs_sb_info *sbi = KS3_SB(inode->i_sb);
	loff_t size = i_size_read(inode);
	int err;

	if (!test_bit(KS3_I_DIRTY, &ki->flags))
		return 0;
	if (!inode->i_nlink) {	/* unlinked while open: nothing to store */
		ks3fs_mpu_discard(inode);
		clear_bit(KS3_I_DIRTY, &ki->flags);
		return 0;
	}
	if (ki->mpu || size >= 2 * sbi->part_size || size > KS3FS_MAX_PUT)
		err = mpu_commit(inode);
	else
		err = put_whole(inode, size);
	if (err)
		return err;

	ki->remote_size = size;
	ki->attr_time = jiffies;
	set_bit(KS3_I_REMOTE, &ki->flags);
	clear_bit(KS3_I_DIRTY, &ki->flags);
	/* stores through an mmap that missed this upload go with the next */
	if (clean_folios(inode->i_mapping, 0, size))
		set_bit(KS3_I_DIRTY, &ki->flags);
	/* metadata changed while a multipart upload was open */
	if (test_bit(KS3_I_META_DIRTY, &ki->flags))
		ks3fs_push_meta(inode);
	return 0;
}

int ks3fs_upload(struct inode *inode)
{
	int err;

	if (!test_bit(KS3_I_DIRTY, &KS3_I(inode)->flags))
		return 0;
	inode_lock(inode);
	err = ks3fs_upload_locked(inode);
	inode_unlock(inode);
	return err;
}

/* ---------- file operations ---------- */

static int ks3fs_flush(struct file *file, fl_owner_t id)
{
	if (!(file->f_mode & FMODE_WRITE))
		return 0;
	struct inode *inode = file_inode(file);

	return ks3fs_upload(inode) ?: ks3fs_push_meta(inode);
}

/* ---------- unlink while open ("silly rename", as NFS does) ---------- */

/*
 * An object unlinked (or renamed over) while it is open is moved to a
 * hidden key and deleted at the last close, so the open files can still
 * read what they have not cached.  Caller holds the inode lock.
 */
int ks3fs_orphan(struct inode *inode)
{
	struct ks3fs_sb_info *sbi = KS3_SB(inode->i_sb);
	struct ks3fs_inode *ki = KS3_I(inode);
	char etag[KS3FS_ETAG_LEN];
	char *key, *orphan;
	int err;

	if (!S_ISREG(inode->i_mode) || !atomic_read(&ki->opens) ||
	    !test_bit(KS3_I_REMOTE, &ki->flags) || !ki->remote_size ||
	    test_bit(KS3_I_ORPHAN, &ki->flags))
		return 0;
	/* parts of an open multipart upload exist nowhere else */
	if (ki->mpu) {
		err = ks3fs_upload_locked(inode);
		if (err)
			return err;
	}
	key = ks3fs_inode_key(inode);
	/* i_ino is unsigned long before 7.3 and u64 after */
	orphan = kasprintf(GFP_KERNEL, "%s%s/%llx-%llx", sbi->prefix,
			   KS3FS_ORPHANS, (unsigned long long)inode->i_ino,
			   ktime_get_real_ns());
	if (!key || !orphan) {
		err = -ENOMEM;
		goto out;
	}
	if (ki->remote_size > KS3FS_MAX_PUT)
		err = ks3fs_s3_copy_large(sbi, key, orphan, ki->remote_size,
					  NULL, etag);
	else
		err = ks3fs_s3_copy(sbi, key, orphan, etag);
	if (err)
		goto out;
	spin_lock(&ki->lock);
	swap(ki->key, orphan);
	if (etag[0])
		strscpy(ki->etag, etag, sizeof(ki->etag));
	spin_unlock(&ki->lock);
	set_bit(KS3_I_ORPHAN, &ki->flags);
out:
	kfree(key);
	kfree(orphan);
	return err;
}

void ks3fs_orphan_delete(struct inode *inode)
{
	char *key;

	if (!test_and_clear_bit(KS3_I_ORPHAN, &KS3_I(inode)->flags))
		return;
	key = ks3fs_inode_key(inode);
	if (key && ks3fs_s3_delete(KS3_SB(inode->i_sb), key))
		pr_warn("ks3fs: could not delete orphan '%s'\n", key);
	kfree(key);
}

static int ks3fs_open(struct inode *inode, struct file *file)
{
	int err = generic_file_open(inode, file);

	if (!err)
		atomic_inc(&KS3_I(inode)->opens);
	return err;
}

static int ks3fs_release(struct inode *inode, struct file *file)
{
	int err;

	if (atomic_dec_and_test(&KS3_I(inode)->opens))
		ks3fs_orphan_delete(inode);
	if (!(file->f_mode & FMODE_WRITE))
		return 0;
	err = ks3fs_upload(inode);
	if (err) {
		pr_warn_ratelimited("ks3fs: upload of '%s' on close failed: %d\n",
				    KS3_I(inode)->key, err);
		/* last chance: don't leave a multipart upload behind */
		inode_lock(inode);
		ks3fs_mpu_discard(inode);
		inode_unlock(inode);
	}
	return 0;
}

static int ks3fs_fsync(struct file *file, loff_t start, loff_t end,
		       int datasync)
{
	return ks3fs_upload(file_inode(file));
}

/*
 * First store to a folio through a shared mapping (and the first after
 * each upload write-protected it): the file needs uploading again.
 */
static vm_fault_t ks3fs_page_mkwrite(struct vm_fault *vmf)
{
	struct inode *inode = file_inode(vmf->vma->vm_file);
	struct folio *folio = page_folio(vmf->page);
	vm_fault_t ret = VM_FAULT_LOCKED;

	sb_start_pagefault(inode->i_sb);
	file_update_time(vmf->vma->vm_file);
	folio_lock(folio);
	if (folio->mapping != inode->i_mapping) {	/* truncated */
		folio_unlock(folio);
		ret = VM_FAULT_NOPAGE;
		goto out;
	}
	folio_mark_dirty(folio);
	folio_set_checked(folio);
	set_bit(KS3_I_DIRTY, &KS3_I(inode)->flags);
out:
	sb_end_pagefault(inode->i_sb);
	return ret;
}

static const struct vm_operations_struct ks3fs_vm_ops = {
	.fault		= filemap_fault,
	.map_pages	= filemap_map_pages,
	.page_mkwrite	= ks3fs_page_mkwrite,
};

static int ks3fs_mmap(struct file *file, struct vm_area_struct *vma)
{
	file_accessed(file);
	vma->vm_ops = &ks3fs_vm_ops;
	return 0;
}

static ssize_t ks3fs_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	ssize_t ret;

	if (iocb->ki_flags & IOCB_DIRECT)
		return -EINVAL;
	inode_lock(inode);
	ret = generic_write_checks(iocb, from);
	/* uploaded parts are frozen until the upload completes */
	if (ret > 0 && range_frozen(inode, iocb->ki_pos, ret)) {
		int err = ks3fs_upload_locked(inode);

		if (err)
			ret = err;
	}
	if (ret > 0)
		ret = __generic_file_write_iter(iocb, from);
	if (ret > 0)
		mpu_stream(inode, iocb->ki_pos);
	inode_unlock(inode);
	if (ret > 0)
		ret = generic_write_sync(iocb, ret);
	return ret;
}

/* Reads that reach a frozen range complete the upload first. */
static int thaw_for_read(struct inode *inode, loff_t pos, size_t len,
			 bool nowait)
{
	int err = 0;

	if (!range_frozen(inode, pos, len))
		return 0;
	if (nowait)
		return -EAGAIN;
	inode_lock(inode);
	if (range_frozen(inode, pos, len))
		err = ks3fs_upload_locked(inode);
	inode_unlock(inode);
	return err;
}

static ssize_t ks3fs_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
	int err = thaw_for_read(file_inode(iocb->ki_filp), iocb->ki_pos,
				iov_iter_count(to),
				iocb->ki_flags & IOCB_NOWAIT);

	return err ?: generic_file_read_iter(iocb, to);
}

static ssize_t ks3fs_splice_read(struct file *in, loff_t *ppos,
				 struct pipe_inode_info *pipe, size_t len,
				 unsigned int flags)
{
	int err = thaw_for_read(file_inode(in), *ppos, len, false);

	return err ?: filemap_splice_read(in, ppos, pipe, len, flags);
}

/* Write zeros over [pos, end) through the page cache, as write(2) would. */
static int zero_range(struct file *file, loff_t pos, loff_t end)
{
	struct inode *inode = file_inode(file);
	struct bio_vec bv[32];
	struct kiocb kiocb;
	int i;

	for (i = 0; i < ARRAY_SIZE(bv); i++)
		bvec_set_page(&bv[i], ZERO_PAGE(0), PAGE_SIZE, 0);
	init_sync_kiocb(&kiocb, file);
	kiocb.ki_pos = pos;
	while (kiocb.ki_pos < end) {
		size_t n = min_t(loff_t, end - kiocb.ki_pos,
				 ARRAY_SIZE(bv) * PAGE_SIZE);
		struct iov_iter it;
		ssize_t ret;

		iov_iter_bvec(&it, ITER_SOURCE, bv, DIV_ROUND_UP(n, PAGE_SIZE),
			      n);
		ret = generic_perform_write(&kiocb, &it);
		if (ret < 0)
			return ret;
		/* like write(2), a large range streams out completed parts */
		mpu_stream(inode, kiocb.ki_pos);
		cond_resched();
	}
	return 0;
}

/*
 * Objects have no allocation to reserve, so plain fallocate only extends
 * the size (the new range reads as zeros without storing anything until
 * the next upload).  Punching or zeroing a range writes zeros over it in
 * the page cache.  Collapsing or inserting would rewrite everything
 * after the range and is not offered.
 */
static long ks3fs_fallocate(struct file *file, int mode, loff_t offset,
			    loff_t len)
{
	struct inode *inode = file_inode(file);
	struct ks3fs_inode *ki = KS3_I(inode);
	loff_t end = offset + len, size;
	int err;

	if (mode & ~(FALLOC_FL_KEEP_SIZE | FALLOC_FL_PUNCH_HOLE |
		     FALLOC_FL_ZERO_RANGE))
		return -EOPNOTSUPP;
	/* no object can be that large: as good as out of space */
	if (!(mode & FALLOC_FL_KEEP_SIZE) &&
	    end > ks3fs_max_object(KS3_SB(inode->i_sb)))
		return -ENOSPC;

	inode_lock(inode);
	size = i_size_read(inode);
	/* RLIMIT_FSIZE (with SIGXFSZ), as for a write */
	if (!(mode & FALLOC_FL_KEEP_SIZE) && end > size) {
		err = inode_newsize_ok(inode, end);
		if (err)
			goto out;
	}
	/* drops setuid/setgid even when there is nothing to do (as ext4) */
	err = file_modified(file);
	if (err)
		goto out;
	if (mode == FALLOC_FL_KEEP_SIZE || (!mode && end <= size))
		goto out;	/* nothing to reserve */
	/* uploaded parts are frozen until the upload completes */
	if (range_frozen(inode, offset, min(end, size) - offset)) {
		err = ks3fs_upload_locked(inode);
		if (err)
			goto out;
	}
	if (mode & (FALLOC_FL_PUNCH_HOLE | FALLOC_FL_ZERO_RANGE)) {
		err = zero_range(file, offset, min(end, size));
		if (err)
			goto out;
	}
	if (!(mode & FALLOC_FL_KEEP_SIZE) && end > size) {
		/* bytes past the old EOF in its folio must read back as zeros */
		err = zero_range(file, size,
				 min_t(loff_t, end, round_up(size, PAGE_SIZE)));
		if (err)
			goto out;
		truncate_setsize(inode, end);
		inode->i_blocks = DIV_ROUND_UP_ULL(end, 512);
		set_bit(KS3_I_DIRTY, &ki->flags);
	}
out:
	inode_unlock(inode);
	return err;
}

int ks3fs_setattr(struct mnt_idmap *idmap, struct dentry *dentry,
		  struct iattr *attr)
{
	struct inode *inode = d_inode(dentry);
	struct ks3fs_sb_info *sbi = KS3_SB(inode->i_sb);
	struct ks3fs_inode *ki = KS3_I(inode);
	unsigned int meta_attrs = ATTR_MODE | ATTR_UID | ATTR_GID |
				  ATTR_ATIME | ATTR_MTIME;
	int err;

	if (!sbi->meta) {
		/* ownership and permissions are mount-wide, nothing to store */
		if ((attr->ia_valid & ATTR_MODE) &&
		    (attr->ia_mode & 07777) != (inode->i_mode & 07777))
			return -EPERM;
		if ((attr->ia_valid & ATTR_UID) &&
		    !uid_eq(attr->ia_uid, inode->i_uid))
			return -EPERM;
		if ((attr->ia_valid & ATTR_GID) &&
		    !gid_eq(attr->ia_gid, inode->i_gid))
			return -EPERM;
		attr->ia_valid &= ~(ATTR_MODE | ATTR_UID | ATTR_GID);
	}

	err = setattr_prepare(idmap, dentry, attr);
	if (err)
		return err;

	if (attr->ia_valid & ATTR_SIZE) {
		if (S_ISDIR(inode->i_mode))
			return -EISDIR;
		if (attr->ia_size > ks3fs_max_object(sbi))
			return -EFBIG;
		/* truncating around frozen parts: finish that upload first */
		if (ki->mpu) {
			err = ks3fs_upload_locked(inode);
			if (err)
				return err;
		}
		if (attr->ia_size != i_size_read(inode)) {
			/*
			 * truncate(2) on a path passes no times, but a size
			 * change must update mtime and ctime (as ext4 does)
			 */
			if (!(attr->ia_valid & ATTR_MTIME)) {
				attr->ia_mtime = attr->ia_ctime = current_time(inode);
				attr->ia_valid |= ATTR_MTIME | ATTR_CTIME;
			}
			truncate_setsize(inode, attr->ia_size);
			inode->i_blocks = DIV_ROUND_UP_ULL(attr->ia_size, 512);
			/* bytes past a shrink must read back as zeros if regrown */
			if (ki->remote_size > attr->ia_size)
				ki->remote_size = attr->ia_size;
			set_bit(KS3_I_DIRTY, &ki->flags);
		} else if (attr->ia_size == 0 && ki->remote_size) {
			/* O_TRUNC of an empty-looking file: still rewrite */
			set_bit(KS3_I_DIRTY, &ki->flags);
		}
	}
	setattr_copy(idmap, inode, attr);
	if (attr->ia_valid & ATTR_MODE) {
		/* the ACL's group class follows the mode (no-op without ACLs) */
		err = posix_acl_chmod(idmap, dentry, inode->i_mode);
		if (err)
			return err;
	}

	/*
	 * Metadata-only changes are stored by ->write_inode (or with the
	 * next upload), so chmod + utimes cost one CopyObject, not two.
	 */
	if (sbi->meta && (attr->ia_valid & meta_attrs)) {
		set_bit(KS3_I_META_DIRTY, &ki->flags);
		ks3fs_schedule_meta_writeback(inode);
	}
	return 0;
}

int ks3fs_getattr(struct mnt_idmap *idmap, const struct path *path,
		  struct kstat *stat, u32 mask, unsigned int flags)
{
	struct inode *inode = d_inode(path->dentry);

	generic_fillattr(idmap, mask, inode, stat);
	stat->blksize = 1 << 20;	/* encourage large I/O */
	return 0;
}

const struct inode_operations ks3fs_symlink_iops = {
	.get_link	= page_get_link,
	.setattr	= ks3fs_setattr,
	.getattr	= ks3fs_getattr,
	.listxattr	= ks3fs_listxattr,
};

const struct inode_operations ks3fs_file_iops = {
	.setattr	= ks3fs_setattr,
	.getattr	= ks3fs_getattr,
	.listxattr	= ks3fs_listxattr,
	.get_inode_acl	= ks3fs_get_acl,
	.set_acl	= ks3fs_set_acl,
};

const struct file_operations ks3fs_file_fops = {
	.llseek		= generic_file_llseek,
	.read_iter	= ks3fs_read_iter,
	.write_iter	= ks3fs_write_iter,
	.mmap		= ks3fs_mmap,
	.open		= ks3fs_open,
	.flush		= ks3fs_flush,
	.release	= ks3fs_release,
	.fsync		= ks3fs_fsync,
	.splice_read	= ks3fs_splice_read,
	.splice_write	= iter_file_splice_write,
	.fallocate	= ks3fs_fallocate,
};
