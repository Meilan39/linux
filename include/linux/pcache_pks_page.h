/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PCACHE_PKS_PAGE_H
#define _LINUX_PCACHE_PKS_PAGE_H

/*
 * Page-level tests of the protected page cache. Kept small so low-level
 * headers such as <linux/highmem.h> can include it; the rest of the interface
 * is in <linux/pcache_pks.h>.
 */
#include <linux/jump_label.h>

#ifdef CONFIG_PCACHE_PKS

#include <linux/mm.h>

DECLARE_STATIC_KEY_FALSE(pcache_pks_enabled);
extern unsigned long pcache_pks_alias_delta;

#ifdef CONFIG_PCACHE_PKS_BASELINES
DECLARE_STATIC_KEY_FALSE(pcache_pks_per_page);
extern unsigned long *pcache_pks_page_map;
extern unsigned long pcache_pks_max_pfn;

static inline bool pks_naive_is_page(unsigned long pfn)
{
	return pfn < pcache_pks_max_pfn && test_bit(pfn, pcache_pks_page_map);
}

struct page *pks_naive_alloc(gfp_t gfp, int preferred_nid, nodemask_t *nodemask);
void pks_naive_free(struct page *page);
#endif

/* Is page-cache protection active on this boot? */
static inline bool pcache_pks_is_enabled(void)
{
	return static_branch_unlikely(&pcache_pks_enabled);
}

/* Is @page backed by ZONE_PKS on this boot? */
static inline bool pcache_pks_zone_page(const struct page *page)
{
	return pcache_pks_is_enabled() && page_zonenum(page) == ZONE_PKS;
}

/*
 * Is @page protected? Every page whose zone is ZONE_PKS carries
 * PKS_KEY_PAGE_CACHE in the direct map on every CPU and has a write alias:
 * blocks enter ZONE_PKS only after tagging and flushing, and leave it before
 * keys are cleared. While a caller holds a reference the answer is stable.
 */
static inline bool pcache_pks_page(const struct page *page)
{
	if (!pcache_pks_is_enabled())
		return false;
	if (page_zonenum(page) == ZONE_PKS)
		return true;
#ifdef CONFIG_PCACHE_PKS_BASELINES
	if (static_branch_unlikely(&pcache_pks_per_page))
		return pks_naive_is_page(page_to_pfn(page));
#endif
	return false;
}

/*
 * Return the address through which granted write paths store into @page.
 * @kaddr is a direct-map address for @page, such as kmap_local_page() returns.
 * For protected pages this selects the write alias; for others it is @kaddr.
 */
static inline void *pcache_pks_waddr(const struct page *page, void *kaddr)
{
	if (unlikely(pcache_pks_page(page)))
		return kaddr + pcache_pks_alias_delta;
	return kaddr;
}

/*
 * PG_error on a protected page that is being filled means that a store of the
 * fill was refused. Forget an earlier error, such as one of writeback, before
 * the read is issued.
 */
static inline void pcache_pks_fill_begin(struct page *page)
{
	if (unlikely(pcache_pks_page(page)) && !PageUptodate(page))
		ClearPageError(page);
}

static inline bool pcache_pks_fill_failed(struct page *page)
{
	return unlikely(pcache_pks_page(page) && PageError(page));
}

#else /* !CONFIG_PCACHE_PKS */

struct page;

static inline bool pcache_pks_is_enabled(void)
{
	return false;
}

static inline bool pcache_pks_zone_page(const struct page *page)
{
	return false;
}

static inline bool pcache_pks_page(const struct page *page)
{
	return false;
}

static inline void *pcache_pks_waddr(const struct page *page, void *kaddr)
{
	return kaddr;
}

static inline void pcache_pks_fill_begin(struct page *page)
{
}

static inline bool pcache_pks_fill_failed(struct page *page)
{
	return false;
}

#endif /* CONFIG_PCACHE_PKS */

#endif /* _LINUX_PCACHE_PKS_PAGE_H */
