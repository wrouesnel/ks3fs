// SPDX-License-Identifier: GPL-2.0
/*
 * Run many independent S3 requests concurrently (directory rename copies,
 * attribute prefetch).  fn(ctx, i) is called for every i < nr from up to
 * sbi->parallel workers on an unbound workqueue.
 *
 * The run is refcounted so that a caller killed while waiting can return at
 * once: the workers stop taking new items and the last reference frees
 * @ctx through @release.  The first error stops the run and is returned.
 */
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/kref.h>
#include <linux/completion.h>
#include <linux/workqueue.h>
#include <linux/sched/signal.h>

#include "ks3fs.h"

struct par_run {
	struct kref ref;
	int (*fn)(void *ctx, int i);
	void (*release)(void *ctx);
	void *ctx;
	int nr;
	atomic_t next;
	atomic_t err;
	atomic_t running;
	bool abort;
	struct completion done;
	struct par_worker {
		struct work_struct work;
		struct par_run *run;
	} workers[];
};

static void par_free(struct kref *ref)
{
	struct par_run *run = container_of(ref, struct par_run, ref);

	if (run->release)
		run->release(run->ctx);
	kfree(run);
}

static void par_work(struct work_struct *work)
{
	struct par_run *run = container_of(work, struct par_worker, work)->run;
	int i;

	while (!READ_ONCE(run->abort) && !atomic_read(&run->err) &&
	       (i = atomic_inc_return(&run->next) - 1) < run->nr) {
		int err = run->fn(run->ctx, i);

		if (err)
			atomic_cmpxchg(&run->err, 0, err);
		cond_resched();
	}
	if (atomic_dec_and_test(&run->running))
		complete(&run->done);
	kref_put(&run->ref, par_free);
}

/*
 * Returns 0, the first error from @fn, or -EINTR if the caller was killed
 * (in which case @release frees @ctx later, once the workers are done).
 * @release is always called exactly once.
 */
int ks3fs_parallel(struct ks3fs_sb_info *sbi, int nr,
		   int (*fn)(void *ctx, int i), void *ctx,
		   void (*release)(void *ctx))
{
	int nw = clamp_t(int, nr, 1, sbi->parallel);
	struct par_run *run;
	int i, err;

	run = kzalloc(struct_size(run, workers, nw), GFP_KERNEL);
	if (!run) {
		if (release)
			release(ctx);
		return -ENOMEM;
	}
	kref_init(&run->ref);
	run->fn = fn;
	run->release = release;
	run->ctx = ctx;
	run->nr = nr;
	atomic_set(&run->running, nw);
	init_completion(&run->done);
	for (i = 0; i < nw; i++) {
		kref_get(&run->ref);
		run->workers[i].run = run;
		INIT_WORK(&run->workers[i].work, par_work);
		queue_work(system_unbound_wq, &run->workers[i].work);
	}

	if (wait_for_completion_killable(&run->done)) {
		WRITE_ONCE(run->abort, true);
		err = -EINTR;
	} else {
		err = atomic_read(&run->err);
	}
	kref_put(&run->ref, par_free);
	return err;
}
