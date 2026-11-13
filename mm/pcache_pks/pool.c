// SPDX-License-Identifier: GPL-2.0
/*
 * Protected page cache: the block pool.
 *
 * This file decides where protected pages come from and when their memory gets
 * its protection keys. The pool takes 2 MiB (PMD) blocks from the buddy
 * allocator on demand, tags the direct-map leaf of each block with
 * PKS_KEY_PAGE_CACHE, maps its write alias, and returns blocks under memory
 * pressure.
 *
 * The pool is layered like slab: a per-CPU magazine of ascending-PFN pages in
 * front of a global list of blocks, each block tracking its free order-0 pages
 * in a 512-bit bitmap, in front of the buddy allocator. A shrinker returns
 * whole empty blocks to the buddy allocator and evacuates sparse ones under
 * pressure.
 */
#define pr_fmt(fmt) "pcache_pks: " fmt

#include <linux/bitmap.h>
#include <linux/cpu.h>
#include <linux/cpuhotplug.h>
#include <linux/gfp.h>
#include <linux/jiffies.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/page_owner.h>
#include <linux/pagemap.h>
#include <linux/printk.h>
#include <linux/rmap.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/sched/signal.h>
#include <linux/seq_file.h>
#include <linux/shrinker.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/swap.h>
#include <linux/vmalloc.h>
#include <linux/vmstat.h>
#include <linux/workqueue.h>
#include <linux/writeback.h>
#include <asm/set_memory.h>
#include <asm/tlbflush.h>
#include "../internal.h"
#include "internal.h"

#define PKS_MAG_SIZE		64
#define PKS_GROW_MAX		64		/* up to 128 MiB tagged per batch */
#define PKS_GROW_TRIES		3
#define PKS_EVICT_SCAN		16		/* partial blocks inspected per pass */
#define PKS_WRITEBACK_RETRIES	200

enum pks_list {
	PKS_LIST_NONE,
	PKS_LIST_PARTIAL,
	PKS_LIST_FULL,
	PKS_LIST_EMPTY,
};

struct pks_block {
	unsigned long		base_pfn;	/* first pfn, aligned to block */
	unsigned int		nr_free;	/* 0 .. PKS_BLOCK_PAGES */
	enum pks_list		where;		/* which list it is on */
	bool			isolated;	/* owned by evacuator/releaser */
	struct list_head	list;
	DECLARE_BITMAP(free, PKS_BLOCK_PAGES);	/* 1 = free in pool */
};

struct pks_magazine {
	unsigned int		nr_alloc;	/* ascending pages ready to hand out */
	unsigned int		nr_free;	/* freed pages awaiting drain */
	struct page		*alloc[PKS_MAG_SIZE];
	struct page		*free[PKS_MAG_SIZE];
	struct work_struct	drain_work;
};

static struct {
	spinlock_t		lock;
	struct list_head	partial;
	struct list_head	full;
	struct list_head	empty;
	unsigned long		nr_blocks;
	unsigned long		nr_empty;
	unsigned long		nr_free_pages;
} pks_pool = {
	.lock	 = __SPIN_LOCK_UNLOCKED(pks_pool.lock),
	.partial = LIST_HEAD_INIT(pks_pool.partial),
	.full	 = LIST_HEAD_INIT(pks_pool.full),
	.empty	 = LIST_HEAD_INIT(pks_pool.empty),
};

static DEFINE_MUTEX(pks_tag_mutex);		/* serializes tag/untag batches */

/*
 * Batch scratch space for growth and release, owned by whoever holds
 * pks_tag_mutex. Kept off the stack: growth calls alloc_pages() with direct
 * reclaim, and release runs inside a shrinker, both on deep call chains.
 */
static struct {
	struct pks_block	*blocks[PKS_GROW_MAX];
	unsigned long		addrs[PKS_GROW_MAX];
	unsigned long		flush[2 * PKS_GROW_MAX];
	bool			stuck[PKS_GROW_MAX];
} pks_batch;
static DEFINE_PER_CPU(struct pks_magazine, pks_mags);

struct pks_block **pcache_pks_block_map __read_mostly;
EXPORT_SYMBOL_GPL(pcache_pks_block_map);

unsigned long pcache_pks_nr_blocks_max __read_mostly;
EXPORT_SYMBOL_GPL(pcache_pks_nr_blocks_max);

static inline struct pks_block *pks_block_of(unsigned long pfn)
{
	return READ_ONCE(pcache_pks_block_map[pfn >> PKS_BLOCK_ORDER]);
}

/* Must hold pks_pool.lock. Move @b onto @target, keeping nr_empty accurate. */
static void pks_set_list(struct pks_block *b, enum pks_list target)
{
	if (b->isolated || b->where == target)
		return;
	switch (b->where) {
	case PKS_LIST_EMPTY:
		pks_pool.nr_empty--;
		fallthrough;
	case PKS_LIST_PARTIAL:
	case PKS_LIST_FULL:
		list_del(&b->list);
		break;
	default:
		break;
	}
	switch (target) {
	case PKS_LIST_PARTIAL:
		list_add(&b->list, &pks_pool.partial);
		break;
	case PKS_LIST_FULL:
		list_add(&b->list, &pks_pool.full);
		break;
	case PKS_LIST_EMPTY:
		list_add(&b->list, &pks_pool.empty);
		pks_pool.nr_empty++;
		break;
	default:
		break;
	}
	b->where = target;
}

/* Must hold pks_pool.lock. Return one freed page to its block's bitmap. */
static long pks_return_one(struct page *page)
{
	unsigned long pfn = page_to_pfn(page);
	struct pks_block *b = pks_block_of(pfn);
	unsigned int bit;

	if (WARN_ON_ONCE(!b))
		return 0;
	bit = pfn - b->base_pfn;
	if (WARN_ON_ONCE(test_bit(bit, b->free)))
		return 0;
	__set_bit(bit, b->free);
	b->nr_free++;
	pks_pool.nr_free_pages++;
	pks_set_list(b, b->nr_free == PKS_BLOCK_PAGES ? PKS_LIST_EMPTY :
			PKS_LIST_PARTIAL);
	return 1;
}

