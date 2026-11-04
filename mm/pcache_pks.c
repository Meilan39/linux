// SPDX-License-Identifier: GPL-2.0
/*
 * PKS-protected page-cache pool.
 *
 * A PMD-aligned physical range is reserved at boot, tagged once in the direct
 * map with PKS_KEY_PAGE_CACHE, and handed out one order-0 folio at a time to
 * protected page-cache mappings. Freed pool pages return directly to pool_free
 * rather than entering the general buddy allocator. When pool_free runs dry,
 * non-blocking trylock scans evict clean cached pool folios and wake background
 * writeback when dirty pool pages dominate.
 */
#include <linux/atomic.h>
#include <linux/export.h>
#include <linux/gfp.h>
#include <linux/jiffies.h>
#include <linux/memblock.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/pcache_pks.h>
#include <linux/printk.h>
#include <linux/ratelimit.h>
#include <linux/rmap.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/sched/signal.h>
#include <linux/sizes.h>
#include <linux/spinlock.h>
#include <linux/swap.h>
#include <linux/writeback.h>
#include <asm/insn.h>
#include <asm/set_memory.h>
#include "internal.h"

#define PCACHE_PKS_DEFAULT_MB		4096UL
#define PCACHE_PKS_MAX_MB		65536UL
#define PCACHE_PKS_MIN_SIZE		(2 * PMD_SIZE)
#define PCACHE_PKS_RECLAIM_BATCH	64UL
#define PCACHE_PKS_RECLAIM_ATTEMPTS	16
#define PCACHE_PKS_WRITEBACK_RETRIES	200

DEFINE_STATIC_KEY_FALSE(pcache_pks_enabled);
EXPORT_SYMBOL_GPL(pcache_pks_enabled);

unsigned long pcache_pks_start_pfn __ro_after_init;
EXPORT_SYMBOL_GPL(pcache_pks_start_pfn);

unsigned long pcache_pks_end_pfn __ro_after_init;
EXPORT_SYMBOL_GPL(pcache_pks_end_pfn);

static phys_addr_t pool_base __initdata;
static phys_addr_t pool_size __initdata;
static DEFINE_SPINLOCK(pool_lock);
static LIST_HEAD(pool_free);
static atomic_long_t pool_free_count = ATOMIC_LONG_INIT(0);
static atomic_long_t reclaim_cursor = ATOMIC_LONG_INIT(0);

static bool pcache_pks_param __initdata;
static unsigned long pcache_pks_mb_param __initdata;

static int __init setup_pcache_pks(char *str)
{
	return kstrtobool(str, &pcache_pks_param);
}
early_param("pcache_pks", setup_pcache_pks);

static int __init setup_pcache_pks_mb(char *str)
{
	unsigned long val;
	int ret;

	ret = kstrtoul(str, 0, &val);
	if (ret)
		return ret;
	if (!val)
		return -EINVAL;
	pcache_pks_mb_param = val;
	return 0;
}
early_param("pcache_pks_mb", setup_pcache_pks_mb);

void __init pcache_pks_pool_reserve(void)
{
	phys_addr_t total_ram, max_share, target;

	if (!pcache_pks_param || !pks_available())
		return;

	total_ram = memblock_phys_mem_size();
	if (pcache_pks_mb_param) {
		unsigned long mb = min(pcache_pks_mb_param, PCACHE_PKS_MAX_MB);

		target = (phys_addr_t)mb << 20;
		max_share = total_ram / 2;
	} else {
		target = (phys_addr_t)PCACHE_PKS_DEFAULT_MB << 20;
		max_share = total_ram / 8;
	}

	target = ALIGN_DOWN(min(target, max_share), PMD_SIZE);
	if (target < PCACHE_PKS_MIN_SIZE) {
		pr_err("pcache_pks: insufficient system RAM for pool\n");
		return;
	}

	/*
	 * A PMD-aligned physical range is also PMD-aligned in the direct map,
	 * whose base is PUD-aligned, so tagging the pool never splits a PMD.
	 */
	pool_base = memblock_phys_alloc_range(target, PMD_SIZE,
					      SZ_1M, PFN_PHYS(max_pfn));
	if (!pool_base) {
		pr_err("pcache_pks: cannot reserve %llu MiB pool\n",
		       (unsigned long long)(target >> 20));
		return;
	}

	pool_size = target;
	pcache_pks_start_pfn = PHYS_PFN(pool_base);
	pcache_pks_end_pfn = PHYS_PFN(pool_base + pool_size);
}

