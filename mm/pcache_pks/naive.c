// SPDX-License-Identifier: GPL-2.0
/*
 * Protected page cache: per-page placement baseline (CONFIG_PCACHE_PKS_BASELINES).
 *
 * Selected with pcache_pks.placement=naive for evaluation: allocates 4 KiB
 * non-movable pages from buddy, tags each with set_memory_pkey_noflush() +
 * alias map + TLB flush, and batches deferred untagging via WQ_MEM_RECLAIM
 * on free.
 */
#define pr_fmt(fmt) "pcache_pks: " fmt

#include <linux/atomic.h>
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

unsigned long *pcache_pks_page_map __read_mostly;
EXPORT_SYMBOL_GPL(pcache_pks_page_map);

unsigned long pcache_pks_max_pfn __read_mostly;
EXPORT_SYMBOL_GPL(pcache_pks_max_pfn);

static DEFINE_SPINLOCK(pks_naive_lock);
static LIST_HEAD(pks_naive_pending);
static atomic_long_t pks_naive_nr_pending = ATOMIC_LONG_INIT(0);
static atomic_long_t pks_naive_nr_tagged = ATOMIC_LONG_INIT(0);
static struct workqueue_struct *pks_naive_wq __ro_after_init;

static bool pks_naive_untag(unsigned long addr)
{
	pks_alias_unmap(addr, PAGE_SIZE, NULL);
	return !set_memory_pkey_noflush(addr, 1, 0, NULL);
}

struct page *pks_naive_alloc(gfp_t gfp, int preferred_nid, nodemask_t *nodemask)
{
	DEFINE_PCACHE_PKS_SCOPE(scope);
	bool flush_all = false;
	bool clean = false;
	unsigned int noreclaim;
	unsigned long addr;
	struct page *page;
	int err;

	/* Direct-map PTE splitting may sleep and allocate page tables. */
	if (!gfpflags_allow_blocking(gfp) || (current->flags & PF_MEMALLOC))
		return NULL;

	page = __alloc_pages((gfp & ~(__GFP_MOVABLE | __GFP_HIGHMEM | __GFP_PKS)) |
			     __GFP_RECLAIMABLE | __GFP_ZERO,
			     0, preferred_nid, nodemask);
	if (!page)
		return NULL;
	if (page_to_pfn(page) >= pcache_pks_max_pfn) {
		__free_page(page);
		return NULL;
	}
	addr = (unsigned long)page_address(page);

	pcache_pks_scope_clamp(&scope);
	noreclaim = memalloc_noreclaim_save();
	vm_unmap_aliases();
	err = set_memory_pkey_noflush(addr, 1, PKS_KEY_PAGE_CACHE, &flush_all);
	if (!err)
		err = pks_alias_map(addr, PAGE_SIZE);
	if (!err) {
		/* The alias was not mapped before, so one round is enough */
		if (flush_all)
			flush_tlb_all();
		else
			flush_tlb_kernel_range(addr, addr + PAGE_SIZE);
	} else {
		clean = pks_naive_untag(addr);
		flush_tlb_all();
	}
	memalloc_noreclaim_restore(noreclaim);
	pcache_pks_scope_end(&scope);

	if (err) {
		if (!WARN_ON_ONCE(!clean))
			__free_page(page);
		return NULL;
	}

	pks_audit_range(addr, PAGE_SIZE, PKS_KEY_PAGE_CACHE);
	pks_audit_range(pks_alias_addr(addr), PAGE_SIZE, PKS_KEY_PAGE_CACHE_W);
	pks_stat_inc(PKS_STAT_TAG);
	pks_stat_inc(PKS_STAT_FLUSH);
	atomic_long_inc(&pks_naive_nr_tagged);
	set_bit(page_to_pfn(page), pcache_pks_page_map);
	return page;
}

static void pks_naive_untag_fn(struct work_struct *work)
{
	struct page *page, *next;
	unsigned long processed = 0, freed = 0;
	LIST_HEAD(batch);

	spin_lock_irq(&pks_naive_lock);
	list_splice_init(&pks_naive_pending, &batch);
	spin_unlock_irq(&pks_naive_lock);
	if (list_empty(&batch))
		return;

	list_for_each_entry_safe(page, next, &batch, lru) {
		unsigned long addr = (unsigned long)page_address(page);

		clear_bit(page_to_pfn(page), pcache_pks_page_map);
		mod_node_page_state(page_pgdat(page),
				    NR_KERNEL_MISC_RECLAIMABLE, -1);
		if (WARN_ON_ONCE(!pks_naive_untag(addr)))
			list_del(&page->lru);
		if (!(++processed & 0x3f))
			cond_resched();
	}
	flush_tlb_all();
	pks_stat_inc(PKS_STAT_FLUSH);

	list_for_each_entry_safe(page, next, &batch, lru) {
		list_del(&page->lru);
		pks_audit_range((unsigned long)page_address(page), PAGE_SIZE, 0);
		set_page_refcounted(page);
		__free_page(page);
		freed++;
	}
	if (freed) {
		pks_stat_add(PKS_STAT_UNTAG, freed);
		atomic_long_sub(freed, &pks_naive_nr_tagged);
	}
	atomic_long_sub(processed, &pks_naive_nr_pending);
}
static DECLARE_WORK(pks_naive_work, pks_naive_untag_fn);

void pks_naive_free(struct page *page)
{
	unsigned long flags;

	spin_lock_irqsave(&pks_naive_lock, flags);
	list_add_tail(&page->lru, &pks_naive_pending);
	atomic_long_inc(&pks_naive_nr_pending);
	spin_unlock_irqrestore(&pks_naive_lock, flags);
	mod_node_page_state(page_pgdat(page), NR_KERNEL_MISC_RECLAIMABLE, 1);
	queue_work(pks_naive_wq, &pks_naive_work);
}

void pks_naive_info(struct pks_zone_info *info)
{
	memset(info, 0, sizeof(*info));
	info->tagged_pages = atomic_long_read(&pks_naive_nr_tagged);
	info->pending_pages = atomic_long_read(&pks_naive_nr_pending);
	info->free_pages = info->pending_pages;
}

int __init pks_naive_init(void)
{
	pcache_pks_max_pfn = max_pfn;
	pcache_pks_page_map = kvcalloc(BITS_TO_LONGS(pcache_pks_max_pfn),
				       sizeof(unsigned long), GFP_KERNEL);
	pks_naive_wq = alloc_workqueue("pcache_pks_untag",
				       WQ_UNBOUND | WQ_MEM_RECLAIM, 1);
	if (!pcache_pks_page_map || !pks_naive_wq) {
		if (pks_naive_wq) {
			destroy_workqueue(pks_naive_wq);
			pks_naive_wq = NULL;
		}
		kvfree(pcache_pks_page_map);
		pcache_pks_page_map = NULL;
		pcache_pks_max_pfn = 0;
		return -ENOMEM;
	}
	static_branch_enable(&pcache_pks_per_page);
	return 0;
}