/* IRQs disabled. Flush a CPU's magazine back to block bitmaps. */
static void pks_drain(struct pks_magazine *m, bool include_alloc)
{
	unsigned long flags;
	long returned = 0;
	unsigned int i;

	if (!m->nr_free && (!include_alloc || !m->nr_alloc))
		return;

	spin_lock_irqsave(&pks_pool.lock, flags);
	if (include_alloc) {
		for (i = 0; i < m->nr_alloc; i++)
			returned += pks_return_one(m->alloc[i]);
		m->nr_alloc = 0;
	}
	for (i = 0; i < m->nr_free; i++)
		returned += pks_return_one(m->free[i]);
	m->nr_free = 0;
	spin_unlock_irqrestore(&pks_pool.lock, flags);

	if (returned)
		mod_node_page_state(NODE_DATA(first_online_node),
				    NR_KERNEL_MISC_RECLAIMABLE, returned);
}

static void pks_drain_work_fn(struct work_struct *work)
{
	unsigned long flags;

	/*
	 * Drain the magazine of the CPU this runs on, not the one the work was
	 * queued for: if that CPU went offline first, the work runs elsewhere,
	 * and the dead CPU's magazine belongs to pks_cpu_offline() alone.
	 */
	local_irq_save(flags);
	pks_drain(this_cpu_ptr(&pks_mags), true);
	local_irq_restore(flags);
}

/* Must hold pks_pool.lock. Move up to @want ascending free pages of @b to @out. */
static unsigned int pks_take_locked(struct pks_block *b, struct page **out,
				    unsigned int want)
{
	unsigned int bit, got = 0;

	for_each_set_bit(bit, b->free, PKS_BLOCK_PAGES) {
		__clear_bit(bit, b->free);
		out[got++] = pfn_to_page(b->base_pfn + bit);
		if (got == want)
			break;
	}
	b->nr_free -= got;
	pks_pool.nr_free_pages -= got;
	pks_set_list(b, b->nr_free ? PKS_LIST_PARTIAL : PKS_LIST_FULL);
	return got;
}

/*
 * Must hold pks_pool.lock. Pick the block to allocate from: keep packing the
 * partial block at the head, and only then open an empty one, so whole free
 * blocks stay releasable.
 */
static struct pks_block *pks_next_block_locked(void)
{
	struct pks_block *b;

	b = list_first_entry_or_null(&pks_pool.partial, struct pks_block, list);
	if (!b) {
		b = list_first_entry_or_null(&pks_pool.empty, struct pks_block,
					     list);
		if (b)
			pks_set_list(b, PKS_LIST_PARTIAL);
	}
	return b;
}

/* IRQs disabled, @m->alloc[] empty. Fill it with ascending-PFN pages. */
static bool pks_refill(struct pks_magazine *m)
{
	struct pks_block *b;
	unsigned long flags;
	unsigned int i, got = 0;

	spin_lock_irqsave(&pks_pool.lock, flags);
	b = pks_next_block_locked();
	if (b)
		got = pks_take_locked(b, m->alloc, PKS_MAG_SIZE);
	spin_unlock_irqrestore(&pks_pool.lock, flags);
	if (!got)
		return false;

	mod_node_page_state(NODE_DATA(first_online_node),
			    NR_KERNEL_MISC_RECLAIMABLE, -(long)got);

	/* Reverse so the fast-path pop (alloc[--nr_alloc]) yields lowest PFN. */
	for (i = 0; i < got / 2; i++)
		swap(m->alloc[i], m->alloc[got - 1 - i]);
	m->nr_alloc = got;
	return true;
}

/* Take one free page from anywhere in the pool for the calling task. */
static struct page *pks_take_one(void)
{
	struct page *page = NULL;
	struct pks_block *b;
	unsigned long flags;

	spin_lock_irqsave(&pks_pool.lock, flags);
	b = pks_next_block_locked();
	if (b)
		pks_take_locked(b, &page, 1);
	spin_unlock_irqrestore(&pks_pool.lock, flags);

	if (page)
		mod_node_page_state(NODE_DATA(first_online_node),
				    NR_KERNEL_MISC_RECLAIMABLE, -1);
	return page;
}

static gfp_t pks_block_gfp(gfp_t gfp)
{
	/*
	 * Movable so compaction can relocate neighbours; no __GFP_HIGHMEM so the
	 * block never lands in ZONE_MOVABLE and always has a direct map. Zeroed
	 * before tagging so a later suppressed stray read cannot leak old data.
	 */
	return (gfp & (__GFP_RECLAIM | __GFP_IO | __GFP_FS | __GFP_NORETRY |
		       __GFP_RETRY_MAYFAIL)) |
	       __GFP_MOVABLE | __GFP_NOWARN | __GFP_NOMEMALLOC | __GFP_ZERO;
}

/*
 * Region affinity
 * ---------------
 * Tagging one 2 MiB block inside a 1 GiB direct-map leaf splits that leaf into
 * 512 entries, and nothing in this kernel merges them again. If growth takes
 * its blocks wherever the buddy allocator has one, the pool ends up having
 * split several times as much of the direct map as it ever held.
 *
 * A leaf that is already split costs nothing more to use. So growth first
 * looks for a free 2 MiB chunk inside a 1 GiB region that is split, because the
 * pool tagged a block there before or because the region was never mapped by
 * one leaf, and takes it straight off the buddy free list. Only when there is
 * none does it ask alloc_pages(), which may open a new region. This changes
 * where a block comes from, never whether growth succeeds.
 */
#define PKS_REGION_SHIFT	(PUD_SHIFT - PAGE_SHIFT)
#define PKS_REGION_PAGES	(1UL << PKS_REGION_SHIFT)

struct pks_region {
	unsigned long	scanned;	/* jiffies of the last scan that failed */
	u16		nr_blocks;	/* pool blocks in the region now */
	bool		split;		/* its 1 GiB leaf is known to be split */
	bool		full;		/* that scan found no free chunk */
};