static int __init pcache_pks_init(void)
{
	LIST_HEAD(pages);
	unsigned long pfn, nr_pages = 0;
	int ret;

	if (!pool_base)
		return 0;

	if (want_init_on_alloc(0) || want_init_on_free() ||
	    page_poisoning_enabled_static() || debug_pagealloc_enabled()) {
		pr_err("pcache_pks: incompatible page initialization active\n");
		return 0;
	}

	memset(__va(pool_base), 0, pool_size);

	for (pfn = pcache_pks_start_pfn; pfn < pcache_pks_end_pfn; pfn++) {
		struct page *page = pfn_to_page(pfn);

		if (!PageReserved(page) || page_count(page) != 1 ||
		    PageCompound(page) || PageLRU(page)) {
			pr_err("pcache_pks: invalid page state in pool\n");
			return 0;
		}
		set_page_count(page, 0);
		list_add_tail(&page->lru, &pages);
		nr_pages++;
	}

	ret = set_memory_pkey((unsigned long)__va(pool_base),
			      (int)(pool_size >> PAGE_SHIFT),
			      PKS_KEY_PAGE_CACHE);
	if (ret) {
		pr_err("pcache_pks: set_memory_pkey failed: %d\n", ret);
		return 0;
	}

	list_splice(&pages, &pool_free);
	atomic_long_set(&pool_free_count, nr_pages);
	static_branch_enable(&pcache_pks_enabled);
	pr_info("pcache_pks: initialized %llu MiB pool at %pa\n",
		(unsigned long long)(pool_size >> 20), &pool_base);
	return 0;
}
early_initcall(pcache_pks_init);

static unsigned long pcache_pks_scan_pool(gfp_t gfp, bool unmap,
					  bool *saw_dirty_or_wb)
{
	unsigned long nr_pages = pcache_pks_end_pfn - pcache_pks_start_pfn;
	unsigned long start, i, reclaimed = 0;

	if (!nr_pages)
		return 0;

	start = (unsigned long)atomic_long_read(&reclaim_cursor) % nr_pages;

	for (i = 0; i < nr_pages; i++) {
		unsigned long idx = (start + i) % nr_pages;
		struct page *page = pfn_to_page(pcache_pks_start_pfn + idx);
		struct folio *folio;
		struct address_space *mapping;
		bool evicted = false;

		if (i && (i & 0xfff) == 0)
			cond_resched();

		if (PageReserved(page) || PageUnevictable(page))
			continue;

		if (PageDirty(page) || PageWriteback(page)) {
			if (saw_dirty_or_wb)
				*saw_dirty_or_wb = true;
			continue;
		}

		folio = page_folio(page);
		if (!unmap && folio_mapped(folio))
			continue;

		if (!folio_try_get(folio))
			continue;

		/*
		 * PG_locked is only well defined on LRU folios. A folio just
		 * taken from the pool holds a reference but has not yet been
		 * through the non-atomic __folio_set_locked() in
		 * filemap_add_folio(), and locking it here would lose that bit.
		 */
		if (!folio_test_lru(folio)) {
			folio_put(folio);
			continue;
		}

		if (!folio_trylock(folio)) {
			folio_put(folio);
			continue;
		}

		if (folio_test_dirty(folio) || folio_test_writeback(folio)) {
			if (saw_dirty_or_wb)
				*saw_dirty_or_wb = true;
			goto unlock_put;
		}

		if (!folio_evictable(folio))
			goto unlock_put;

		if (folio_mapped(folio)) {
			if (!unmap)
				goto unlock_put;
			try_to_unmap(folio, 0);
			if (folio_mapped(folio) || folio_test_dirty(folio)) {
				if (folio_test_dirty(folio) && saw_dirty_or_wb)
					*saw_dirty_or_wb = true;
				goto unlock_put;
			}
		}

		mapping = folio_mapping(folio);
		if (mapping) {
			if (invalidate_inode_page(&folio->page) > 0)
				evicted = true;
		} else if (folio_has_private(folio)) {
			if (filemap_release_folio(folio, gfp))
				evicted = true;
		}

unlock_put:
		folio_unlock(folio);
		folio_put(folio);

		if (evicted) {
			reclaimed++;
			if (reclaimed >= PCACHE_PKS_RECLAIM_BATCH) {
				i++;
				break;
			}
		}
	}

	atomic_long_set(&reclaim_cursor, (start + i) % nr_pages);
	return reclaimed;
}

