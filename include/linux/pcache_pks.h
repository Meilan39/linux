/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PCACHE_PKS_H
#define _LINUX_PCACHE_PKS_H

#include <linux/jump_label.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/pks.h>
#include <linux/pks-keys.h>

struct pt_regs;

struct pcache_pks_scope {
	u8 old;
	bool active;
};

#ifdef CONFIG_PCACHE_PKS

DECLARE_STATIC_KEY_FALSE(pcache_pks_enabled);
extern unsigned long pcache_pks_start_pfn;
extern unsigned long pcache_pks_end_pfn;

void pcache_pks_pool_reserve(void);
struct folio *pcache_pks_alloc_folio(gfp_t gfp, unsigned int order);
void pcache_pks_recycle_page(struct page *page);
bool pcache_pks_fault_callback(struct pt_regs *regs, unsigned long address,
			       bool write);

/* Is the PKS page-cache pool active? */
static inline bool pcache_pks_is_enabled(void)
{
	return static_branch_unlikely(&pcache_pks_enabled);
}

/* Is @page part of the PKS page-cache pool? */
static inline bool pcache_pks_page(struct page *page)
{
	unsigned long pfn;

	if (!pcache_pks_is_enabled())
		return false;
	pfn = page_to_pfn(page);
	return pfn >= pcache_pks_start_pfn && pfn < pcache_pks_end_pfn;
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
#else
static inline void pcache_pks_count_scope(void) {}
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
 */
static inline void pcache_pks_scope_clamp(struct pcache_pks_scope *scope)
{
	scope->active = pcache_pks_is_enabled();
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

static inline void pcache_pks_pool_reserve(void) {}
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
static inline bool pcache_pks_page(struct page *page)
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