/* All of this is protected by pks_tag_mutex. */
static struct pks_region *pks_regions;
static unsigned long pks_nr_regions;
static unsigned long pks_claim_cursor;	/* pfn after the last chunk claimed */
static bool pks_affine __ro_after_init = true;

static int __init setup_pcache_pks_affine(char *str)
{
	return kstrtobool(str, &pks_affine);
}
early_param("pcache_pks.affine", setup_pcache_pks_affine);

static inline struct pks_region *pks_region_of(unsigned long pfn)
{
	return &pks_regions[pfn >> PKS_REGION_SHIFT];
}

/*
 * Take one free 2 MiB chunk of region @r off the buddy free list, or return
 * NULL. Like an allocation, this respects the zone's minimum watermark; unlike
 * one, it never reclaims or compacts. The page comes back as alloc_pages()
 * would return it: reference count 1, owner recorded, zeroed if @gfp says so.
 */
static struct page *pks_claim_in_region(unsigned long r, gfp_t gfp)
{
	unsigned long pfn = r << PKS_REGION_SHIFT;
	unsigned long end = pfn + PKS_REGION_PAGES;

	if (pks_claim_cursor > pfn && pks_claim_cursor < end)
		pfn = pks_claim_cursor;

	for (; pfn < end; pfn += pageblock_nr_pages) {
		struct page *page = pfn_to_online_page(pfn);
		unsigned long flags;
		unsigned int order;
		struct zone *zone;

		/* An unlocked hint, checked again under the zone lock. */
		if (!page || !PageBuddy(page) ||
		    buddy_order_unsafe(page) < PKS_BLOCK_ORDER)
			continue;
		/* The same memory alloc_pages(__GFP_MOVABLE) would use. */
		zone = page_zone(page);
		if (zone_idx(zone) != ZONE_NORMAL && zone_idx(zone) != ZONE_DMA32)
			continue;
		if (get_pageblock_migratetype(page) != MIGRATE_MOVABLE)
			continue;

		/*
		 * Under the lock: the chunk is still free, its pageblock is
		 * still movable (not isolated in the meantime, and not CMA),
		 * and taking it leaves the zone what an allocation for
		 * ZONE_NORMAL would leave it, the reserve that a lower zone
		 * keeps for its own users included. __isolate_free_page()
		 * checks the minimum watermark without that reserve.
		 */
		spin_lock_irqsave(&zone->lock, flags);
		order = PageBuddy(page) ? buddy_order(page) : 0;
		if (order < PKS_BLOCK_ORDER ||
		    get_pageblock_migratetype(page) != MIGRATE_MOVABLE ||
		    !zone_watermark_ok(zone, 0,
				       min_wmark_pages(zone) + (1UL << order),
				       ZONE_NORMAL, 0) ||
		    !__isolate_free_page(page, order)) {
			spin_unlock_irqrestore(&zone->lock, flags);
			continue;
		}
		/* Of a 4 MiB chunk, give the upper half straight back. */
		if (order > PKS_BLOCK_ORDER)
			__putback_isolated_page(page + PKS_BLOCK_PAGES,
						PKS_BLOCK_ORDER,
						MIGRATE_MOVABLE);
		spin_unlock_irqrestore(&zone->lock, flags);

		post_alloc_hook(page, PKS_BLOCK_ORDER, gfp);
		pks_claim_cursor = pfn + pageblock_nr_pages;
		return page;
	}
	return NULL;
}

/* Find a block in a region that is already split: occupied regions first. */
static struct page *pks_claim_affine(gfp_t gfp)
{
	unsigned long r;
	int pass;

	lockdep_assert_held(&pks_tag_mutex);

	for (pass = 0; pass < 2; pass++) {
		for (r = 0; r < pks_nr_regions; r++) {
			struct pks_region *reg = &pks_regions[r];
			bool occupied = reg->nr_blocks;
			struct page *page;

			/* Pass 0 takes the occupied regions, pass 1 the rest. */
			if (!reg->split || occupied == (pass == 1))
				continue;
			/* Do not scan a region that had nothing a moment ago. */
			if (reg->full && time_before(jiffies, reg->scanned + HZ))
				continue;
			page = pks_claim_in_region(r, gfp);
			if (page) {
				reg->full = false;
				return page;
			}
			reg->full = true;
			reg->scanned = jiffies;
			pks_claim_cursor = 0;
		}
	}
	return NULL;
}

/*
 * Flush the translations of the @n blocks of pks_batch on every CPU: the
 * direct-map leaves, and with @alias the alias leaves as well.
 */
static void pks_flush_batch(unsigned int n, bool alias, bool flush_all)
{
	unsigned long *addrs = pks_batch.addrs;
	unsigned long *flush = pks_batch.flush;
	unsigned int i, nr = 0;

	if (flush_all) {
		flush_tlb_all();
	} else {
		for (i = 0; i < n; i++) {
			flush[nr++] = addrs[i];
			if (alias)
				flush[nr++] = pks_alias_addr(addrs[i]);
		}
		flush_tlb_kernel_leaves(flush, nr);
	}
	pks_stat_inc(PKS_STAT_FLUSH);
}

/*
 * Remove the alias and the key of every block of pks_batch. The caller
 * flushes. pks_batch.stuck[] marks the blocks whose key could not be removed.
 */
static void pks_untag_noflush(unsigned int n, bool *need_flush_all)
{
	unsigned long *addrs = pks_batch.addrs;
	unsigned int i;

	for (i = 0; i < n; i++) {
		pks_alias_unmap(addrs[i], PMD_SIZE, need_flush_all);
		pks_batch.stuck[i] =
			set_memory_pkey_noflush(addrs[i], PKS_BLOCK_PAGES, 0,
						need_flush_all) != 0;
	}
}

/*
 * Tag the @n blocks of pks_batch: give each direct-map leaf PKS_KEY_PAGE_CACHE,
 * map each write alias, and flush every CPU, so that no CPU can still write a
 * block through a stale direct-map translation once it is published. On
 * failure every block is untagged again, and pks_batch.stuck[] marks the ones
 * whose key could not be removed.
 *
 * Runs under noreclaim: the page-table pages allocated to split a 1 GiB leaf
 * or to map the alias must not recurse into fs reclaim or the shrinker while
 * pks_tag_mutex is held.
 */