static bool pcache_pks_reclaim_pages(gfp_t gfp)
{
	struct pcache_pks_scope pks_scope;
	unsigned int noreclaim_flag;
	unsigned long reclaimed;
	bool dirty_or_wb = false;
	int tries;

	pcache_pks_scope_clamp(&pks_scope);
	noreclaim_flag = memalloc_noreclaim_save();

	lru_add_drain();
	reclaimed = pcache_pks_scan_pool(gfp, false, &dirty_or_wb);
	if (reclaimed > 0 || atomic_long_read(&pool_free_count) > 0)
		goto out;

	if ((gfp & __GFP_FS) && !(gfp & __GFP_NORETRY)) {
		lru_add_drain_all();
		dirty_or_wb = false;
		reclaimed = pcache_pks_scan_pool(gfp, true, &dirty_or_wb);
		if (reclaimed > 0 || atomic_long_read(&pool_free_count) > 0)
			goto out;
	}

	if (dirty_or_wb &&
	    (gfp & (__GFP_FS | __GFP_IO)) == (__GFP_FS | __GFP_IO) &&
	    !current->journal_info) {
		wakeup_flusher_threads(WB_REASON_VMSCAN);
		if (gfp & __GFP_NORETRY)
			goto out;

		for (tries = 0; tries < PCACHE_PKS_WRITEBACK_RETRIES; tries++) {
			if (fatal_signal_pending(current))
				break;
			schedule_timeout_uninterruptible(msecs_to_jiffies(10));
			if (atomic_long_read(&pool_free_count) > 0)
				break;
			lru_add_drain();
			dirty_or_wb = false;
			reclaimed = pcache_pks_scan_pool(gfp, true,
							 &dirty_or_wb);
			if (reclaimed > 0 ||
			    atomic_long_read(&pool_free_count) > 0 ||
			    !dirty_or_wb)
				break;
			if ((tries & 0xf) == 0xf)
				wakeup_flusher_threads(WB_REASON_VMSCAN);
		}
	}

out:
	memalloc_noreclaim_restore(noreclaim_flag);
	pcache_pks_scope_end(&pks_scope);
	return reclaimed > 0 || atomic_long_read(&pool_free_count) > 0;
}

struct folio *pcache_pks_alloc_folio(gfp_t gfp, unsigned int order)
{
	struct page *page;
	unsigned long flags;
	int attempts = 0;

	if (WARN_ON_ONCE(order != 0) || (gfp & __GFP_ZERO))
		return NULL;

retry:
	spin_lock_irqsave(&pool_lock, flags);
	if (!list_empty(&pool_free)) {
		page = list_first_entry(&pool_free, struct page, lru);
		if (WARN_ON_ONCE(!PageReserved(page) || page_count(page) ||
				 PageLRU(page) || PageCompound(page))) {
			spin_unlock_irqrestore(&pool_lock, flags);
			return NULL;
		}
		list_del(&page->lru);
		atomic_long_dec(&pool_free_count);
		spin_unlock_irqrestore(&pool_lock, flags);

		__ClearPageReserved(page);
		post_alloc_hook(page, 0, gfp);
		return page_folio(page);
	}
	spin_unlock_irqrestore(&pool_lock, flags);

	if (attempts++ < PCACHE_PKS_RECLAIM_ATTEMPTS &&
	    gfpflags_allow_blocking(gfp) &&
	    !(current->flags & PF_MEMALLOC)) {
		if (pcache_pks_reclaim_pages(gfp))
			goto retry;
	}

	if (!(gfp & __GFP_NOWARN))
		pr_warn_ratelimited("pcache_pks: pool exhausted\n");
	return NULL;
}
EXPORT_SYMBOL_GPL(pcache_pks_alloc_folio);

void pcache_pks_recycle_page(struct page *page)
{
	unsigned long flags;

	if (WARN_ON_ONCE(!pcache_pks_page(page)))
		return;

	__SetPageReserved(page);
	spin_lock_irqsave(&pool_lock, flags);
	list_add(&page->lru, &pool_free);
	atomic_long_inc(&pool_free_count);
	spin_unlock_irqrestore(&pool_lock, flags);
}

bool pcache_pks_fault_callback(struct pt_regs *regs, unsigned long address,
			       bool write)
{
	struct insn insn;

	/*
	 * PKS_KEY_PAGE_CACHE is configured write-disabled only; a read fault
	 * indicates unexpected key state and fails closed.
	 */
	if (!write)
		return false;

	/*
	 * In synchronous user-task context the offending task dies as well, on
	 * its way back to user mode. Exiting from this exception instead would
	 * abandon every lock, page reference and preempt count its kernel path
	 * holds, so that path has to unwind normally.
	 */
	if (!in_interrupt() && !(current->flags & PF_KTHREAD)) {
		pr_warn_ratelimited("pcache_pks: unauthorized write by '%s'[%d] at %px (IP %pS), killing task\n",
				    current->comm, current->pid,
				    (void *)address, (void *)regs->ip);
		force_sig(SIGKILL);
	}

	/*
	 * In every context, decode and advance past the faulting store
	 * instruction so that it never retires.
	 */
	if (insn_decode_kernel(&insn, (void *)regs->ip) == 0 &&
	    insn.length > 0 && insn.length <= MAX_INSN_SIZE) {
		pr_warn_ratelimited("pcache_pks: suppressed stray store at %pS\n",
				    (void *)regs->ip);
		regs->ip += insn.length;
		return true;
	}

	return false;
}
