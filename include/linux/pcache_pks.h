/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PCACHE_PKS_H
#define _LINUX_PCACHE_PKS_H

#include <linux/jump_label.h>
#include <linux/mm.h>
#include <linux/pks.h>
#include <linux/pks-keys.h>

struct pks_block;

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

#endif /* CONFIG_PCACHE_PKS */

#endif /* _LINUX_PCACHE_PKS_H */
