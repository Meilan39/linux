// SPDX-License-Identifier: GPL-2.0
/*
 * Protected page cache: per-page placement. An evaluation baseline, built only
 * with CONFIG_PCACHE_PKS_BASELINES and selected with pcache_pks.placement=naive.
 *
 * This is the direct way to protect a page: take any page from the buddy
 * allocator, tag it when it enters the page cache, and untag it when it leaves.
 * Everything else, the scopes, the alias, the fault handler and the admission
 * rules, is the same code as with the block pool, so a comparison of the two
 * shows the cost of the placement alone.
 *
 * The baseline is built as well as the approach allows:
 *   - it uses the same tagging and alias helpers as the block pool;
 *   - a page is tagged with one single-page flush, not a full one;
 *   - freed pages are untagged by a worker that handles everything that is
 *     waiting in one pass and flushes once for the pass.
 * What stays is what the approach implies: every page that enters the page
 * cache costs a page-attribute change and an IPI to every CPU, and every page
 * it touches leaves its part of the direct map split into 4 KiB entries.
 */
#define pr_fmt(fmt) "pcache_pks: " fmt

#include <linux/bitmap.h>
#include <linux/gfp.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/spinlock.h>
#include <linux/vmalloc.h>
#include <linux/vmstat.h>
#include <linux/workqueue.h>
#include <asm/set_memory.h>
#include <asm/tlbflush.h>
#include "../internal.h"
#include "internal.h"

DEFINE_STATIC_KEY_FALSE(pcache_pks_per_page);
EXPORT_SYMBOL_GPL(pcache_pks_per_page);

/* One bit per page frame: set while the page is tagged and published. */
unsigned long *pcache_pks_page_map __read_mostly;
EXPORT_SYMBOL_GPL(pcache_pks_page_map);

static DEFINE_SPINLOCK(pks_naive_lock);
static LIST_HEAD(pks_naive_pending);		/* freed, still tagged */
static unsigned long pks_naive_nr_pending;
static atomic_long_t pks_naive_nr_tagged;
static struct workqueue_struct *pks_naive_wq;

/* Remove the alias and the key of one page. Returns false if the key stuck. */
static bool pks_naive_untag(unsigned long addr)
{
	bool unused;

	pks_alias_unmap(addr, PAGE_SIZE, &unused);
	return !set_memory_pkey_noflush(addr, 1, 0, NULL);
}

static struct page *pks_naive_alloc(gfp_t gfp)
{
	struct pcache_pks_scope scope;
	unsigned int noreclaim;
	unsigned long addr;
	struct page *page;
	int err;

	/* Tagging sleeps, and must not run from inside reclaim. */
	if (!gfpflags_allow_blocking(gfp) || (current->flags & PF_MEMALLOC))
		return NULL;

	/*
	 * The page can be evicted but never migrated, so it is not movable.
	 * It is zeroed before it is tagged, as a block of the pool is.
	 */
	page = alloc_page((gfp & ~(__GFP_MOVABLE | __GFP_HIGHMEM)) |
			  __GFP_RECLAIMABLE | __GFP_ZERO);
	if (!page)
		return NULL;
	if (page_to_pfn(page) >= pcache_pks_nr_blocks_max << PKS_BLOCK_ORDER) {
		__free_page(page);
		return NULL;
	}
	addr = (unsigned long)page_address(page);

	/* ->write_begin() gets here inside an open write scope. */
	pcache_pks_scope_clamp(&scope);
	noreclaim = memalloc_noreclaim_save();
	vm_unmap_aliases();
	err = set_memory_pkey_noflush(addr, 1, PKS_KEY_PAGE_CACHE, NULL);
	if (!err)
		err = pks_alias_map(addr, PAGE_SIZE);
	if (err) {
		bool clean = pks_naive_untag(addr);

		flush_tlb_all();
		memalloc_noreclaim_restore(noreclaim);
		pcache_pks_scope_end(&scope);
		/* Never free a page whose key might still be set. */
		if (!WARN_ON_ONCE(!clean))
			__free_page(page);
		return NULL;
	}
	flush_tlb_kernel_range(addr, addr + PAGE_SIZE);
	memalloc_noreclaim_restore(noreclaim);
	pcache_pks_scope_end(&scope);