static int pks_tag_batch(unsigned int n)
{
	unsigned long *addrs = pks_batch.addrs;
	bool need_flush_all = false;
	unsigned int noreclaim, i;
	int err = 0;

	lockdep_assert_held(&pks_tag_mutex);

	noreclaim = memalloc_noreclaim_save();
	/* No lazily freed vmalloc alias of these pages may keep a key-0 entry. */
	vm_unmap_aliases();
	for (i = 0; i < n && !err; i++)
		err = set_memory_pkey_noflush(addrs[i], PKS_BLOCK_PAGES,
					      PKS_KEY_PAGE_CACHE,
					      &need_flush_all);
	for (i = 0; i < n && !err; i++)
		err = pks_alias_map(addrs[i], PMD_SIZE);
	if (err) {
		/* Revert every block; one that was never tagged is a no-op. */
		pks_untag_noflush(n, &need_flush_all);
		pks_flush_batch(n, true, need_flush_all);
	} else {
		pks_flush_batch(n, false, need_flush_all);
	}
	memalloc_noreclaim_restore(noreclaim);

	return err;
}

/*
 * Untag the @n blocks of pks_batch and flush every CPU, so that the next owner
 * of this memory cannot take a key fault through a stale translation and
 * nothing can reach it through the alias.
 */
static void pks_untag_batch(unsigned int n)
{
	bool need_flush_all = false;

	lockdep_assert_held(&pks_tag_mutex);

	pks_untag_noflush(n, &need_flush_all);
	pks_flush_batch(n, true, need_flush_all);
}

/*
 * Give the @n isolated, fully free blocks of @batch back to the buddy
 * allocator. The order is load-bearing: the map entry is cleared first, so that
 * no lookup still treats the block as protected, and the block is untagged and
 * flushed before its pages are freed.
 */
static void pks_release_blocks(struct pks_block **batch, unsigned int n)
{
	unsigned long *addrs = pks_batch.addrs;
	unsigned int i;

	lockdep_assert_held(&pks_tag_mutex);

	for (i = 0; i < n; i++) {
		struct pks_block *b = batch[i];
		unsigned long idx = b->base_pfn >> PKS_BLOCK_ORDER;

		WARN_ON_ONCE(b->nr_free != PKS_BLOCK_PAGES);
		addrs[i] = (unsigned long)__va(PFN_PHYS(b->base_pfn));
		WRITE_ONCE(pcache_pks_block_map[idx], NULL);
		/* The region gets a free chunk back: worth scanning again. */
		pks_region_of(b->base_pfn)->nr_blocks--;
		pks_region_of(b->base_pfn)->full = false;
	}
	pks_untag_batch(n);

	mod_node_page_state(NODE_DATA(first_online_node),
			    NR_KERNEL_MISC_RECLAIMABLE,
			    -(long)(n * PKS_BLOCK_PAGES));

	for (i = 0; i < n; i++) {
		struct page *head = pfn_to_page(batch[i]->base_pfn);

		/* Never free a page whose key might still be set. */
		if (!WARN_ON_ONCE(pks_batch.stuck[i])) {
			pks_audit_range(addrs[i], PMD_SIZE, 0);
			set_page_refcounted(head);
			__free_pages(head, PKS_BLOCK_ORDER);
		}
		kfree(batch[i]);
	}
	pks_stat_add(PKS_STAT_UNTAG, n);
	if (current->reclaim_state)
		current->reclaim_state->reclaimed_slab += n * PKS_BLOCK_PAGES;
}

/*
 * Empty blocks kept back from the buddy allocator, so a stable working set
 * does not cycle blocks through tag and untag.
 */
static unsigned long pks_reserve_blocks(unsigned long blocks)
{
	return clamp_t(unsigned long, blocks / 32, 8, 256);
}

/* Free pool pages that sit in blocks which are not wholly free. */
static unsigned long pks_stranded_pages(void)
{
	unsigned long free = READ_ONCE(pks_pool.nr_free_pages);
	unsigned long in_empty = READ_ONCE(pks_pool.nr_empty) * PKS_BLOCK_PAGES;

	return free > in_empty ? free - in_empty : 0;
}

/*
 * Get one 2 MiB block: from a region that is already split if one has a free
 * chunk, otherwise from wherever the buddy allocator finds one.
 */
static struct page *pks_alloc_block(gfp_t gfp)
{
	struct page *page;

	if (pks_affine) {
		page = pks_claim_affine(gfp);
		if (page) {
			pks_stat_inc(PKS_STAT_GROW_AFFINE);
			return page;
		}
	}
	page = alloc_pages(gfp, PKS_BLOCK_ORDER);

	/*
	 * A movable allocation may be served from a CMA area. A pool block
	 * never moves, so it would make that area unusable for as long as the
	 * pool keeps the block. Take it from elsewhere.
	 */
	if (page && is_migrate_cma_page(page)) {
		__free_pages(page, PKS_BLOCK_ORDER);
		page = alloc_pages(gfp & ~__GFP_MOVABLE, PKS_BLOCK_ORDER);
	}
	return page;
}

/*
 * Grow the pool by a batch of 2 MiB blocks and return one page of it to the
 * caller, or NULL if no block could be obtained. Handing the grower its page
 * before the rest are published means a growth can never be lost to other
 * writers, so a NULL return always means memory, not contention.
 *
 * Without @force, nothing is grown if another grower has published blocks in
 * the meantime. A reservation passes @force, because it grows a pool that has
 * free pages.
 */
static struct page *pks_grow(gfp_t gfp, bool force)
{
	struct pks_block **blocks = pks_batch.blocks;
	unsigned long *addrs = pks_batch.addrs;
	gfp_t rgfp = gfp & (__GFP_RECLAIM | __GFP_IO | __GFP_FS);
	struct page *page = NULL;
	unsigned long want, i, n = 0, flags;
	unsigned int nofs;

