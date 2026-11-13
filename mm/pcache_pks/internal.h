/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _MM_PCACHE_PKS_INTERNAL_H
#define _MM_PCACHE_PKS_INTERNAL_H

#include <linux/pcache_pks.h>
#include <linux/percpu.h>

/*
 * Event counters. They exist on every kernel that has the feature, so they are
 * per-CPU and cost one increment.
 */
enum pks_stat_item {
	PKS_STAT_TAG,		/* blocks tagged */
	PKS_STAT_UNTAG,		/* blocks untagged */
	PKS_STAT_FLUSH,		/* TLB flush rounds sent for tagging */
	PKS_STAT_GROW_FAIL,	/* growth attempts that got no block */
	PKS_STAT_INPOOL,	/* allocations served by in-pool reclaim */
	PKS_STAT_INPOOL_TIMEOUT,/* in-pool reclaims that waited in vain */
	PKS_STAT_EVICTED,	/* pages the pool evicted itself */
	PKS_NR_STATS,
};

struct pks_stats {
	unsigned long count[PKS_NR_STATS];
};

DECLARE_PER_CPU(struct pks_stats, pcache_pks_stats);

static inline void pks_stat_add(enum pks_stat_item item, unsigned long n)
{
	this_cpu_add(pcache_pks_stats.count[item], n);
}

static inline void pks_stat_inc(enum pks_stat_item item)
{
	this_cpu_inc(pcache_pks_stats.count[item]);
}

unsigned long pks_stat_read(enum pks_stat_item item);

/*
 * Placement: where protected pages come from and when their memory is tagged.
 * pool.c is the implementation.
 */
struct pks_pool_info {
	unsigned long blocks;
	unsigned long empty_blocks;
	unsigned long free_pages;
};

int pks_pool_init(void);
struct page *pks_pool_alloc(gfp_t gfp);
void pks_pool_free(struct page *page);
void pks_pool_info(struct pks_pool_info *info);

/* alias.c: the second, writable mapping of protected memory */
int pks_alias_init(void);
int pks_alias_map(unsigned long addr, unsigned long size);
void pks_alias_unmap(unsigned long addr, unsigned long size,
		     bool *need_flush_all);

/* The alias address of a direct-map address. */
static inline unsigned long pks_alias_addr(unsigned long addr)
{
	return addr + pcache_pks_alias_delta;
}

static inline void pks_audit_range(unsigned long addr, unsigned long size,
				   int pkey)
{
}

#endif /* _MM_PCACHE_PKS_INTERNAL_H */
