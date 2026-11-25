// SPDX-License-Identifier: GPL-2.0
/*
 * Protected page cache: ZONE_PKS block lifecycle.
 *
 * Protected page-cache folios come from a dedicated zone, ZONE_PKS, whose
 * managed memory grows and shrinks in aligned 2 MiB and 4 MiB blocks on
 * demand. While a block resides in ZONE_PKS, its direct map carries
 * PKS_KEY_PAGE_CACHE and its write alias carries PKS_KEY_PAGE_CACHE_W on
 * every CPU.
 *
 *   promote:  take(home) -> key_set      -> flush -> rezone(PKS) -> free -> wmarks
 *   demote:   take(PKS)  -> rezone(home) -> key_clr -> flush     -> free -> wmarks
 */
#define pr_fmt(fmt) "pcache_pks: " fmt

#include <linux/gfp.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/compaction.h>
#include <linux/page-isolation.h>
#include <linux/page_owner.h>
#include <linux/page_ref.h>
#include <linux/sched/mm.h>
#include <linux/shrinker.h>
#include <linux/spinlock.h>
#include <linux/swap.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>
#include <asm/set_memory.h>
#include <asm/tlbflush.h>
#include "../internal.h"
#include "internal.h"

#define PKS_SYNC_GROW_PAGES	(2 * pageblock_nr_pages)
#define PKS_PROMOTE_MAX		(64 * pageblock_nr_pages)
#define PKS_MIN_ACTIVE_RESERVE	(8 * pageblock_nr_pages)
#define PKS_SHRINKER_BATCH	(8 * pageblock_nr_pages)
#define PKS_TAKE_RETRIES	4

static struct workqueue_struct *pks_zone_wq __ro_after_init;
static struct work_struct pks_balance_work[MAX_NUMNODES];

static unsigned long pks_block_addr(struct page *block)
{
	return (unsigned long)page_address(block);
}

static unsigned long pks_block_size(struct page *block)
{
	return PAGE_SIZE << page_private(block);
}

/* Return the physical home zone [ZONE_DMA..ZONE_NORMAL] that owns @page. */
static struct zone *pks_home_zone(struct page *page)
{
	struct pglist_data *pgdat = page_pgdat(page);
	unsigned long pfn = page_to_pfn(page);
	int zi;

	for (zi = ZONE_NORMAL; zi >= 0; zi--) {
		struct zone *zone = &pgdat->node_zones[zi];

		if (!zone_is_pks(zone) && zone_spans_pfn(zone, pfn))
			return zone;
	}
	WARN_ON_ONCE(1);
	return &pgdat->node_zones[ZONE_NORMAL];
}

/* Lockless zone update: preserve concurrent flags such as HWPoison. */
static void pks_set_page_zone(struct page *page, enum zone_type zid)
{
	unsigned long flags, old = READ_ONCE(page->flags);

	do {
		flags = old & ~(ZONES_MASK << ZONES_PGSHIFT);
		flags |= (unsigned long)(zid & ZONES_MASK) << ZONES_PGSHIFT;
	} while (!try_cmpxchg(&page->flags, &old, flags));
}

/*
 * Transfer @nr pages of @block into @to while holding the source zone lock
 * against concurrent pageblock isolation.
 */
static bool pks_rezone_range(struct page *block, unsigned long nr,
			     struct zone *to)
{
	struct zone *from = page_zone(block);
	unsigned long i, flags;
	bool isolated = false;

	if (from == to)
		return true;

	spin_lock_irqsave(&from->lock, flags);
	for (i = 0; i < nr; i += pageblock_nr_pages)
		isolated |= is_migrate_isolate_page(block + i);
	if (!isolated) {
		for (i = 0; i < nr; i++)
			pks_set_page_zone(block + i, zone_idx(to));
		for (i = 0; i < nr; i += pageblock_nr_pages)
			set_pageblock_migratetype(block + i, MIGRATE_MOVABLE);
	}
	spin_unlock_irqrestore(&from->lock, flags);
	return !isolated;
}