	mutex_lock(&pks_tag_mutex);
	/*
	 * A writer inside a journal handle can wait for this mutex, and reclaim
	 * that enters the filesystem can wait for that handle. So nothing that
	 * is allocated while the mutex is held may reclaim through the
	 * filesystem. The caller's full reclaim rights are used after the
	 * mutex is dropped, by the reclaim inside the pool.
	 */
	nofs = memalloc_nofs_save();
	/* Another grower may have published blocks while we waited. */
	if (!force)
		page = pks_take_one();
	if (page)
		goto out_unlock;

	want = clamp_t(unsigned long, READ_ONCE(pks_pool.nr_blocks) / 8, 1,
		       PKS_GROW_MAX);

	for (i = 0; i < want; i++) {
		/* Only the first block uses the caller's reclaim rights. */
		gfp_t bgfp = i ? (GFP_NOWAIT | __GFP_NOWARN | __GFP_NORETRY |
				  __GFP_ZERO | __GFP_MOVABLE | __GFP_NOMEMALLOC)
			       : pks_block_gfp(gfp);
		gfp_t dgfp = i ? (GFP_NOWAIT | __GFP_NOWARN) : (rgfp | __GFP_NOWARN);
		struct pks_block *b;
		struct page *head;

		b = kzalloc(sizeof(*b), dgfp);
		if (!b)
			break;
		head = pks_alloc_block(bgfp);
		if (!head) {
			kfree(b);
			break;
		}
		/* The map is sized at boot; memory added later is not used. */
		if ((page_to_pfn(head) >> PKS_BLOCK_ORDER) >=
		    pcache_pks_nr_blocks_max) {
			__free_pages(head, PKS_BLOCK_ORDER);
			kfree(b);
			break;
		}
		/*
		 * Drop the head's reference atomically. A racing speculative get
		 * (a stale page-cache or GUP-fast lookup) may hold another, and a
		 * plain set_page_count() would erase it, letting that holder's
		 * put free a page the pool later hands out. __free_pages() copes
		 * with the extra reference.
		 */
		if (!page_ref_freeze(head, 1)) {
			__free_pages(head, PKS_BLOCK_ORDER);
			kfree(b);
			continue;
		}
		split_page_owner(head, PKS_BLOCK_PAGES);
		b->base_pfn = page_to_pfn(head);
		b->nr_free = PKS_BLOCK_PAGES;
		b->where = PKS_LIST_NONE;
		b->isolated = false;
		bitmap_fill(b->free, PKS_BLOCK_PAGES);
		INIT_LIST_HEAD(&b->list);
		addrs[n] = (unsigned long)page_address(head);
		blocks[n++] = b;
	}
	if (!n)
		goto out_unlock;

	if (pks_tag_batch(n)) {
		for (i = 0; i < n; i++) {
			struct page *head = pfn_to_page(blocks[i]->base_pfn);

			/* Never free a page whose key might still be set. */
			if (!WARN_ON_ONCE(pks_batch.stuck[i])) {
				set_page_refcounted(head);
				__free_pages(head, PKS_BLOCK_ORDER);
			}
			kfree(blocks[i]);
		}
		goto out_unlock;
	}

	for (i = 0; i < n; i++) {
		pks_audit_range(addrs[i], PMD_SIZE, PKS_KEY_PAGE_CACHE);
		pks_audit_range(pks_alias_addr(addrs[i]), PMD_SIZE,
				PKS_KEY_PAGE_CACHE_W);
	}

	/* Publish only after every CPU has the tagged translation. */
	spin_lock_irqsave(&pks_pool.lock, flags);
	for (i = 0; i < n; i++) {
		struct pks_block *b = blocks[i];

		/*
		 * Pairs with the READ_ONCE() in pcache_pks_page() and
		 * pks_block_of(): loads through the published pointer see the
		 * initialized block.
		 */
		smp_store_release(&pcache_pks_block_map[b->base_pfn >>
							PKS_BLOCK_ORDER], b);
		pks_region_of(b->base_pfn)->nr_blocks++;
		pks_region_of(b->base_pfn)->split = true;
		pks_pool.nr_blocks++;
		pks_pool.nr_free_pages += PKS_BLOCK_PAGES;
		pks_set_list(b, PKS_LIST_EMPTY);
	}
	pks_take_locked(blocks[0], &page, 1);
	spin_unlock_irqrestore(&pks_pool.lock, flags);

	mod_node_page_state(NODE_DATA(first_online_node),
			    NR_KERNEL_MISC_RECLAIMABLE,
			    n * PKS_BLOCK_PAGES - 1);
	pks_stat_add(PKS_STAT_TAG, n);
out_unlock:
	if (!page)
		pks_stat_inc(PKS_STAT_GROW_FAIL);
	memalloc_nofs_restore(nofs);
	mutex_unlock(&pks_tag_mutex);
	if (!page && !(gfp & __GFP_NOWARN))
		pr_warn_ratelimited("block growth failed\n");
	return page;
}

/*
 * Evict clean, unlocked, non-writeback LRU folios from one isolated block.
 * PG_locked is only well defined on LRU folios: the folio_test_lru() check
 * after folio_try_get() keeps folio_trylock() off a freshly allocated folio that
 * has not yet been through the non-atomic __folio_set_locked() in
 * filemap_add_folio(), whose lock bit it would otherwise clear.
 */
static unsigned long pks_evict_block(struct pks_block *b, gfp_t gfp, bool unmap,
				     bool *saw_dirty_or_wb)
{
	unsigned long i, reclaimed = 0;
	struct pks_magazine *m;
	unsigned long flags;

	for (i = 0; i < PKS_BLOCK_PAGES; i++) {
		struct page *page = pfn_to_page(b->base_pfn + i);
		struct address_space *mapping;
		struct folio *folio;
		bool evicted = false;

		if (test_bit(i, b->free))
			continue;
		if ((i & 0x3f) == 0x3f)
			cond_resched();
		if (PageUnevictable(page))
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
		if (evicted)
			reclaimed++;
	}

	/* Freed pages of @b came straight back; drain anything else parked. */
	local_irq_save(flags);
	m = this_cpu_ptr(&pks_mags);
	pks_drain(m, false);
	local_irq_restore(flags);
	pks_stat_add(PKS_STAT_EVICTED, reclaimed);
	return reclaimed;
}

