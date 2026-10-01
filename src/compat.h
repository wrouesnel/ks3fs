/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Compatibility shims for the kernel versions ks3fs is built against.
 * The baseline is Ubuntu Noble's 6.8 kernel; newer HWE/mainline kernels
 * changed a handful of VFS signatures which are papered over here.
 */
#ifndef _KS3FS_COMPAT_H
#define _KS3FS_COMPAT_H

#include <linux/version.h>

/* ->write_begin/->write_end: page (<6.12), folio (6.12+), kiocb (6.17+) */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 17, 0)
#define KS3_WB_CTX		const struct kiocb *iocb
#else
#define KS3_WB_CTX		struct file *file
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
#define KS3_WB_FOLIO_ARG	struct folio **foliop
#define KS3_WB_SET_FOLIO(f)	(*foliop = (f))
#define KS3_WE_FOLIO_ARG	struct folio *folio
#define KS3_WE_GET_FOLIO()	(folio)
#else
#define KS3_WB_FOLIO_ARG	struct page **pagep
#define KS3_WB_SET_FOLIO(f)	(*pagep = &(f)->page)
#define KS3_WE_FOLIO_ARG	struct page *page
#define KS3_WE_GET_FOLIO()	page_folio(page)
#endif

/* ->d_revalidate gained the parent inode and name in 6.14 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 14, 0)
#define KS3_REVALIDATE_ARGS	struct inode *dir, const struct qstr *name, \
				struct dentry *dentry, unsigned int flags
#else
#define KS3_REVALIDATE_ARGS	struct dentry *dentry, unsigned int flags
#endif

/* ->mkdir returns a dentry since 6.15 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 15, 0)
#define KS3_MKDIR_RET		struct dentry *
#define KS3_MKDIR_RETURN(err)	ERR_PTR(err)
#else
#define KS3_MKDIR_RET		int
#define KS3_MKDIR_RETURN(err)	(err)
#endif

/* sb->s_d_op became private behind set_default_d_op() in 6.17 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 17, 0)
#define ks3_set_d_op(sb, ops)	set_default_d_op((sb), (ops))
#else
#define ks3_set_d_op(sb, ops)	((sb)->s_d_op = (ops))
#endif

/* kernel_connect() takes struct sockaddr_unsized since 6.19 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 19, 0)
#define KS3_CONNECT_ADDR(a)	((struct sockaddr_unsized *)(a))
#else
#define KS3_CONNECT_ADDR(a)	((struct sockaddr *)(a))
#endif

/* d_alloc_parallel() manages its own wait queue since 7.2 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 2, 0)
#define KS3_DALLOC_WQ(wq)
#define ks3_d_alloc_parallel(parent, name, wq)	d_alloc_parallel(parent, name)
#else
#define KS3_DALLOC_WQ(wq)	DECLARE_WAIT_QUEUE_HEAD_ONSTACK(wq)
#define ks3_d_alloc_parallel(parent, name, wq)	d_alloc_parallel(parent, name, &(wq))
#endif

/* ->create lost its "excl" argument in 7.3 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 3, 0)
#define KS3_CREATE_EXCL
#else
#define KS3_CREATE_EXCL		, bool excl
#endif

#define secs_to_jiffies_compat(s)	msecs_to_jiffies((s) * 1000U)

#endif /* _KS3FS_COMPAT_H */
