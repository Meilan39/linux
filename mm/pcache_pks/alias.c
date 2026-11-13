// SPDX-License-Identifier: GPL-2.0
/*
 * Protected page cache: the write alias.
 *
 * Every protected page is mapped twice.
 *
 *   direct map   PKS_KEY_PAGE_CACHE     never write-enabled, in any context
 *   write alias  PKS_KEY_PAGE_CACHE_W   write-enabled inside a write scope
 *
 * A store into a protected page therefore succeeds only if it is made inside a
 * write scope and through the alias address. Only the helpers that write file
 * data on behalf of a write path compute that address, with
 * pcache_pks_waddr(). A store through page_address(), kmap(), sg_virt() or any
 * other direct-map pointer faults even while a scope is open.
 *
 * The alias is linear. One virtual range as large as physical memory is
 * reserved in the vmalloc area at boot, and the alias of a page is the base of
 * that range plus the page's physical address. Translation is one addition, and
 * a block is mapped there by one 2 MiB leaf when it is tagged and unmapped
 * when it is untagged.
 */
#define pr_fmt(fmt) "pcache_pks: " fmt

#include <linux/init.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/pgtable.h>
#include <linux/printk.h>
#include <linux/vmalloc.h>
#include <asm/pgtable_types.h>
#include <asm/set_memory.h>
#include "../internal.h"
#include "internal.h"

unsigned long pcache_pks_alias_delta __ro_after_init;
EXPORT_SYMBOL_GPL(pcache_pks_alias_delta);

/*
 * Map the alias of the direct-map range [@addr, @addr + @size). No flush is
 * needed: the alias range was not mapped before, and an earlier mapping was
 * flushed when it was removed.
 */
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

/*
 * Unmap the alias of the direct-map range [@addr, @addr + @size). The caller
 * flushes. @need_flush_all is set when the alias was not one 2 MiB leaf, so
 * that one invlpg on its base does not cover it.
 */
void pks_alias_unmap(unsigned long addr, unsigned long size,
		     bool *need_flush_all)
{
	unsigned long alias = pks_alias_addr(addr);
	unsigned int level;
	pte_t *pte;

	pte = lookup_address(alias, &level);
	if (!pte || pte_none(*pte))
		return;
	if (level != PG_LEVEL_2M)
		*need_flush_all = true;
	vunmap_range_noflush(alias, alias + size);
}

int __init pks_alias_init(void)
{
	struct vm_struct *area;
	unsigned long base;

	/*
	 * The alias of a 2 MiB block can be a 2 MiB leaf only if the base is
	 * aligned. Aligning it to 1 GiB costs nothing and keeps the alias
	 * congruent with the direct map at every leaf size.
	 */
	area = get_vm_area(PFN_PHYS(max_pfn) + PUD_SIZE, VM_IOREMAP);
	if (!area) {
		pr_err("no room for the write alias in the vmalloc area\n");
		return -ENOMEM;
	}
	base = ALIGN((unsigned long)area->addr, PUD_SIZE);
	pcache_pks_alias_delta = base - PAGE_OFFSET;
	return 0;
}