static void pks_free_block(struct page *block)
{
	unsigned int order = page_private(block);

	list_del(&block->lru);
	set_page_private(block, 0);
	__free_pages_core(block, order);
}

static void pks_free_blocks(struct list_head *blocks)
{
	struct page *block, *next;

	list_for_each_entry_safe(block, next, blocks, lru)
		pks_free_block(block);
}

#ifdef CONFIG_PCACHE_PKS_DEBUG
static void pks_audit_blocks(struct list_head *blocks, int dm_pkey,
			     int alias_pkey)
{
	struct page *block;

	list_for_each_entry(block, blocks, lru) {
		unsigned long addr = pks_block_addr(block);
		unsigned long size = pks_block_size(block);

		pks_audit_range(addr, size, dm_pkey);
		if (alias_pkey)
			pks_audit_range(pks_alias_addr(addr), size, alias_pkey);
	}
}
#else
static inline void pks_audit_blocks(struct list_head *blocks, int dm_pkey,
				    int alias_pkey)
{
}
#endif

/*
 * The alias and the span of ZONE_PKS cover the memory that was there at boot.
 * Memory added later stays in its home zone.
 */
static bool pks_block_spanned(struct page *block, unsigned int order)
{
	struct zone *zone = page_pgdat(block)->node_zones + ZONE_PKS;
	unsigned long pfn = page_to_pfn(block);

	return zone_spans_pfn(zone, pfn) &&
	       zone_spans_pfn(zone, pfn + (1UL << order) - 1);
}

/*
 * Withdraw whole pageblock-aligned blocks from the zone selected by @gfp on
 * @nid, largest order first. Held blocks belong solely to the caller and count
 * toward no zone's managed_pages while in flight.
 */
static unsigned long pks_take(int nid, gfp_t gfp, unsigned long want,
			      struct list_head *blocks)
{
	gfp_t base_gfp = gfp | __GFP_MOVABLE | __GFP_THISNODE | __GFP_NORETRY |
			 __GFP_NOMEMALLOC | __GFP_NOWARN;
	unsigned long got = 0;
	unsigned int order;

	for (order = MAX_ORDER - 1; order >= pageblock_order; order--) {
		/* Larger than a pageblock is welcome, not worth reclaim */
		gfp_t cur_gfp = order > pageblock_order ?
				base_gfp & ~__GFP_RECLAIM : base_gfp;
		unsigned int retries = PKS_TAKE_RETRIES;

		while (got + (1UL << order) <= want) {
			struct page *block = __alloc_pages(cur_gfp, order, nid,
							   NULL);

			if (!block)
				break;
			/*
			 * CMA must stay movable, so avoid promoting it; retry
			 * without __GFP_MOVABLE once.
			 */
			if (is_migrate_cma_page(block)) {
				__free_pages(block, order);
				if (cur_gfp & __GFP_MOVABLE) {
					cur_gfp &= ~__GFP_MOVABLE;
					continue;
				}
				break;
			}
			/*
			 * GUP-fast and lockless page-cache lookup take a
			 * reference before they check that the page is still
			 * theirs. Such a reference must not survive the move:
			 * pks_free_block() resets the counts.
			 */
			if (!page_ref_freeze(block, 1)) {
				__free_pages(block, order);
				if (retries--)
					continue;
				break;
			}
			atomic_long_sub(1UL << order,
					&page_zone(block)->managed_pages);
			if (!pks_block_spanned(block, order)) {
				/* To the tail: the next call gets another */
				__free_pages_core(block, order);
				break;
			}
			set_page_private(block, order);
			list_add_tail(&block->lru, blocks);
			got += 1UL << order;
		}
	}
	return got;
}

static int pks_block_set_keys(struct page *block, bool *flush_all)
{
	unsigned long addr = pks_block_addr(block);
	unsigned long end = addr + pks_block_size(block);
	int err = 0;

	for (; addr < end && !err; addr += PMD_SIZE) {
		err = set_memory_pkey_noflush(addr, PTRS_PER_PTE,
					      PKS_KEY_PAGE_CACHE, flush_all);
		if (!err)
			err = pks_alias_map(addr, PMD_SIZE);
	}
	return err;
}

