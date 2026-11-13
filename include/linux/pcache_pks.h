/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PCACHE_PKS_H
#define _LINUX_PCACHE_PKS_H

#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/pcache_pks_page.h>
#include <linux/percpu.h>
#include <linux/pks.h>
#include <linux/pks-keys.h>
#include <linux/sched.h>
#include <linux/timex.h>

struct pt_regs;

/*
 * Write scopes
 * ------------
 * A write scope makes the write alias of protected pages writable for the
 * current thread, from pcache_pks_scope_begin_*() to pcache_pks_scope_end().
 * Opening one is a grant. Every grant is made at a site of the list below,
 * names the object that justifies it, and is refused when the condition of its
 * kind does not hold at that moment:
 *
 *   file    The thread writes file content on behalf of an open file. The file
 *           must be open for writing, and its inode neither immutable nor an
 *           active swap file. These are the tests the VFS write entry points
 *           make; the grant repeats them at the place where the store happens,
 *           so it does not depend on which path led there.
 *   inode   The thread changes the file's size. It must hold the inode lock.
 *   page    The thread zeroes part of one page as part of page-cache
 *           maintenance, which does not change what a reader of the file sees.
 *           The page must be locked.
 *   fill    The thread fills pages that are being read in. Each page must be
 *           locked and not yet up to date.
 *
 * A refused grant is counted and reported, and the caller fails its operation
 * or goes on without access. This enum is the complete list of places that can
 * make protected pages writable.
 */
enum pcache_pks_site {
	PKS_SITE_BUFFERED_WRITE,	/* file:  generic_perform_write() */
	PKS_SITE_FALLOCATE,		/* file:  vfs_fallocate() */
	PKS_SITE_MKWRITE,		/* file:  ->page_mkwrite() */
	PKS_SITE_LOOP_FALLOCATE,	/* file:  loop discard and write-zeroes */
	PKS_SITE_TRUNCATE,		/* inode: notify_change() with ATTR_SIZE */
	PKS_SITE_ORPHAN,		/* inode: orphan truncation at mount */
	PKS_SITE_WRITEBACK_ZERO,	/* page:  tail of the last page at writeback */
	PKS_SITE_READ_ZERO,		/* fill:  holes of a page being read in */
	PKS_SITE_LOOP_FILL,		/* fill:  loop read request */
	PKS_NR_SITES,
};

struct pcache_pks_scope {
	u8 old;
	bool active;
#ifdef CONFIG_PCACHE_PKS_DEBUG
	u8 site;		/* PKS_NR_SITES for a clamp */
	u64 start;
#endif
};

#ifdef CONFIG_PCACHE_PKS

struct folio *pcache_pks_alloc_folio(gfp_t gfp, unsigned int order);
void pcache_pks_recycle_page(struct page *page);
bool pcache_pks_fault_callback(struct pt_regs *regs, unsigned long address,
			       bool write);

/* Does @mapping keep its page cache in protected pages? */
static inline bool pcache_pks_mapping(const struct address_space *mapping)
{
	return pcache_pks_is_enabled() &&
	       mapping && mapping_pks_protected(mapping);
}

DECLARE_PER_CPU(unsigned long [PKS_NR_SITES], pcache_pks_scope_count);
void pcache_pks_grant_refused(enum pcache_pks_site site);

#ifdef CONFIG_PCACHE_PKS_DEBUG
void pcache_pks_check_user_return(void);
void pcache_pks_window_account(enum pcache_pks_site site, u64 cycles);

/* Debug kernels measure how long each scope keeps write access open. */
static inline void pcache_pks_window_open(struct pcache_pks_scope *scope,
					  unsigned int site)
{
	scope->site = site;
	if (site < PKS_NR_SITES)
		scope->start = get_cycles();
}

static inline void pcache_pks_window_close(struct pcache_pks_scope *scope)
{
	if (scope->site < PKS_NR_SITES)
		pcache_pks_window_account(scope->site,
					  get_cycles() - scope->start);
}
#else
static inline void pcache_pks_check_user_return(void) {}
static inline void pcache_pks_window_open(struct pcache_pks_scope *scope,
					  unsigned int site)
{
}
static inline void pcache_pks_window_close(struct pcache_pks_scope *scope) {}
#endif

