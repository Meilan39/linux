/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PCACHE_PKS_PAGE_H
#define _LINUX_PCACHE_PKS_PAGE_H

/*
 * Page-level tests of the protected page cache. This header is kept small so
 * that low-level headers such as <linux/highmem.h> can include it; the rest of
 * the interface is in <linux/pcache_pks.h>.
 */
#include <linux/jump_label.h>
#include <linux/mm.h>

struct pks_block;

#ifdef CONFIG_PCACHE_PKS

/*
 * A protected page-cache page lives inside a 2 MiB (PMD) block whose direct-map
 * leaf is tagged PKS_KEY_PAGE_CACHE. Blocks are taken from and returned to the
 * buddy allocator on demand; membership is a table indexed by block number.
 */
#define PKS_BLOCK_ORDER		(PMD_SHIFT - PAGE_SHIFT)
#define PKS_BLOCK_PAGES		(1UL << PKS_BLOCK_ORDER)

DECLARE_STATIC_KEY_FALSE(pcache_pks_enabled);
extern struct pks_block **pcache_pks_block_map;
extern unsigned long pcache_pks_nr_blocks_max;

/* Is the page-cache protection active on this boot? */
static inline bool pcache_pks_is_enabled(void)
{
	return static_branch_unlikely(&pcache_pks_enabled);
}

/*
 * Is @page a protected page? O(1) and lockless: the block map entry is
 * published with release semantics only after the whole block is tagged and
 * flushed, and cleared before the block is untagged, so a non-NULL entry means
 * that every page of the block carries PKS_KEY_PAGE_CACHE.
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

static inline bool pcache_pks_is_enabled(void)
{
	return false;
}
static inline bool pcache_pks_page(const struct page *page)
{
	return false;
}

#endif /* CONFIG_PCACHE_PKS */

#endif /* _LINUX_PCACHE_PKS_PAGE_H */