static int pks_block_clear_keys(struct page *block, bool *flush_all)
{
	unsigned long addr = pks_block_addr(block);
	unsigned long end = addr + pks_block_size(block);
	int err = 0;

	for (; addr < end && !err; addr += PMD_SIZE) {
		pks_alias_unmap(addr, PMD_SIZE, flush_all);
		err = set_memory_pkey_noflush(addr, PTRS_PER_PTE, 0, flush_all);
	}
	return err;
}

/* Tag every block of @blocks. On failure any number of them may be tagged. */
static int pks_protect_blocks(struct list_head *blocks, bool *flush_all)
{
	/* CPA allocates page tables to split leaves; reclaim must not recurse */
	unsigned int noreclaim = memalloc_noreclaim_save();
	struct page *block;
	int err = 0;

	vm_unmap_aliases();
	list_for_each_entry(block, blocks, lru) {
		err = pks_block_set_keys(block, flush_all);
		if (err)
			break;
	}
	memalloc_noreclaim_restore(noreclaim);
	return err;
}

static void pks_flush_blocks(void *info)
{
	struct list_head *blocks = info;
	struct page *block;
	unsigned long addr, end;

	list_for_each_entry(block, blocks, lru) {
		addr = pks_block_addr(block);
		end = addr + pks_block_size(block);
		for (; addr < end; addr += PMD_SIZE) {
			flush_tlb_one_kernel(addr);
			flush_tlb_one_kernel(pks_alias_addr(addr));
		}
	}
}

/* Flush one round across all CPUs for every 2 MiB leaf in @blocks. */
static void pks_flush(struct list_head *blocks, bool flush_all)
{
	if (list_empty(blocks) && !flush_all)
		return;
	if (flush_all)
		flush_tlb_all();
	else
		on_each_cpu(pks_flush_blocks, blocks, 1);
	pks_stat_inc(PKS_STAT_FLUSH);
}

static bool pks_chunk_mixed_home(struct page *block, unsigned int order,
				 struct zone **home_out)
{
	unsigned long off, nr = 1UL << order;
	struct zone *home = pks_home_zone(block);

	*home_out = home;
	for (off = pageblock_nr_pages; off < nr; off += pageblock_nr_pages) {
		if (pks_home_zone(block + off) != home)
			return true;
	}
	return false;
}

/*
 * Unpublish @blocks from ZONE_PKS into their physical home zones. If an
 * order-10 block straddles a zone boundary, split it into pageblock_order
 * halves first so each half rejoins its own home zone. Any block isolated
 * concurrently stays in ZONE_PKS via @kept.
 */
static unsigned long pks_rezone_to_home(struct list_head *blocks,
					struct list_head *kept)
{
	struct page *block, *next;
	unsigned long moved = 0;
	LIST_HEAD(rezoned);

	list_for_each_entry_safe(block, next, blocks, lru) {
		unsigned int order = page_private(block);
		unsigned long off, nr = 1UL << order;
		struct zone *home;

		if (!pks_chunk_mixed_home(block, order, &home)) {
			if (pks_rezone_range(block, nr, home)) {
				list_move_tail(&block->lru, &rezoned);
				moved += nr;
			} else {
				list_move_tail(&block->lru, kept);
			}
			continue;
		}

		list_del(&block->lru);
		split_page_owner(block, nr);
		for (off = 0; off < nr; off += pageblock_nr_pages) {
			struct page *sub = block + off;

			set_page_private(sub, pageblock_order);
			if (pks_rezone_range(sub, pageblock_nr_pages,
					     pks_home_zone(sub))) {
				list_add_tail(&sub->lru, &rezoned);
				moved += pageblock_nr_pages;
			} else {
				list_add_tail(&sub->lru, kept);
			}
		}
	}
	list_splice(&rezoned, blocks);
	return moved;
}

