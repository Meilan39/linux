// SPDX-License-Identifier: GPL-2.0
/*
 * Protected page cache: debugging (CONFIG_PCACHE_PKS_DEBUG).
 *
 * Nothing here is part of the protection. It checks invariants the other files
 * rely on, and reports state in more detail than the sysfs counters.
 */
#define pr_fmt(fmt) "pcache_pks: " fmt

#include <linux/debugfs.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/percpu.h>
#include <linux/pks.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <asm/pgtable_types.h>
#include <asm/pkeys_common.h>
#include <asm/set_memory.h>
#include "internal.h"

/*
 * Key audit: every entry that maps [@addr, @addr + @size) must carry @pkey.
 * Run on the direct map and on the alias after tagging, before the memory is
 * published, and on the direct map after untagging, before it is freed. A
 * mapping change that drops or misplaces a key would otherwise show only as a
 * fault that does not come.
 */
void pks_audit_range(unsigned long addr, unsigned long size, int pkey)
{
	unsigned long end = addr + size;
	unsigned int level;

	while (addr < end) {
		pte_t *pte = lookup_address(addr, &level);

		if (WARN_ONCE(!pte || pte_flags_pkey(pte_val(*pte)) != pkey,
			      "pcache_pks: mapping at %lx has key %d, expected %d\n",
			      addr, pte ? (int)pte_flags_pkey(pte_val(*pte)) : -1,
			      pkey))
			return;
		addr += page_level_size(level);
	}
}

/*
 * Window accounting: for each grant site, how long write access was open.
 * The time is taken between open and close, so it includes any time the
 * thread slept in between.
 */
struct pks_window {
	u64 cycles;
	u64 max;
};

static DEFINE_PER_CPU(struct pks_window [PKS_NR_SITES], pks_windows);

void pcache_pks_window_account(enum pcache_pks_site site, u64 cycles)
{
	struct pks_window *w = &get_cpu_var(pks_windows)[site];

	w->cycles += cycles;
	if (cycles > w->max)
		w->max = cycles;
	put_cpu_var(pks_windows);
}
EXPORT_SYMBOL_GPL(pcache_pks_window_account);

/*
 * Called with interrupts disabled on every return to user mode. A write scope
 * must never outlive the system call or exception that opened it. One that
 * did is reported and closed, so that it cannot be used from the next entry.
 */
void pcache_pks_check_user_return(void)
{
	u32 prot;

	if (!pcache_pks_is_enabled())
		return;
	prot = (current->thread.pkrs >> PKR_PKEY_SHIFT(PKS_KEY_PAGE_CACHE_W)) &
	       PKEY_ACCESS_MASK;
	if (likely(prot & (PKEY_DISABLE_WRITE | PKEY_DISABLE_ACCESS)))
		return;
	pks_stat_inc(PKS_STAT_SCOPE_LEAK);
	WARN_ONCE(1, "pcache_pks: write scope leaked to user mode (%s[%d])\n",
		  current->comm, current->pid);
	pks_update_protection(PKS_KEY_PAGE_CACHE_W, PKEY_DISABLE_WRITE);
}

static int pks_status_show(struct seq_file *m, void *v)
{
	static const char * const class_names[PKS_NR_INSN_CLASSES] = {
		"store", "string", "rmw", "other",
	};
	struct pks_pool_info info;
	unsigned long scopes = 0;
	unsigned int cpu;
	int i;

	pks_pool_info(&info);
	for (i = 0; i < PKS_NR_SITES; i++)
		scopes += pks_scope_count(i);

	seq_printf(m, "enabled:             %d\n", pcache_pks_is_enabled());
	seq_printf(m, "block_map_entries:   %lu\n", pcache_pks_nr_blocks_max);
	seq_printf(m, "pool_blocks:         %lu\n", info.blocks);
	seq_printf(m, "pool_empty_blocks:   %lu\n", info.empty_blocks);
	seq_printf(m, "pool_pages:          %lu\n",
		   info.blocks * PKS_BLOCK_PAGES);
	seq_printf(m, "pool_free_pages:     %lu\n", info.free_pages);
	seq_printf(m, "scope_count:         %lu\n", scopes);
	seq_printf(m, "refused_grants:      %lu\n",
		   pks_stat_read(PKS_STAT_REFUSED));
	seq_printf(m, "reclaim_pages:       %lu\n",
		   pks_stat_read(PKS_STAT_EVICTED));
	seq_printf(m, "grow_blocks:         %lu\n",
		   pks_stat_read(PKS_STAT_TAG));
	seq_printf(m, "release_blocks:      %lu\n",
		   pks_stat_read(PKS_STAT_UNTAG));
	seq_printf(m, "grow_affine_blocks:  %lu\n",
		   pks_stat_read(PKS_STAT_GROW_AFFINE));
	seq_printf(m, "regions_split:       %lu\n", info.regions_split);
	seq_printf(m, "regions_occupied:    %lu\n", info.regions_occupied);
	seq_printf(m, "tlb_flushes:         %lu\n",
		   pks_stat_read(PKS_STAT_FLUSH));
	seq_printf(m, "grow_failures:       %lu\n",
		   pks_stat_read(PKS_STAT_GROW_FAIL));
	seq_printf(m, "grow_fail_fallbacks: %lu\n",
		   pks_stat_read(PKS_STAT_INPOOL));
	seq_printf(m, "fallback_timeouts:   %lu\n",
		   pks_stat_read(PKS_STAT_INPOOL_TIMEOUT));
	seq_printf(m, "suppressed_faults:   %lu\n",
		   pks_stat_read(PKS_STAT_FAULT_COMPLETED));
	seq_printf(m, "stopped_faults:      %lu\n",
		   pks_stat_read(PKS_STAT_FAULT_STOPPED));
	for (i = 0; i < PKS_NR_INSN_CLASSES; i++)
		seq_printf(m, "fault_%-14s %lu\n", class_names[i],
			   pks_stat_read(PKS_STAT_FAULT_STORE + i));
	seq_printf(m, "scope_leaks:         %lu\n",
		   pks_stat_read(PKS_STAT_SCOPE_LEAK));

	/* One line per grant site: scopes opened, cycles open, longest one. */
	for (i = 0; i < PKS_NR_SITES; i++) {
		u64 cycles = 0, max = 0;

		for_each_possible_cpu(cpu) {
			struct pks_window *w = &per_cpu(pks_windows, cpu)[i];

			cycles += READ_ONCE(w->cycles);
			max = max(max, READ_ONCE(w->max));
		}
		seq_printf(m, "site_%-15s %lu %llu %llu\n", pks_site_names[i],
			   pks_scope_count(i), cycles, max);
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(pks_status);

static int pks_blocks_show(struct seq_file *m, void *v)
{
	pks_pool_show_blocks(m);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(pks_blocks);

void __init pks_debug_init(void)
{
	struct dentry *dir = debugfs_create_dir("pcache_pks", NULL);

	debugfs_create_file("status", 0444, dir, NULL, &pks_status_fops);
	debugfs_create_file("blocks", 0444, dir, NULL, &pks_blocks_fops);
}