static inline void __pcache_pks_scope_open(struct pcache_pks_scope *scope,
					   enum pcache_pks_site site)
{
	this_cpu_inc(pcache_pks_scope_count[site]);
	scope->active = true;
	scope->old = pks_update_protection(PKS_KEY_PAGE_CACHE_W,
					   PKEY_READ_WRITE);
	pcache_pks_window_open(scope, site);
}

/**
 * pcache_pks_scope_begin_file - grant write access on behalf of an open file
 * @scope: state for pcache_pks_scope_end()
 * @site: the grant site
 * @file: the file whose content the caller is about to change
 *
 * Return: 0 if access was granted or @file is not protected. -EBADF, -EPERM or
 * -ETXTBSY if @file is protected and may not be written; the caller must then
 * fail the operation.
 */
static inline int pcache_pks_scope_begin_file(struct pcache_pks_scope *scope,
					      enum pcache_pks_site site,
					      const struct file *file)
{
	const struct inode *inode;
	int err = 0;

	scope->active = false;
	if (likely(!pcache_pks_mapping(file->f_mapping)))
		return 0;

	inode = file->f_mapping->host;
	if (unlikely(!(file->f_mode & FMODE_WRITE)))
		err = -EBADF;
	else if (unlikely(IS_IMMUTABLE(inode)))
		err = -EPERM;
	else if (unlikely(IS_SWAPFILE(inode)))
		err = -ETXTBSY;
	if (unlikely(err)) {
		pcache_pks_grant_refused(site);
		return err;
	}
	__pcache_pks_scope_open(scope, site);
	return 0;
}

/**
 * pcache_pks_scope_begin_inode - grant write access for a change of file size
 * @scope: state for pcache_pks_scope_end()
 * @site: the grant site
 * @inode: the inode being truncated or extended; the caller holds its lock
 *
 * Return: 0 if access was granted or @inode is not protected, -EPERM if the
 * inode lock is not held; the caller must then fail the operation.
 */
static inline int pcache_pks_scope_begin_inode(struct pcache_pks_scope *scope,
					       enum pcache_pks_site site,
					       struct inode *inode)
{
	scope->active = false;
	if (likely(!pcache_pks_mapping(inode->i_mapping)))
		return 0;

	if (unlikely(!inode_is_locked(inode))) {
		pcache_pks_grant_refused(site);
		return -EPERM;
	}
	lockdep_assert_held_write(&inode->i_rwsem);
	__pcache_pks_scope_open(scope, site);
	return 0;
}

/**
 * pcache_pks_scope_begin_page - grant write access for page-cache maintenance
 * @scope: state for pcache_pks_scope_end()
 * @site: the grant site
 * @page: the page the caller is about to zero part of; it must be locked
 *
 * If @page is protected but not locked, the grant is refused and the caller's
 * store will fault.
 */
static inline void pcache_pks_scope_begin_page(struct pcache_pks_scope *scope,
					       enum pcache_pks_site site,
					       struct page *page)
{
	scope->active = false;
	if (likely(!pcache_pks_page(page)))
		return;

	if (unlikely(!PageLocked(page))) {
		pcache_pks_grant_refused(site);
		return;
	}
	__pcache_pks_scope_open(scope, site);
}

/* May a fill store into @page? It must be locked and not yet up to date. */
static inline bool pcache_pks_page_fillable(struct page *page)
{
	return PageLocked(page) && !PageUptodate(page);
}

/**
 * pcache_pks_scope_begin_fill_pages - grant write access to fill several pages
 * @scope: state for pcache_pks_scope_end()
 * @site: the grant site
 * @nr: number of protected pages the fill will store into
 * @nr_fillable: how many of them pcache_pks_page_fillable() accepted
 *
 * For callers that fill a set of pages with one operation, such as a block
 * driver serving a read request. The caller counts; access is granted only if
 * every protected page of the set is fillable.
 */
static inline void
pcache_pks_scope_begin_fill_pages(struct pcache_pks_scope *scope,
				  enum pcache_pks_site site,
				  unsigned int nr, unsigned int nr_fillable)
{
	scope->active = false;
	if (likely(!nr))
		return;

	if (unlikely(nr_fillable != nr)) {
		pcache_pks_grant_refused(site);
		return;
	}
	__pcache_pks_scope_open(scope, site);
}