/* Clear the keys of @blocks, which are in their home zones, and free them. */
static void pks_release_home_blocks(struct list_head *blocks, bool flush_all)
{
	unsigned int noreclaim = memalloc_noreclaim_save();
	struct page *block, *next;

	list_for_each_entry_safe(block, next, blocks, lru) {
		if (WARN_ON_ONCE(pks_block_clear_keys(block, &flush_all))) {
			/* Leak it: it may still be tagged */
			list_del(&block->lru);
			flush_all = true;
		}
	}
	memalloc_noreclaim_restore(noreclaim);

	pks_flush(blocks, flush_all);
	pks_audit_blocks(blocks, 0, 0);
	pks_free_blocks(blocks);
}

/* Publish protected @blocks into @zone, returning isolated blocks in @isolated. */
static unsigned long pks_rezone_to_pks(struct zone *zone,
				       struct list_head *blocks,
				       struct list_head *isolated)
{
	struct page *block, *next;
	unsigned long moved = 0;

	list_for_each_entry_safe(block, next, blocks, lru) {
		unsigned long bnr = 1UL << page_private(block);

		if (pks_rezone_range(block, bnr, zone)) {
			pks_free_block(block);
			moved += bnr;
		} else {
			list_move_tail(&block->lru, isolated);
		}
	}
	return moved;
}

/* Move up to @want pages into ZONE_PKS: protect, flush, rezone, free, wmarks. */
static unsigned long pks_promote(struct zone *zone, gfp_t gfp,
				 unsigned long want)
{
	bool flush_all = false;
	LIST_HEAD(blocks);
	LIST_HEAD(isolated);
	unsigned long moved;

	if (!pks_take(zone_to_nid(zone), gfp | __GFP_ZERO, want, &blocks))
		return 0;

	if (pks_protect_blocks(&blocks, &flush_all)) {
		pks_release_home_blocks(&blocks, true);
		return 0;
	}
	pks_flush(&blocks, flush_all);
	pks_audit_blocks(&blocks, PKS_KEY_PAGE_CACHE, PKS_KEY_PAGE_CACHE_W);

	moved = pks_rezone_to_pks(zone, &blocks, &isolated);
	if (!list_empty(&isolated))
		pks_release_home_blocks(&isolated, false);
	if (moved) {
		pks_stat_add(PKS_STAT_TAG, moved >> pageblock_order);
		pks_update_zone_wmarks(zone);
	}
	return moved;
}

/* Return up to @want whole free pages from ZONE_PKS to their home zones. */
static unsigned long pks_demote(struct zone *zone, unsigned long want)
{
	unsigned long moved = 0;
	LIST_HEAD(blocks);
	LIST_HEAD(kept);

	if (!pks_take(zone_to_nid(zone), __GFP_PKS, want, &blocks))
		return 0;

	moved = pks_rezone_to_home(&blocks, &kept);
	pks_free_blocks(&kept);
	if (!list_empty(&blocks))
		pks_release_home_blocks(&blocks, false);
	if (moved) {
		pks_stat_add(PKS_STAT_UNTAG, moved >> pageblock_order);
		pks_update_zone_wmarks(zone);
	}
	return moved;
}

/* Synchronous promotion for an allocation that found ZONE_PKS exhausted. */
bool pks_zone_grow(struct zone *zone, gfp_t gfp)
{
	DEFINE_PCACHE_PKS_SCOPE(scope);
	unsigned long nr;

	if (!pcache_pks_is_enabled() || !zone_is_initialized(zone) ||
	    pks_static_placement())
		return false;

	pcache_pks_scope_clamp(&scope);
	nr = pks_promote(zone, gfp & (__GFP_RECLAIM | __GFP_IO | __GFP_FS),
			 PKS_SYNC_GROW_PAGES);
	pcache_pks_scope_end(&scope);
	if (!nr)
		pks_stat_inc(PKS_STAT_GROW_FAIL);
	else if (!zone_watermark_ok(zone, 0, high_wmark_pages(zone), ZONE_PKS, 0))
		pks_zone_wake(zone);
	return nr > 0;
}

/*
 * Asynchronous refill worker for ZONE_PKS, analogous to kswapd. Promotes one
 * coalesced batch toward the high watermark so a single TLB flush covers up to
 * PKS_PROMOTE_MAX pages.
 */
