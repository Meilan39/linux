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
	PKS_STAT_REFUSED,	/* write grants refused */
	PKS_STAT_FAULT_STORE,	/* faults by instruction class, in the */
	PKS_STAT_FAULT_STRING,	/* order of enum pks_insn_class */
	PKS_STAT_FAULT_RMW,
	PKS_STAT_FAULT_OTHER,
	PKS_STAT_FAULT_COMPLETED,/* faults after which the context went on */
	PKS_STAT_FAULT_STOPPED,	/* faults the handler declined */
	PKS_STAT_SCOPE_LEAK,	/* scopes found open on return to user mode */
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

/* debug.c */
#ifdef CONFIG_PCACHE_PKS_DEBUG
struct seq_file;
void pks_debug_init(void);
void pks_audit_range(unsigned long addr, unsigned long size, int pkey);
void pks_pool_show_blocks(struct seq_file *m);
#else
static inline void pks_debug_init(void) {}
static inline void pks_audit_range(unsigned long addr, unsigned long size,
				   int pkey)
{
}
#endif

/* scope.c */
extern const char * const pks_site_names[PKS_NR_SITES];
unsigned long pks_scope_count(enum pcache_pks_site site);

/* fault.c */
enum pks_insn_class {
	PKS_INSN_STORE,		/* a move to memory */
	PKS_INSN_STRING,	/* movs, stos */
	PKS_INSN_RMW,		/* add, or, adc, sbb, and, sub, xor to memory */
	PKS_INSN_OTHER,		/* anything else, and anything locked */
	PKS_NR_INSN_CLASSES,
};

unsigned long pks_alu_flags(unsigned int op, unsigned int bits,
			    unsigned long dst, unsigned long src,
			    unsigned long flags);

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

#endif /* _MM_PCACHE_PKS_INTERNAL_H */