/*
 * Isolate the sparsest of the first partial blocks (or a full one), evacuate
 * it, and put it back at the tail so successive passes rotate through blocks.
 * Mapped folios are only unmapped when @gfp allows fs reclaim.
 */
static unsigned long pks_evacuate_sparse(gfp_t gfp, bool *saw_dirty_or_wb)
{
	struct pks_block *best = NULL, *iter;
	unsigned int scanned = 0;
	unsigned long reclaimed, flags;

	spin_lock_irqsave(&pks_pool.lock, flags);
	list_for_each_entry(iter, &pks_pool.partial, list) {
		if (!best || iter->nr_free > best->nr_free)
			best = iter;
		if (++scanned >= PKS_EVICT_SCAN)
			break;
	}
	if (!best)
		best = list_first_entry_or_null(&pks_pool.full,
						struct pks_block, list);
	if (best) {
		/* Listed blocks are partial or full here, never empty. */
		list_del(&best->list);
		best->where = PKS_LIST_NONE;
		WRITE_ONCE(best->isolated, true);
	}
	spin_unlock_irqrestore(&pks_pool.lock, flags);
	if (!best)
		return 0;

	lru_add_drain();
	reclaimed = pks_evict_block(best, gfp, gfp & __GFP_FS,
				    saw_dirty_or_wb);

	spin_lock_irqsave(&pks_pool.lock, flags);
	WRITE_ONCE(best->isolated, false);
	if (best->nr_free == PKS_BLOCK_PAGES) {
		list_add(&best->list, &pks_pool.empty);
		best->where = PKS_LIST_EMPTY;
		pks_pool.nr_empty++;
	} else if (best->nr_free == 0) {
		list_add_tail(&best->list, &pks_pool.full);
		best->where = PKS_LIST_FULL;
	} else {
		list_add_tail(&best->list, &pks_pool.partial);
		best->where = PKS_LIST_PARTIAL;
	}
	spin_unlock_irqrestore(&pks_pool.lock, flags);
	return reclaimed;
}

/* Release up to @want_pages of empty blocks, keeping @reserve empty blocks. */
static unsigned long pks_release_empty(unsigned long reserve,
				       unsigned long want_pages)
{
	struct pks_block **batch = pks_batch.blocks;
	unsigned long want_blocks, flags;
	unsigned int n = 0;

	if (READ_ONCE(pks_pool.nr_empty) <= reserve)
		return 0;
	if (!mutex_trylock(&pks_tag_mutex))
		return 0;

	want_blocks = DIV_ROUND_UP(max(want_pages, 1UL), PKS_BLOCK_PAGES);
	spin_lock_irqsave(&pks_pool.lock, flags);
	while (n < want_blocks && n < PKS_GROW_MAX &&
	       pks_pool.nr_empty > reserve) {
		struct pks_block *b = list_first_entry(&pks_pool.empty,
						       struct pks_block, list);

		list_del(&b->list);
		b->where = PKS_LIST_NONE;
		WRITE_ONCE(b->isolated, true);
		pks_pool.nr_empty--;
		pks_pool.nr_blocks--;
		pks_pool.nr_free_pages -= PKS_BLOCK_PAGES;
		batch[n++] = b;
	}
	spin_unlock_irqrestore(&pks_pool.lock, flags);

	if (n)
		pks_release_blocks(batch, n);
	mutex_unlock(&pks_tag_mutex);
	return n * PKS_BLOCK_PAGES;
}

/* Push every CPU's parked pages back into the block bitmaps. May sleep. */
static void pks_drain_all_mags(void)
{
	unsigned int cpu;

	for_each_online_cpu(cpu) {
		struct pks_magazine *m = per_cpu_ptr(&pks_mags, cpu);

		if (READ_ONCE(m->nr_alloc) || READ_ONCE(m->nr_free))
			queue_work_on(cpu, mm_percpu_wq, &m->drain_work);
	}
	for_each_online_cpu(cpu)
		flush_work(&per_cpu_ptr(&pks_mags, cpu)->drain_work);
}

/*
 * When no order-9 block can be had (fragmentation, or a tight mem= boot) but
 * the pool holds blocks, find a page inside the pool: parked in a magazine,
 * evictable from a block, or -- last -- clean once writeback finishes. It stays
 * bounded and killable, and whatever it frees goes to this caller first, so a
 * sleeping allocator is not starved by writers that keep running.
 */
static struct page *pks_reclaim_inpool(gfp_t gfp)
{
	struct pks_magazine *m;
	struct page *page;
	unsigned long flags;
	bool dirty = false;
	int tries;

	pks_stat_inc(PKS_STAT_INPOOL);
	local_irq_save(flags);
	m = this_cpu_ptr(&pks_mags);
	pks_drain(m, false);
	local_irq_restore(flags);
	page = pks_take_one();
	if (page)
		return page;

	pks_drain_all_mags();
	page = pks_take_one();
	if (page)
		return page;

	lru_add_drain();
	pks_evacuate_sparse(gfp, &dirty);
	page = pks_take_one();
	if (page)
		return page;
	if (gfp & __GFP_FS) {
		lru_add_drain_all();
		pks_evacuate_sparse(gfp, &dirty);
		page = pks_take_one();
		if (page)
			return page;
	}

	if (!dirty || (gfp & __GFP_NORETRY) || current->journal_info ||
	    (gfp & (__GFP_FS | __GFP_IO)) != (__GFP_FS | __GFP_IO))
		return NULL;

	wakeup_flusher_threads(WB_REASON_VMSCAN);
	for (tries = 0; tries < PKS_WRITEBACK_RETRIES; tries++) {
		if (fatal_signal_pending(current))
			break;
		schedule_timeout_uninterruptible(msecs_to_jiffies(10));
		pks_evacuate_sparse(gfp, NULL);
		page = pks_take_one();
		if (page)
			return page;
		if ((tries & 0xf) == 0xf) {
			pks_drain_all_mags();
			wakeup_flusher_threads(WB_REASON_VMSCAN);
		}
	}
	if (tries == PKS_WRITEBACK_RETRIES)
		pks_stat_inc(PKS_STAT_INPOOL_TIMEOUT);
	return NULL;
}