static void pks_balance(struct work_struct *work)
{
	int nid = work - pks_balance_work;
	struct zone *zone = NODE_DATA(nid)->node_zones + ZONE_PKS;
	unsigned long free, high, deficit, want;

	if (!zone_watermark_ok(zone, 0, high_wmark_pages(zone), ZONE_PKS, 0)) {
		free = zone_page_state(zone, NR_FREE_PAGES);
		high = high_wmark_pages(zone);
		deficit = (high > free) ?
			round_up(high - free, pageblock_nr_pages) : 0;
		want = clamp(max(zone_managed_pages(zone) / 8, deficit),
			     (unsigned long)PKS_SYNC_GROW_PAGES,
			     (unsigned long)PKS_PROMOTE_MAX);
		pks_promote(zone, __GFP_KSWAPD_RECLAIM, want);
	}
}

void pks_zone_wake(struct zone *zone)
{
	if (pcache_pks_is_enabled() && pks_zone_wq && zone_is_initialized(zone) &&
	    !pks_static_placement())
		queue_work(pks_zone_wq, pks_balance_work + zone_to_nid(zone));
}

/* Pages of @zone in pagesets: free, but not on its free lists. */
static unsigned long pks_pcp_pages(struct zone *zone)
{
	unsigned long held = 0;
	int cpu;

	for_each_online_cpu(cpu)
		held += READ_ONCE(per_cpu_ptr(zone->per_cpu_pageset,
					      cpu)->count);
	return held;
}

static bool pks_zone_idle(struct zone *zone, unsigned long held)
{
	return zone_page_state(zone, NR_FREE_PAGES) + held >=
	       zone_managed_pages(zone);
}

/*
 * How many of @free pages ZONE_PKS can do without. A zone in use keeps some to
 * allocate from; an idle one gives up everything.
 */
static unsigned long pks_spare_pages(struct zone *zone, unsigned long free,
				     unsigned long held)
{
	unsigned long floor = 0;

	if (!pks_zone_idle(zone, held))
		floor = max(high_wmark_pages(zone),
			    (unsigned long)PKS_MIN_ACTIVE_RESERVE);
	return free > floor ?
	       round_down(free - floor, pageblock_nr_pages) : 0;
}

/* Free pages that form whole blocks. */
static unsigned long pks_whole_pages(struct zone *zone)
{
	unsigned long whole = 0;
	unsigned int order;

	for (order = pageblock_order; order < MAX_ORDER; order++)
		whole += READ_ONCE(zone->free_area[order].nr_free) << order;
	return whole;
}

static unsigned long pks_shrink_count(struct shrinker *s,
				      struct shrink_control *sc)
{
	struct zone *zone = NODE_DATA(sc->nid)->node_zones + ZONE_PKS;
	unsigned long held;

	if (!zone_managed_pages(zone) || pks_static_placement())
		return 0;
	held = pks_pcp_pages(zone);
	return pks_spare_pages(zone, zone_page_state(zone, NR_FREE_PAGES) + held,
			       held);
}

static unsigned long pks_shrink_scan(struct shrinker *s,
				     struct shrink_control *sc)
{
	struct zone *zone = NODE_DATA(sc->nid)->node_zones + ZONE_PKS;
	DEFINE_PCACHE_PKS_SCOPE(scope);
	unsigned long free, held, spare, nr;

	pcache_pks_scope_clamp(&scope);
	held = pks_pcp_pages(zone);
	free = zone_page_state(zone, NR_FREE_PAGES);
	spare = min(pks_spare_pages(zone, free, held), sc->nr_to_scan);
	nr = pks_demote(zone, min(spare, pks_whole_pages(zone)));
	/* Drain pagesets that hold a block's worth, or the last pages in use */
	if (nr < sc->nr_to_scan &&
	    (held >= pageblock_nr_pages || pks_zone_idle(zone, held))) {
		drain_all_pages(zone);
		held = 0;
		free = zone_page_state(zone, NR_FREE_PAGES);
		spare = min(pks_spare_pages(zone, free, held),
			    sc->nr_to_scan - nr);
		nr += pks_demote(zone, min(spare, pks_whole_pages(zone)));
	}
	pcache_pks_scope_end(&scope);

