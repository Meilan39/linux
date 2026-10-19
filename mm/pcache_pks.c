// SPDX-License-Identifier: GPL-2.0
/*
 * PKS-protected page-cache pool.
 *
 * A PMD-aligned range of memory is reserved at boot, tagged with
 * PKS_KEY_PAGE_CACHE (write-disabled by default) and handed out one order-0
 * page at a time to the page cache of protected mappings. Freed pool pages
 * return here rather than to the buddy allocator.
 */
#include <linux/memblock.h>
#include <linux/mm.h>
#include <linux/pcache_pks.h>
#include <linux/printk.h>
#include <linux/spinlock.h>
#include <asm/set_memory.h>
#include "internal.h"

#define PCACHE_PKS_POOL_SIZE	SZ_256M

DEFINE_STATIC_KEY_FALSE(pcache_pks_enabled);
EXPORT_SYMBOL_GPL(pcache_pks_enabled);

unsigned long pcache_pks_start_pfn __ro_after_init;
unsigned long pcache_pks_end_pfn __ro_after_init;

static phys_addr_t pool_base __initdata;
static DEFINE_SPINLOCK(pool_lock);
static LIST_HEAD(pool_free);

static bool pcache_pks_param __initdata;

static int __init setup_pcache_pks(char *str)
{
	return kstrtobool(str, &pcache_pks_param);
}
early_param("pcache_pks", setup_pcache_pks);

void __init pcache_pks_pool_reserve(void)
{
	if (!pcache_pks_param || !pks_available())
		return;

	/*
	 * A PMD-aligned physical range is also PMD-aligned in the direct map,
	 * whose base is PUD-aligned, so tagging the pool never splits a PMD.
	 */
	pool_base = memblock_phys_alloc_range(PCACHE_PKS_POOL_SIZE, PMD_SIZE,
					      SZ_1M, PFN_PHYS(max_pfn));
	if (!pool_base) {
		pr_err("pcache_pks: cannot reserve pool\n");
		return;
	}

	pcache_pks_start_pfn = PHYS_PFN(pool_base);
	pcache_pks_end_pfn = PHYS_PFN(pool_base + PCACHE_PKS_POOL_SIZE);
}

static int __init pcache_pks_init(void)
{
	LIST_HEAD(pages);
	unsigned long pfn;
	int ret;

	if (!pool_base)
		return 0;

	if (want_init_on_alloc(0) || want_init_on_free() ||
	    page_poisoning_enabled_static() || debug_pagealloc_enabled()) {
		pr_err("pcache_pks: incompatible page initialization active\n");
		return 0;
	}

	memset(__va(pool_base), 0, PCACHE_PKS_POOL_SIZE);

	for (pfn = pcache_pks_start_pfn; pfn < pcache_pks_end_pfn; pfn++) {
		struct page *page = pfn_to_page(pfn);

		if (!PageReserved(page) || page_count(page) != 1 ||
		    PageCompound(page) || PageLRU(page)) {
			pr_err("pcache_pks: invalid page state in pool\n");
			return 0;
		}
		set_page_count(page, 0);
		list_add_tail(&page->lru, &pages);
	}

	ret = set_memory_pkey((unsigned long)__va(pool_base),
			      PCACHE_PKS_POOL_SIZE >> PAGE_SHIFT,
			      PKS_KEY_PAGE_CACHE);
	if (ret) {
		pr_err("pcache_pks: set_memory_pkey failed: %d\n", ret);
		return 0;
	}

	list_splice(&pages, &pool_free);
	static_branch_enable(&pcache_pks_enabled);
	pr_info("pcache_pks: initialized %dMB pool at %pa\n",
		PCACHE_PKS_POOL_SIZE >> 20, &pool_base);
	return 0;
}
early_initcall(pcache_pks_init);

struct folio *pcache_pks_alloc_folio(gfp_t gfp, unsigned int order)
{
	struct page *page;
	unsigned long flags;

	if (WARN_ON_ONCE(order != 0) || (gfp & __GFP_ZERO))
		return NULL;

	spin_lock_irqsave(&pool_lock, flags);
	if (list_empty(&pool_free)) {
		spin_unlock_irqrestore(&pool_lock, flags);
		pr_warn_ratelimited("pcache_pks: pool exhausted\n");
		return NULL;
	}
	page = list_first_entry(&pool_free, struct page, lru);
	if (WARN_ON_ONCE(!PageReserved(page) || page_count(page) ||
			 PageLRU(page) || PageCompound(page))) {
		spin_unlock_irqrestore(&pool_lock, flags);
		return NULL;
	}
	list_del(&page->lru);
	spin_unlock_irqrestore(&pool_lock, flags);

	__ClearPageReserved(page);
	post_alloc_hook(page, 0, gfp);
	return page_folio(page);
}

void pcache_pks_recycle_page(struct page *page)
{
	unsigned long flags;

	if (WARN_ON_ONCE(!pcache_pks_page(page)))
		return;

	__SetPageReserved(page);
	spin_lock_irqsave(&pool_lock, flags);
	list_add(&page->lru, &pool_free);
	spin_unlock_irqrestore(&pool_lock, flags);
}