/*
 * Slow path: grow, or failing that find a page inside the pool. Each attempt
 * returns the page it wins straight to this caller, so only an attempt that
 * found nothing counts against the budget.
 *
 * Runs clamped: ->write_begin() reaches here inside an open write scope, and
 * none of the reclaim, compaction, tagging or eviction below may run with
 * write access.
 */
static struct page *pks_alloc_slow(gfp_t gfp)
{
	struct pcache_pks_scope scope;
	struct page *page = NULL;
	int tries;

	pcache_pks_scope_clamp(&scope);
	for (tries = 0; tries < PKS_GROW_TRIES && !page; tries++) {
		/* A static reservation never grows. */
		if (!pks_static)
			page = pks_grow(gfp, false);
		if (!page && READ_ONCE(pks_pool.nr_blocks))
			page = pks_reclaim_inpool(gfp);
	}
	pcache_pks_scope_end(&scope);
	return page;
}

/*
 * Allocate one tagged page, ready for use as alloc_pages() would return it.
 * Its content is whatever its last user left.
 */
struct page *pks_pool_alloc(gfp_t gfp)
{
	struct pks_magazine *m;
	struct page *page = NULL;
	unsigned long flags;

	local_irq_save(flags);
	m = this_cpu_ptr(&pks_mags);
	if (m->nr_alloc || pks_refill(m))
		page = m->alloc[--m->nr_alloc];
	local_irq_restore(flags);

	/* Never grow from inside reclaim: pks_grow() may be what is reclaiming. */
	if (!page && gfpflags_allow_blocking(gfp) &&
	    !(current->flags & PF_MEMALLOC))
		page = pks_alloc_slow(gfp);
	if (page)
		post_alloc_hook(page, 0, gfp);
	return page;
}

/* Take back one page of the pool. Any context. */
void pks_pool_free(struct page *page)
{
	struct pks_magazine *m;
	struct pks_block *b;
	unsigned long flags;

	/*
	 * A block being evacuated takes its freed pages back directly, so its
	 * evictions reach its bitmap on whichever CPU drops the last reference.
	 */
	b = pks_block_of(page_to_pfn(page));
	if (unlikely(READ_ONCE(b->isolated))) {
		long returned;

		spin_lock_irqsave(&pks_pool.lock, flags);
		returned = pks_return_one(page);
		spin_unlock_irqrestore(&pks_pool.lock, flags);
		if (returned)
			mod_node_page_state(NODE_DATA(first_online_node),
					    NR_KERNEL_MISC_RECLAIMABLE, 1);
		return;
	}

	local_irq_save(flags);
	m = this_cpu_ptr(&pks_mags);
	if (m->nr_free == PKS_MAG_SIZE)
		pks_drain(m, false);
	m->free[m->nr_free++] = page;
	local_irq_restore(flags);
}

/*
 * Report what pks_shrink_scan() can return to the buddy allocator: empty
 * blocks above the reserve, plus stranded free pages once evacuation would
 * run. In-use pages are deliberately not counted: they are on the file LRU,
 * which already ages them, and counting them here would press protected page
 * cache harder than ordinary page cache under the same pressure.
 */
static unsigned long pks_shrink_count(struct shrinker *s,
				      struct shrink_control *sc)
{
	unsigned long blocks, empty, reserve, stranded, n = 0;

	/* A static reservation is never given back. */
	if (!pcache_pks_is_enabled() || pks_static)
		return 0;
	blocks = READ_ONCE(pks_pool.nr_blocks);
	if (!blocks)
		return SHRINK_EMPTY;

	empty = READ_ONCE(pks_pool.nr_empty);
	reserve = pks_reserve_blocks(blocks);
	if (empty > reserve)
		n += (empty - reserve) * PKS_BLOCK_PAGES;
	stranded = pks_stranded_pages();
	if ((sc->gfp_mask & __GFP_FS) &&
	    stranded > blocks * PKS_BLOCK_PAGES / 8)
		n += stranded;
	return n;
}

static unsigned long pks_shrink_scan(struct shrinker *s,
				     struct shrink_control *sc)
{
	struct pcache_pks_scope scope;
	unsigned long reserve, freed;
	unsigned int noreclaim;

	if (!pcache_pks_is_enabled() || pks_static ||
	    !READ_ONCE(pks_pool.nr_blocks))
		return SHRINK_STOP;

	/* Some reclaim callers, such as __node_reclaim(), do not clamp. */
	pcache_pks_scope_clamp(&scope);
	noreclaim = memalloc_noreclaim_save();

	reserve = pks_reserve_blocks(READ_ONCE(pks_pool.nr_blocks));
	freed = pks_release_empty(reserve, sc->nr_to_scan);

	if (freed < sc->nr_to_scan && gfpflags_allow_blocking(sc->gfp_mask)) {
		pks_drain_all_mags();
		freed += pks_release_empty(reserve, sc->nr_to_scan - freed);
	}

	if (freed < sc->nr_to_scan && (sc->gfp_mask & __GFP_FS) &&
	    pks_stranded_pages() >
	    READ_ONCE(pks_pool.nr_blocks) * PKS_BLOCK_PAGES / 8) {
		pks_evacuate_sparse(sc->gfp_mask, NULL);
		freed += pks_release_empty(reserve, sc->nr_to_scan - freed);
	}

	memalloc_noreclaim_restore(noreclaim);
	pcache_pks_scope_end(&scope);

	return freed ? freed : SHRINK_STOP;
}

static struct shrinker pks_shrinker = {
	.count_objects	= pks_shrink_count,
	.scan_objects	= pks_shrink_scan,
	.seeks		= DEFAULT_SEEKS,
	.batch		= PKS_BLOCK_PAGES,
};

