// SPDX-License-Identifier: GPL-2.0
/*
 * Protected page cache: enablement, the entry points for the page cache and
 * the page allocator, and the counters.
 *
 * A mapping that asks for protection keeps its page cache in memory whose
 * kernel mapping carries a supervisor protection key (PKS). The key is
 * write-disabled, so a kernel store into such a page faults.
 *
 *   core.c	this file
 *   pool.c	placement: which memory is tagged, and when
 *   alias.c	the second mapping, through which granted stores are made
 *   scope.c	write scopes: who is granted access to the alias, and when
 *   fault.c	what happens to a store that was not granted
 */
#define pr_fmt(fmt) "pcache_pks: " fmt

#include <linux/cpumask.h>
#include <linux/gfp.h>
#include <linux/init.h>
#include <linux/kobject.h>
#include <linux/mm.h>
#include <linux/pks.h>
#include <linux/printk.h>
#include <linux/sysfs.h>
#include "../internal.h"
#include "internal.h"

DEFINE_STATIC_KEY_FALSE(pcache_pks_enabled);
EXPORT_SYMBOL_GPL(pcache_pks_enabled);

DEFINE_PER_CPU(struct pks_stats, pcache_pks_stats);

static bool pcache_pks_param __initdata;

static int __init setup_pcache_pks(char *str)
{
	return kstrtobool(str, &pcache_pks_param);
}
early_param("pcache_pks", setup_pcache_pks);

unsigned long pks_stat_read(enum pks_stat_item item)
{
	unsigned long sum = 0;
	unsigned int cpu;

	for_each_possible_cpu(cpu)
		sum += per_cpu(pcache_pks_stats.count[item], cpu);
	return sum;
}

/*
 * Allocate a page-cache folio for a protected mapping. The folio is tagged in
 * every kernel mapping before it is returned. It is not zeroed: like any
 * page-cache folio, it is filled by the read or write that asked for it.
 */
struct folio *pcache_pks_alloc_folio(gfp_t gfp, unsigned int order)
{
	struct page *page;

	if (WARN_ON_ONCE(order) || (gfp & __GFP_ZERO))
		return NULL;

	page = pks_pool_alloc(gfp);
	if (!page) {
		if (!(gfp & __GFP_NOWARN))
			pr_warn_ratelimited("pool exhausted\n");
		return NULL;
	}

	post_alloc_hook(page, 0, gfp);
	return page_folio(page);
}
EXPORT_SYMBOL_GPL(pcache_pks_alloc_folio);

/* The page allocator's free path hands every protected page back here. */
void pcache_pks_recycle_page(struct page *page)
{
	pks_pool_free(page);
}

#define PKS_STAT_ATTR(_name, _item)					\
static ssize_t _name##_show(struct kobject *kobj,			\
			    struct kobj_attribute *attr, char *buf)	\
{									\
	return sysfs_emit(buf, "%lu\n", pks_stat_read(_item));		\
}									\
static struct kobj_attribute _name##_attr = __ATTR_RO(_name)

PKS_STAT_ATTR(tag_ops, PKS_STAT_TAG);
PKS_STAT_ATTR(untag_ops, PKS_STAT_UNTAG);
PKS_STAT_ATTR(tlb_flushes, PKS_STAT_FLUSH);
PKS_STAT_ATTR(refused_grants, PKS_STAT_REFUSED);
PKS_STAT_ATTR(suppressed_faults, PKS_STAT_FAULT_COMPLETED);
PKS_STAT_ATTR(stopped_faults, PKS_STAT_FAULT_STOPPED);

static ssize_t scope_count_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	unsigned long sum = 0;
	int site;

	for (site = 0; site < PKS_NR_SITES; site++)
		sum += pks_scope_count(site);
	return sysfs_emit(buf, "%lu\n", sum);
}
static struct kobj_attribute scope_count_attr = __ATTR_RO(scope_count);

static ssize_t tagged_kb_show(struct kobject *kobj,
			      struct kobj_attribute *attr, char *buf)
{
	struct pks_pool_info info;

	pks_pool_info(&info);
	return sysfs_emit(buf, "%lu\n",
			  info.blocks << (PKS_BLOCK_ORDER + PAGE_SHIFT - 10));
}
static struct kobj_attribute tagged_kb_attr = __ATTR_RO(tagged_kb);

static ssize_t free_kb_show(struct kobject *kobj,
			    struct kobj_attribute *attr, char *buf)
{
	struct pks_pool_info info;

	pks_pool_info(&info);
	return sysfs_emit(buf, "%lu\n", info.free_pages << (PAGE_SHIFT - 10));
}
static struct kobj_attribute free_kb_attr = __ATTR_RO(free_kb);

static struct attribute *pcache_pks_attrs[] = {
	&tagged_kb_attr.attr,
	&free_kb_attr.attr,
	&tag_ops_attr.attr,
	&untag_ops_attr.attr,
	&tlb_flushes_attr.attr,
	&scope_count_attr.attr,
	&refused_grants_attr.attr,
	&suppressed_faults_attr.attr,
	&stopped_faults_attr.attr,
	NULL,
};

static const struct attribute_group pcache_pks_attr_group = {
	.attrs = pcache_pks_attrs,
};

/* /sys/kernel/mm/pcache_pks/ exists exactly when the protection is active. */
static void __init pcache_pks_sysfs_init(void)
{
	struct kobject *kobj = kobject_create_and_add("pcache_pks", mm_kobj);

	if (!kobj || sysfs_create_group(kobj, &pcache_pks_attr_group))
		pr_warn("sysfs counters unavailable\n");
}

static int __init pcache_pks_init(void)
{
	if (!pcache_pks_param || !pks_available())
		return 0;

	/*
	 * Page-init hardening writes freed and allocated pages through the
	 * direct map, which a tagged page rejects.
	 */
	if (want_init_on_alloc(0) || want_init_on_free() ||
	    page_poisoning_enabled_static() || debug_pagealloc_enabled()) {
		pr_err("incompatible page initialization active\n");
		return 0;
	}

	if (pks_alias_init() || pks_pool_init())
		return 0;

	static_branch_enable(&pcache_pks_enabled);
	pcache_pks_sysfs_init();
	pr_info("page-cache protection enabled (%lu-entry block map)\n",
		pcache_pks_nr_blocks_max);
	return 0;
}
subsys_initcall(pcache_pks_init);
