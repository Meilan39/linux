// SPDX-License-Identifier: GPL-2.0
/*
 * Protected page cache: enablement, zone capacity sysfs reporting, and shared
 * event counters.
 *
 * A mapping that asks for protection keeps its page cache in ZONE_PKS, whose
 * kernel direct mapping carries a write-disabled supervisor protection key.
 *
 *   core.c   this file
 *   zone.c   placement: block promotion, demotion, and the shrinker
 *   alias.c  write alias mapping used exclusively by granted write scopes
 *   scope.c  conditioned write scopes and grant authorization
 *   fault.c  instruction-class fault neutralization
 *   debug.c  key audits, debugfs status, and scope-leak tracing
 */
#define pr_fmt(fmt) "pcache_pks: " fmt

#include <linux/cpumask.h>
#include <linux/init.h>
#include <linux/kobject.h>
#include <linux/mm.h>
#include <linux/pks.h>
#include <linux/printk.h>
#include <linux/sysfs.h>
#include "internal.h"

DEFINE_STATIC_KEY_FALSE(pcache_pks_enabled);
EXPORT_SYMBOL_GPL(pcache_pks_enabled);

DEFINE_PER_CPU(struct pks_stats, pcache_pks_stats);

static bool pcache_pks_param __ro_after_init;

static int __init setup_pcache_pks(char *str)
{
	return str ? kstrtobool(str, &pcache_pks_param) : -EINVAL;
}
early_param("pcache_pks", setup_pcache_pks);

unsigned long pks_stat_read(enum pks_stat_item item)
{
	unsigned long sum = 0;
	unsigned int cpu;

	for_each_possible_cpu(cpu)
		sum += READ_ONCE(per_cpu(pcache_pks_stats.count[item], cpu));
	return sum;
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
	return sysfs_emit(buf, "%lu\n", pks_scope_total());
}
static struct kobj_attribute scope_count_attr = __ATTR_RO(scope_count);

static ssize_t tagged_kb_show(struct kobject *kobj,
			      struct kobj_attribute *attr, char *buf)
{
	struct pks_zone_info info;

	pks_zone_info(&info);
	return sysfs_emit(buf, "%lu\n", info.tagged_pages << (PAGE_SHIFT - 10));
}
static struct kobj_attribute tagged_kb_attr = __ATTR_RO(tagged_kb);

static ssize_t free_kb_show(struct kobject *kobj,
			    struct kobj_attribute *attr, char *buf)
{
	struct pks_zone_info info;

	pks_zone_info(&info);
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

static void __init pcache_pks_sysfs_init(void)
{
	struct kobject *kobj = kobject_create_and_add("pcache_pks", mm_kobj);

	if (!kobj) {
		pr_warn("sysfs counters unavailable\n");
		return;
	}
	if (sysfs_create_group(kobj, &pcache_pks_attr_group)) {
		kobject_put(kobj);
		pr_warn("sysfs counters unavailable\n");
	}
}

static int __init pcache_pks_init(void)
{
	if (!pcache_pks_param || !pks_available())
		return 0;

	/*
	 * Page-init hardening writes freed and allocated pages through the
	 * direct map, which protected pages reject.
	 */
	if (want_init_on_alloc(0) || want_init_on_free() ||
	    page_poisoning_enabled_static() || debug_pagealloc_enabled()) {
		pr_err("incompatible page initialization active\n");
		return 0;
	}

	if (pks_alias_init())
		return 0;
	if (pks_zone_init()) {
		pks_alias_exit();
		return 0;
	}

	static_branch_enable(&pcache_pks_enabled);
	pcache_pks_sysfs_init();
	pks_debug_init();
	pr_info("page-cache protection enabled\n");
	return 0;
}
subsys_initcall(pcache_pks_init);