static int pks_cpu_offline(unsigned int cpu)
{
	struct pks_magazine *m = per_cpu_ptr(&pks_mags, cpu);
	unsigned long flags;

	local_irq_save(flags);
	pks_drain(m, true);
	local_irq_restore(flags);
	return 0;
}

#ifdef CONFIG_PCACHE_PKS_DEBUG
static void pks_show_list(struct seq_file *m, struct list_head *head)
{
	struct pks_block *b;

	list_for_each_entry(b, head, list)
		seq_printf(m, "0x%lx 0x%lx %u\n", b->base_pfn,
			   b->base_pfn + PKS_BLOCK_PAGES, b->nr_free);
}

/* One "start_pfn end_pfn nr_free" line per block that is on a list. */
void pks_pool_show_blocks(struct seq_file *m)
{
	unsigned long flags;

	spin_lock_irqsave(&pks_pool.lock, flags);
	pks_show_list(m, &pks_pool.partial);
	pks_show_list(m, &pks_pool.full);
	pks_show_list(m, &pks_pool.empty);
	spin_unlock_irqrestore(&pks_pool.lock, flags);
}
#endif

void pks_pool_info(struct pks_pool_info *info)
{
	unsigned long flags, r;

	info->pending_pages = 0;
	spin_lock_irqsave(&pks_pool.lock, flags);
	info->tagged_pages = pks_pool.nr_blocks * PKS_BLOCK_PAGES;
	info->blocks = pks_pool.nr_blocks;
	info->empty_blocks = pks_pool.nr_empty;
	info->free_pages = pks_pool.nr_free_pages;
	spin_unlock_irqrestore(&pks_pool.lock, flags);

	/* Unlocked: these two are for reports only. */
	info->regions_split = 0;
	info->regions_occupied = 0;
	for (r = 0; r < pks_nr_regions; r++) {
		info->regions_split += READ_ONCE(pks_regions[r].split);
		info->regions_occupied += !!READ_ONCE(pks_regions[r].nr_blocks);
	}
}

#ifdef CONFIG_PCACHE_PKS_BASELINES
/*
 * Static placement, an evaluation baseline: the pool is filled to a fixed size
 * at boot and then neither grows nor shrinks. Everything else is the code of
 * the dynamic pool, so a comparison shows what growing on demand costs and
 * what it saves.
 */
bool pks_static __ro_after_init;
static unsigned long pks_reserve_bytes __initdata;

static int __init setup_pcache_pks_reserve(char *str)
{
	if (!str)
		return -EINVAL;
	pks_reserve_bytes = memparse(str, NULL);
	return 0;
}
early_param("pcache_pks.reserve", setup_pcache_pks_reserve);

int __init pks_pool_reserve(void)
{
	unsigned long want = pks_reserve_bytes >> PMD_SHIFT;
	unsigned long flags;
	struct page *page;

	if (!want) {
		pr_err("pcache_pks.placement=static needs pcache_pks.reserve=<size>\n");
		return -EINVAL;
	}
	while (READ_ONCE(pks_pool.nr_blocks) < want) {
		page = pks_grow(GFP_KERNEL | __GFP_NOWARN, true);
		if (!page)
			break;
		/* pks_grow() hands its caller one page; put it back. */
		spin_lock_irqsave(&pks_pool.lock, flags);
		pks_return_one(page);
		spin_unlock_irqrestore(&pks_pool.lock, flags);
		mod_node_page_state(NODE_DATA(first_online_node),
				    NR_KERNEL_MISC_RECLAIMABLE, 1);
		cond_resched();
	}
	pks_static = true;
	pr_info("static reservation: %lu of %lu MiB\n",
		READ_ONCE(pks_pool.nr_blocks) << (PMD_SHIFT - 20),
		want << (PMD_SHIFT - 20));
	return READ_ONCE(pks_pool.nr_blocks) ? 0 : -ENOMEM;
}
#endif /* CONFIG_PCACHE_PKS_BASELINES */

/*
 * Note the regions that are not mapped by one 1 GiB leaf at boot. Using them
 * splits nothing.
 */
static void __init pks_regions_init(void)
{
	unsigned long r;

	for (r = 0; r < pks_nr_regions; r++) {
		unsigned long pfn = r << PKS_REGION_SHIFT;
		unsigned int level;

		if (pfn_valid(pfn) &&
		    lookup_address((unsigned long)pfn_to_kaddr(pfn), &level) &&
		    level != PG_LEVEL_1G)
			pks_regions[r].split = true;
	}
}

int __init pks_pool_init(void)
{
	unsigned long nr;
	unsigned int cpu;
	int ret;

	nr = (max_pfn + PKS_BLOCK_PAGES - 1) >> PKS_BLOCK_ORDER;
	pcache_pks_block_map = kvcalloc(nr, sizeof(*pcache_pks_block_map),
					GFP_KERNEL);
	if (!pcache_pks_block_map)
		return -ENOMEM;
	pcache_pks_nr_blocks_max = nr;

	pks_nr_regions = (max_pfn + PKS_REGION_PAGES - 1) >> PKS_REGION_SHIFT;
	pks_regions = kvcalloc(pks_nr_regions, sizeof(*pks_regions),
			       GFP_KERNEL);
	if (!pks_regions) {
		ret = -ENOMEM;
		goto err_free;
	}
	pks_regions_init();

	for_each_possible_cpu(cpu)
		INIT_WORK(&per_cpu_ptr(&pks_mags, cpu)->drain_work,
			  pks_drain_work_fn);

	ret = cpuhp_setup_state_nocalls(CPUHP_BP_PREPARE_DYN,
					"mm/pcache_pks:drain", NULL,
					pks_cpu_offline);
	if (ret < 0) {
		pr_err("cpuhp setup failed: %d\n", ret);
		goto err_free;
	}
	ret = register_shrinker(&pks_shrinker);
	if (ret) {
		pr_err("shrinker registration failed\n");
		goto err_free;
	}
	return 0;

err_free:
	kvfree(pks_regions);
	pks_regions = NULL;
	pks_nr_regions = 0;
	kvfree(pcache_pks_block_map);
	pcache_pks_block_map = NULL;
	pcache_pks_nr_blocks_max = 0;
	return ret;
}