	pks_audit_range(addr, PAGE_SIZE, PKS_KEY_PAGE_CACHE);
	pks_stat_inc(PKS_STAT_TAG);
	pks_stat_inc(PKS_STAT_FLUSH);
	atomic_long_inc(&pks_naive_nr_tagged);

	/* Publish only after every CPU has the tagged translation. */
	set_bit(page_to_pfn(page), pcache_pks_page_map);
	return page;
}

/*
 * Untag everything that is waiting, flush once, and give the pages to the
 * buddy allocator. The order is the block pool's: unpublish, untag, flush,
 * free.
 */
static void pks_naive_untag_fn(struct work_struct *work)
{
	struct page *page, *next;
	unsigned long n = 0;
	LIST_HEAD(batch);

	spin_lock_irq(&pks_naive_lock);
	list_splice_init(&pks_naive_pending, &batch);
	spin_unlock_irq(&pks_naive_lock);
	if (list_empty(&batch))
		return;

	list_for_each_entry_safe(page, next, &batch, lru) {
		unsigned long addr = (unsigned long)page_address(page);

		clear_bit(page_to_pfn(page), pcache_pks_page_map);
		/* A page whose key stuck is leaked, not freed. */
		if (WARN_ON_ONCE(!pks_naive_untag(addr)))
			list_del(&page->lru);
		if (!(++n & 0x3f))
			cond_resched();
	}
	flush_tlb_all();
	pks_stat_add(PKS_STAT_UNTAG, n);
	pks_stat_inc(PKS_STAT_FLUSH);

	list_for_each_entry_safe(page, next, &batch, lru) {
		list_del(&page->lru);
		pks_audit_range((unsigned long)page_address(page), PAGE_SIZE, 0);
		set_page_refcounted(page);
		__free_page(page);
	}
	mod_node_page_state(NODE_DATA(first_online_node),
			    NR_KERNEL_MISC_RECLAIMABLE, -(long)n);
	atomic_long_sub(n, &pks_naive_nr_tagged);

	spin_lock_irq(&pks_naive_lock);
	pks_naive_nr_pending -= n;
	spin_unlock_irq(&pks_naive_lock);
}
static DECLARE_WORK(pks_naive_work, pks_naive_untag_fn);

/* Any context: the page cannot be untagged here, so it waits for the worker. */
static void pks_naive_free(struct page *page)
{
	unsigned long flags;

	spin_lock_irqsave(&pks_naive_lock, flags);
	list_add_tail(&page->lru, &pks_naive_pending);
	pks_naive_nr_pending++;
	spin_unlock_irqrestore(&pks_naive_lock, flags);
	mod_node_page_state(NODE_DATA(first_online_node),
			    NR_KERNEL_MISC_RECLAIMABLE, 1);
	queue_work(pks_naive_wq, &pks_naive_work);
}

static void pks_naive_info(struct pks_pool_info *info)
{
	memset(info, 0, sizeof(*info));
	info->tagged_pages = atomic_long_read(&pks_naive_nr_tagged);
	info->pending_pages = READ_ONCE(pks_naive_nr_pending);
	info->free_pages = info->pending_pages;
}

int __init pks_naive_init(void)
{
	unsigned long nr_pfns;

	/* Memory added after boot is not used, as with the block pool. */
	pcache_pks_nr_blocks_max = (max_pfn + PKS_BLOCK_PAGES - 1) >>
				   PKS_BLOCK_ORDER;
	nr_pfns = pcache_pks_nr_blocks_max << PKS_BLOCK_ORDER;

	pcache_pks_page_map = kvcalloc(BITS_TO_LONGS(nr_pfns),
				       sizeof(unsigned long), GFP_KERNEL);
	/* The worker frees memory, so it must be able to run when there is none. */
	pks_naive_wq = alloc_workqueue("pcache_pks_untag",
				       WQ_UNBOUND | WQ_MEM_RECLAIM, 1);
	if (!pcache_pks_page_map || !pks_naive_wq) {
		kvfree(pcache_pks_page_map);
		pcache_pks_page_map = NULL;
		pcache_pks_nr_blocks_max = 0;
		return -ENOMEM;
	}
	static_branch_enable(&pcache_pks_per_page);
	return 0;
}

const struct pks_placement pks_naive_placement = {
	.name	= "naive",
	.alloc	= pks_naive_alloc,
	.free	= pks_naive_free,
	.info	= pks_naive_info,
};
