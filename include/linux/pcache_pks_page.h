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
 *
 * Each block has a second mapping, the write alias, at a fixed distance from
 * its direct-map address and tagged PKS_KEY_PAGE_CACHE_W.
 */
#define PKS_BLOCK_ORDER		(PMD_SHIFT - PAGE_SHIFT)
#define PKS_BLOCK_PAGES		(1UL << PKS_BLOCK_ORDER)

DECLARE_STATIC_KEY_FALSE(pcache_pks_enabled);
extern struct pks_block **pcache_pks_block_map;
extern unsigned long pcache_pks_nr_blocks_max;
extern unsigned long pcache_pks_alias_delta;

#ifdef CONFIG_PCACHE_PKS_BASELINES
/* The per-page baseline keeps its membership in a bitmap of page frames. */
DECLARE_STATIC_KEY_FALSE(pcache_pks_per_page);
extern unsigned long *pcache_pks_page_map;
#endif

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
#ifdef CONFIG_PCACHE_PKS_BASELINES
	if (static_branch_unlikely(&pcache_pks_per_page))
		return idx < pcache_pks_nr_blocks_max &&
		       test_bit(page_to_pfn(page), pcache_pks_page_map);
#endif
	return idx < pcache_pks_nr_blocks_max &&
	       READ_ONCE(pcache_pks_block_map[idx]) != NULL;
}

/*
 * The address through which a write path stores into @page. @kaddr is a
 * direct-map address inside @page, as kmap_local_page() returns it. For a
 * protected page the result is the same byte in the write alias, which is the
 * only mapping a write scope makes writable. For any other page it is @kaddr.
 *
 * Only code that writes file data on behalf of a write path may use this.
 * Every call site is part of what the protection trusts.
 */
static inline void *pcache_pks_waddr(const struct page *page, void *kaddr)
{
	if (pcache_pks_page(page))
		return kaddr + pcache_pks_alias_delta;
	return kaddr;
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
static inline void *pcache_pks_waddr(const struct page *page, void *kaddr)
{
	return kaddr;
}

#endif /* CONFIG_PCACHE_PKS */

#endif /* _LINUX_PCACHE_PKS_PAGE_H */