	/* Much of the zone is free, but in pieces smaller than a block */
	free = zone_page_state(zone, NR_FREE_PAGES);
	if (pks_spare_pages(zone, free, held) >
	    pks_whole_pages(zone) + zone_managed_pages(zone) / 8)
		wakeup_kcompactd(zone->zone_pgdat, pageblock_order, ZONE_PKS);

	if (current->reclaim_state)
		current->reclaim_state->reclaimed_slab += nr;
	return nr ?: SHRINK_STOP;
}

static struct shrinker pks_shrinker = {
	.count_objects	= pks_shrink_count,
	.scan_objects	= pks_shrink_scan,
	.seeks		= DEFAULT_SEEKS,
	.flags		= SHRINKER_NUMA_AWARE,
};

void pks_zone_info(struct pks_zone_info *info)
{
	struct pglist_data *pgdat;

	memset(info, 0, sizeof(*info));
	for_each_online_pgdat(pgdat) {
		struct zone *zone = pgdat->node_zones + ZONE_PKS;

		info->tagged_pages += zone_managed_pages(zone);
		info->free_pages += zone_page_state(zone, NR_FREE_PAGES);
	}
}

#ifdef CONFIG_PCACHE_PKS_BASELINES
void __init pks_zone_reserve(unsigned long bytes)
{
	unsigned long want = round_up(bytes >> PAGE_SHIFT, pageblock_nr_pages);
	unsigned long per_node, got = 0;
	int nr_nodes = num_online_nodes();
	struct pglist_data *pgdat;

	if (!want || !nr_nodes)
		return;
	per_node = max_t(unsigned long, pageblock_nr_pages,
			 round_up(want / nr_nodes, pageblock_nr_pages));
	for_each_online_pgdat(pgdat) {
		struct zone *zone = pgdat->node_zones + ZONE_PKS;
		unsigned long rem = min(per_node, want - got);

		while (rem) {
			unsigned long step = min_t(unsigned long, rem,
						   PKS_PROMOTE_MAX);
			unsigned long n = pks_promote(zone, GFP_KERNEL, step);

			got += n;
			rem -= min(rem, n);
			if (!n)
				break;
		}
		if (got >= want)
			break;
	}
	/* The limit of the pagesets follows the size of the reservation */
	setup_per_zone_wmarks();
	if (got < want)
		pr_warn("static placement reserved only %lu of %lu MiB\n",
			got >> (20 - PAGE_SHIFT), bytes >> 20);
	else
		pr_info("static placement reserved %lu MiB\n",
			got >> (20 - PAGE_SHIFT));
}
#endif

int __init pks_zone_init(void)
{
	struct pglist_data *pgdat;
	int err, i;

	BUILD_BUG_ON(pageblock_order < PMD_SHIFT - PAGE_SHIFT);

	pks_zone_wq = alloc_workqueue("pks_zone",
				      WQ_UNBOUND | WQ_MEM_RECLAIM, 1);
	if (!pks_zone_wq)
		return -ENOMEM;

	for_each_online_pgdat(pgdat) {
		struct zone *zone = pgdat->node_zones + ZONE_PKS;

		INIT_WORK(pks_balance_work + pgdat->node_id, pks_balance);
		init_currently_empty_zone(zone, pgdat->node_start_pfn,
					  pgdat->node_spanned_pages);
		setup_zone_pageset(zone);
		zone->spanned_pages = pgdat->node_spanned_pages;
		zone->present_pages = 0;

		for (i = 0; i <= ZONE_NORMAL; i++)
			clear_zone_contiguous(pgdat->node_zones + i);
	}

	pks_shrinker.batch = PKS_SHRINKER_BATCH;
	err = register_shrinker(&pks_shrinker);
	if (err) {
		destroy_workqueue(pks_zone_wq);
		pks_zone_wq = NULL;
		return err;
	}

	setup_per_zone_wmarks();
	return 0;
}
