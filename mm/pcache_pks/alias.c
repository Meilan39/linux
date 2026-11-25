// SPDX-License-Identifier: GPL-2.0
/*
 * Protected page cache: the write alias.
 *
 * Every protected page is mapped twice:
 *
 *   direct map   PKS_KEY_PAGE_CACHE     never write-enabled, in any context
 *   write alias  PKS_KEY_PAGE_CACHE_W   write-enabled only inside write scopes
 *
 * A store into a protected page succeeds only inside an open write scope and
 * through the alias virtual address computed by pcache_pks_waddr(). One
 * contiguous vmalloc reservation congruent at PUD granularity mirrors physical
 * memory so translation is a single constant offset addition.
 */
#define pr_fmt(fmt) "pcache_pks: " fmt

#include <linux/init.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/pgtable.h>
#include <linux/printk.h>
#include <linux/vmalloc.h>
#include <asm/pgtable_types.h>
#include "../internal.h"
#include "internal.h"

unsigned long pcache_pks_alias_delta __ro_after_init;
EXPORT_SYMBOL_GPL(pcache_pks_alias_delta);

static struct vm_struct *pks_alias_area __ro_after_init;
static unsigned long pks_alias_base __ro_after_init;
static unsigned long pks_alias_end __ro_after_init;

bool pks_is_alias_addr(unsigned long addr)
{
	return addr >= pks_alias_base && addr < pks_alias_end;
}

int pks_alias_map(unsigned long addr, unsigned long size)
{
	unsigned long alias = pks_alias_addr(addr);
	int err;

	err = ioremap_page_range(alias, alias + size, __pa(addr),
				 PAGE_KERNEL_PKEY(PKS_KEY_PAGE_CACHE_W));
	if (err)
		vunmap_range_noflush(alias, alias + size);
	return err;
}

void pks_alias_unmap(unsigned long addr, unsigned long size,
		     bool *need_flush_all)
{
	unsigned long alias = pks_alias_addr(addr);
	unsigned int level;
	pte_t *pte;

	pte = lookup_address(alias, &level);
	if (!pte || pte_none(*pte))
		return;
	if (need_flush_all && page_level_size(level) < size)
		*need_flush_all = true;
	vunmap_range_noflush(alias, alias + size);
}

int __init pks_alias_init(void)
{
	unsigned long span = PFN_PHYS(max_pfn);
	struct vm_struct *area;

	/*
	 * Align the base to PUD_SIZE so 2 MiB and 1 GiB page-table congruence
	 * with the direct map is preserved cleanly.
	 */
	area = get_vm_area(span + PUD_SIZE, VM_IOREMAP);
	if (!area) {
		pr_err("no room for the write alias in the vmalloc area\n");
		return -ENOMEM;
	}
	pks_alias_area = area;
	pks_alias_base = ALIGN((unsigned long)area->addr, PUD_SIZE);
	pks_alias_end = pks_alias_base + span;
	pcache_pks_alias_delta = pks_alias_base - PAGE_OFFSET;
	return 0;
}

void __init pks_alias_exit(void)
{
	if (!pks_alias_area)
		return;
	free_vm_area(pks_alias_area);
	pks_alias_area = NULL;
	pks_alias_base = 0;
	pks_alias_end = 0;
	pcache_pks_alias_delta = 0;
}
