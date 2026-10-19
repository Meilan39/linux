/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PCACHE_PKS_H
#define _LINUX_PCACHE_PKS_H

#include <linux/jump_label.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/pks.h>
#include <linux/pks-keys.h>

struct pt_regs;

/*
 * A write scope grants the current thread write access to the PKS pool for
 * the duration of an authorized page-cache store and then restores whatever
 * access it had before, so scopes nest:
 *
 *	struct pcache_pks_scope pks_scope;
 *
 *	pcache_pks_scope_begin(&pks_scope, mapping);
 *	...store into @mapping's page cache...
 *	pcache_pks_scope_end(&pks_scope);
 *
 * The scope is only opened when @mapping is protected; for any other
 * mapping, or a NULL one, both calls do nothing.
 */
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

/* Is @page part of the PKS page-cache pool? */
static inline bool pcache_pks_page(struct page *page)
{
	unsigned long pfn;

	if (!static_branch_unlikely(&pcache_pks_enabled))
		return false;
	pfn = page_to_pfn(page);
	return pfn >= pcache_pks_start_pfn && pfn < pcache_pks_end_pfn;
}

/* Does @mapping keep its page cache in the PKS pool? */
static inline bool pcache_pks_mapping(const struct address_space *mapping)
{
	return static_branch_unlikely(&pcache_pks_enabled) &&
	       mapping && mapping_pks_protected(mapping);
}

static inline void pcache_pks_scope_begin(struct pcache_pks_scope *scope,
					  const struct address_space *mapping)
{
	scope->active = pcache_pks_mapping(mapping);
	if (scope->active)
		scope->old = pks_update_protection(PKS_KEY_PAGE_CACHE,
						   PKEY_READ_WRITE);
}

/* Like pcache_pks_scope_begin(), but revokes write access to the pool. */
static inline void pcache_pks_scope_clamp(struct pcache_pks_scope *scope)
{
	scope->active = static_branch_unlikely(&pcache_pks_enabled);
	if (scope->active)
		scope->old = pks_update_protection(PKS_KEY_PAGE_CACHE,
						   PKEY_DISABLE_WRITE);
}

static inline void pcache_pks_scope_end(struct pcache_pks_scope *scope)
{
	if (scope->active)
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
static inline bool pcache_pks_page(struct page *page) { return false; }
static inline bool pcache_pks_mapping(const struct address_space *mapping)
{
	return false;
}
static inline void pcache_pks_scope_begin(struct pcache_pks_scope *scope,
					  const struct address_space *mapping) {}
static inline void pcache_pks_scope_clamp(struct pcache_pks_scope *scope) {}
static inline void pcache_pks_scope_end(struct pcache_pks_scope *scope) {}

#endif /* CONFIG_PCACHE_PKS */

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
