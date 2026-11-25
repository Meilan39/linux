/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _MM_PCACHE_PKS_INTERNAL_H
#define _MM_PCACHE_PKS_INTERNAL_H

#include <linux/pcache_pks.h>
#include <linux/percpu.h>

/*
 * Event counters exported through /sys/kernel/mm/pcache_pks/ on all active
 * builds, and additionally through debugfs when CONFIG_PCACHE_PKS_DEBUG=y.
 */
enum pks_stat_item {
	PKS_STAT_TAG,		/* pageblocks promoted into ZONE_PKS */
	PKS_STAT_UNTAG,		/* pageblocks demoted back to home zones */
	PKS_STAT_FLUSH,		/* cross-CPU TLB flush rounds */
	PKS_STAT_GROW_FAIL,	/* synchronous promotions returning 0 blocks */
	PKS_STAT_REFUSED,	/* write grants refused */
	PKS_STAT_FAULT_STORE,	/* faults by instruction class */
	PKS_STAT_FAULT_STRING,
	PKS_STAT_FAULT_RMW,
	PKS_STAT_FAULT_OTHER,
	PKS_STAT_FAULT_COMPLETED,
	PKS_STAT_FAULT_STOPPED,
	PKS_STAT_SCOPE_LEAK,	/* open scopes repaired on return to user mode */
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

/* zone.c */
struct pks_zone_info {
	unsigned long tagged_pages;
	unsigned long free_pages;
};

int pks_zone_init(void);
void pks_zone_info(struct pks_zone_info *info);

/* debug.c */
#ifdef CONFIG_PCACHE_PKS_DEBUG
void pks_debug_init(void);
void pks_audit_range(unsigned long addr, unsigned long size, int pkey);
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
unsigned long pks_scope_total(void);

/* alias.c */
int pks_alias_init(void);
void pks_alias_exit(void);
int pks_alias_map(unsigned long addr, unsigned long size);
void pks_alias_unmap(unsigned long addr, unsigned long size,
		     bool *need_flush_all);
bool pks_is_alias_addr(unsigned long addr);

static inline unsigned long pks_alias_addr(unsigned long addr)
{
	return addr + pcache_pks_alias_delta;
}

#endif /* _MM_PCACHE_PKS_INTERNAL_H */