/**
 * pcache_pks_scope_begin_fill - grant write access to fill one page
 * @scope: state for pcache_pks_scope_end()
 * @site: the grant site
 * @page: a page being read in; it must be locked and not yet up to date
 */
static inline void pcache_pks_scope_begin_fill(struct pcache_pks_scope *scope,
					       enum pcache_pks_site site,
					       struct page *page)
{
	scope->active = false;
	if (likely(!pcache_pks_page(page)))
		return;
	pcache_pks_scope_begin_fill_pages(scope, site, 1,
					  pcache_pks_page_fillable(page));
}

/*
 * Take write access away for a stretch of code that runs on top of whatever
 * the thread was doing: a softirq, memory reclaim, compaction. Such code must
 * not inherit a scope that the interrupted or calling path has open.
 *
 * Exception entry and exit keep current->thread.pkrs equal to IA32_PKRS, so the
 * common case, no scope open, is recognized from memory and costs no register
 * access. Only a key that currently permits writes is changed, so an
 * access-disabled key is never relaxed to write-disabled.
 */
static inline void pcache_pks_scope_clamp(struct pcache_pks_scope *scope)
{
	scope->active = pcache_pks_is_enabled() &&
			!((READ_ONCE(current->thread.pkrs) >>
			   PKR_PKEY_SHIFT(PKS_KEY_PAGE_CACHE_W)) &
			  (PKEY_DISABLE_ACCESS | PKEY_DISABLE_WRITE));
	if (unlikely(scope->active)) {
		scope->old = pks_update_protection(PKS_KEY_PAGE_CACHE_W,
						   PKEY_DISABLE_WRITE);
		pcache_pks_window_open(scope, PKS_NR_SITES);
	}
}

/* Close a scope or a clamp: restore what was in effect before it. */
static inline void pcache_pks_scope_end(struct pcache_pks_scope *scope)
{
	if (unlikely(scope->active)) {
		pks_update_protection(PKS_KEY_PAGE_CACHE_W, scope->old);
		pcache_pks_window_close(scope);
	}
}

#else /* !CONFIG_PCACHE_PKS */

static inline struct folio *pcache_pks_alloc_folio(gfp_t gfp,
						   unsigned int order)
{
	return NULL;
}
static inline void pcache_pks_recycle_page(struct page *page) {}
static inline bool pcache_pks_mapping(const struct address_space *mapping)
{
	return false;
}
static inline int pcache_pks_scope_begin_file(struct pcache_pks_scope *scope,
					      enum pcache_pks_site site,
					      const struct file *file)
{
	return 0;
}
static inline int pcache_pks_scope_begin_inode(struct pcache_pks_scope *scope,
					       enum pcache_pks_site site,
					       struct inode *inode)
{
	return 0;
}
static inline void pcache_pks_scope_begin_page(struct pcache_pks_scope *scope,
					       enum pcache_pks_site site,
					       struct page *page)
{
}
static inline bool pcache_pks_page_fillable(struct page *page)
{
	return false;
}
static inline void
pcache_pks_scope_begin_fill_pages(struct pcache_pks_scope *scope,
				  enum pcache_pks_site site,
				  unsigned int nr, unsigned int nr_fillable)
{
}
static inline void pcache_pks_scope_begin_fill(struct pcache_pks_scope *scope,
					       enum pcache_pks_site site,
					       struct page *page)
{
}
static inline void pcache_pks_scope_clamp(struct pcache_pks_scope *scope) {}
static inline void pcache_pks_scope_end(struct pcache_pks_scope *scope) {}
static inline void pcache_pks_check_user_return(void) {}

#endif /* CONFIG_PCACHE_PKS */

static inline bool pcache_pks_file(const struct file *file)
{
	return pcache_pks_mapping(file->f_mapping);
}

/*
 * Admission rule: a protected mapping holds only protected folios, and a
 * protected folio belongs only to a protected mapping.
 */
static inline bool pcache_pks_folio_fits(const struct address_space *mapping,
					 struct folio *folio)
{
	return pcache_pks_page(&folio->page) == pcache_pks_mapping(mapping);
}

#endif /* _LINUX_PCACHE_PKS_H */
