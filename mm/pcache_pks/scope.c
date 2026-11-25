// SPDX-License-Identifier: GPL-2.0
/*
 * Protected page cache: write scopes, site counters, and grant refusals.
 */
#define pr_fmt(fmt) "pcache_pks: " fmt

#include <linux/cpumask.h>
#include <linux/export.h>
#include <linux/highmem.h>
#include <linux/pagemap.h>
#include <linux/pcache_pks.h>
#include <linux/percpu.h>
#include <linux/printk.h>
#include <linux/sched.h>
#include "internal.h"

DEFINE_PER_CPU(unsigned long [PKS_NR_SITES], pcache_pks_scope_count);
EXPORT_PER_CPU_SYMBOL_GPL(pcache_pks_scope_count);

const char * const pks_site_names[PKS_NR_SITES] = {
	[PKS_SITE_BUFFERED_WRITE]	= "buffered_write",
	[PKS_SITE_FALLOCATE]		= "fallocate",
	[PKS_SITE_MKWRITE]		= "mkwrite",
	[PKS_SITE_LOOP_FALLOCATE]	= "loop_fallocate",
	[PKS_SITE_TRUNCATE]		= "truncate",
	[PKS_SITE_ORPHAN]		= "orphan",
	[PKS_SITE_WRITEBACK_ZERO]	= "writeback_zero",
	[PKS_SITE_READ_ZERO]		= "read_zero",
	[PKS_SITE_LOOP_FILL]		= "loop_fill",
	[PKS_SITE_MIGRATE]		= "migrate",
};

/* Scopes opened at @site since boot. */
unsigned long pks_scope_count(enum pcache_pks_site site)
{
	unsigned long sum = 0;
	unsigned int cpu;

	if ((unsigned int)site >= PKS_NR_SITES)
		return 0;

	for_each_possible_cpu(cpu)
		sum += READ_ONCE(per_cpu(pcache_pks_scope_count[site], cpu));
	return sum;
}

unsigned long pks_scope_total(void)
{
	unsigned long sum = 0;
	int site;

	for (site = 0; site < PKS_NR_SITES; site++)
		sum += pks_scope_count(site);
	return sum;
}

void pcache_pks_grant_refused(enum pcache_pks_site site)
{
	const char *name = ((unsigned int)site < PKS_NR_SITES) ?
			   pks_site_names[site] : "unknown";

	pks_stat_inc(PKS_STAT_REFUSED);
	pr_warn_ratelimited("refused write access at %s to '%s'[%d]\n",
			    name, current->comm, current->pid);
}
EXPORT_SYMBOL_GPL(pcache_pks_grant_refused);

/* Copy @src to @dst, as folio_copy() does, for folio_migrate_copy(). */
void pcache_pks_copy_folio(struct folio *dst, struct folio *src)
{
	DEFINE_PCACHE_PKS_SCOPE(scope);
	long i, nr = folio_nr_pages(dst);

	if (likely(!pcache_pks_page(&dst->page))) {
		folio_copy(dst, src);
		return;
	}

	/* folio_migrate_mapping() has returned -EBUSY for these */
	if (WARN_ON_ONCE(folio_nr_pages(src) != nr || !folio_test_locked(src) ||
			 !pcache_pks_page_fillable(&dst->page))) {
		pcache_pks_grant_refused(PKS_SITE_MIGRATE);
		return;
	}

	__pcache_pks_scope_open(&scope, PKS_SITE_MIGRATE);
	for (i = 0; i < nr; i++) {
		struct page *to = folio_page(dst, i);
		struct page *from = folio_page(src, i);
		void *vto = kmap_local_page(to);
		void *vfrom = kmap_local_page(from);

		copy_page(pcache_pks_waddr(to, vto), vfrom);
		kunmap_local(vfrom);
		kunmap_local(vto);
	}
	pcache_pks_scope_end(&scope);
}
