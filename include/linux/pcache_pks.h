/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PCACHE_PKS_H
#define _LINUX_PCACHE_PKS_H

#include <linux/jump_label.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/pks.h>
#include <linux/pks-keys.h>
#include <linux/sched.h>

struct pks_block;
struct pt_regs;

struct pcache_pks_scope {
	u8 old;
	bool active;
};

#ifdef CONFIG_PCACHE_PKS

/*
 * A protected page-cache page lives inside a 2 MiB (PMD) block whose direct-map
 * leaf is tagged PKS_KEY_PAGE_CACHE. Blocks are grown from and released to the
 * buddy allocator on demand; membership is a table indexed by block number.
 */
#define PKS_BLOCK_ORDER		(PMD_SHIFT - PAGE_SHIFT)
#define PKS_BLOCK_PAGES		(1UL << PKS_BLOCK_ORDER)

DECLARE_STATIC_KEY_FALSE(pcache_pks_enabled);
extern struct pks_block **pcache_pks_block_map;
extern unsigned long pcache_pks_nr_blocks_max;

struct folio *pcache_pks_alloc_folio(gfp_t gfp, unsigned int order);
void pcache_pks_recycle_page(struct page *page);
bool pcache_pks_fault_callback(struct pt_regs *regs, unsigned long address,
			       bool write);

/* Is the PKS page-cache pool active? */
static inline bool pcache_pks_is_enabled(void)
{
	return static_branch_unlikely(&pcache_pks_enabled);
}

/*
 * Is @page a PKS pool page? O(1) and lockless: the block map entry is published
 * with release semantics only after the whole block is tagged and TLB-flushed,
 * and cleared before a block is untagged, so a non-NULL entry means every page
 * of that block currently carries PKS_KEY_PAGE_CACHE.
 */
static inline bool pcache_pks_page(const struct page *page)
{
	unsigned long idx;

	if (!pcache_pks_is_enabled())
		return false;
	idx = page_to_pfn(page) >> PKS_BLOCK_ORDER;
	return idx < pcache_pks_nr_blocks_max &&
	       READ_ONCE(pcache_pks_block_map[idx]) != NULL;
}

/* Does @mapping keep its page cache in the PKS pool? */
static inline bool pcache_pks_mapping(const struct address_space *mapping)
{
	return pcache_pks_is_enabled() &&
	       mapping && mapping_pks_protected(mapping);
}

#ifdef CONFIG_PCACHE_PKS_DEBUG
extern atomic64_t pcache_pks_scope_count;
static inline void pcache_pks_count_scope(void)
{
	atomic64_inc(&pcache_pks_scope_count);
}
void pcache_pks_check_user_return(void);
#else
static inline void pcache_pks_count_scope(void) {}
static inline void pcache_pks_check_user_return(void) {}
#endif

/*
 * Open a PKS page-cache write scope on the current task if @mapping is
 * protected. Nesting-safe via the saved previous protection value.
 */
static inline void pcache_pks_scope_begin(struct pcache_pks_scope *scope,
					  const struct address_space *mapping)
{
	scope->active = pcache_pks_mapping(mapping);
	if (unlikely(scope->active)) {
		pcache_pks_count_scope();
		scope->old = pks_update_protection(PKS_KEY_PAGE_CACHE, 0);
	}
}

/*
 * Uncounted raw write scope for internal block-fill paths that operate on
 * locked non-uptodate pool pages without a direct @mapping pointer.
 */
static inline void pcache_pks_scope_begin_raw(struct pcache_pks_scope *scope,
					      bool cond)
{
	scope->active = pcache_pks_is_enabled() && cond;
	if (unlikely(scope->active))
		scope->old = pks_update_protection(PKS_KEY_PAGE_CACHE, 0);
}

/*
 * Clamp PKS_KEY_PAGE_CACHE back to write-disabled across asynchronous or
 * memory-reclaim execution, restoring the caller's state on exit.
 *
 * pks_save_pt_regs() and pks_restore_pt_regs() keep current->thread.pkrs
 * coherent with IA32_PKRS across interrupts and exceptions, so the common case,
 * a softirq, reclaim or compaction entry with no write scope open, is
 * recognized inline and skips both out-of-line WRMSR calls and the preempt
 * toggles they carry. Only a key that currently permits writes is clamped, so
 * an access-disabled key is never relaxed to write-disabled.
 */
static inline void pcache_pks_scope_clamp(struct pcache_pks_scope *scope)
{
	scope->active = pcache_pks_is_enabled() &&
			!((READ_ONCE(current->thread.pkrs) >>
			   PKR_PKEY_SHIFT(PKS_KEY_PAGE_CACHE)) &
			  (PKEY_DISABLE_ACCESS | PKEY_DISABLE_WRITE));
	if (unlikely(scope->active))
		scope->old = pks_update_protection(PKS_KEY_PAGE_CACHE,
						   PKEY_DISABLE_WRITE);
}

static inline void pcache_pks_scope_end(struct pcache_pks_scope *scope)
{
	if (unlikely(scope->active))
		pks_update_protection(PKS_KEY_PAGE_CACHE, scope->old);
}

#else /* !CONFIG_PCACHE_PKS */

static inline struct folio *pcache_pks_alloc_folio(gfp_t gfp,
						   unsigned int order)
{
	return NULL;
}
static inline void pcache_pks_recycle_page(struct page *page) {}
static inline bool pcache_pks_is_enabled(void)
{
	return false;
}
static inline bool pcache_pks_page(const struct page *page)
{
	return false;
}
static inline bool pcache_pks_mapping(const struct address_space *mapping)
{
	return false;
}
static inline void pcache_pks_scope_begin(struct pcache_pks_scope *scope,
					  const struct address_space *mapping)
{
}
static inline void pcache_pks_scope_begin_raw(struct pcache_pks_scope *scope,
					      bool cond)
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
 * Protected mappings hold only pool folios, and pool folios belong only to
 * protected mappings.
 */
static inline bool pcache_pks_folio_fits(const struct address_space *mapping,
					 struct folio *folio)
{
	return pcache_pks_page(&folio->page) == pcache_pks_mapping(mapping);
}

#endif /* _LINUX_PCACHE_PKS_H */
