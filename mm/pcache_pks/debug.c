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
#include "internal.h"

/*
 * Key audit: every entry that maps [@addr, @addr + @size) must carry @pkey.
 * Run on the direct map and on the alias after promotion, before pages are
 * published, and on the direct map after demotion, before they return home.
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
 * The time is wall-clock between open and close, so it includes sleep time.
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
 * must never outlive the syscall or exception that opened it; if one does,
 * log the last opener details, clear the depth, and revoke write access.
 */
void pcache_pks_check_user_return(void)
{
	u8 depth;
	bool open;

	if (!pcache_pks_is_enabled())
		return;

	open = pcache_pks_wkey_open(current->thread.pkrs);
	depth = current->thread.pks_scope_depth;
	if (likely(!open && !depth))
		return;

	pks_stat_inc(PKS_STAT_SCOPE_LEAK);
	WARN_ONCE(1, "pcache_pks: write scope leaked to user mode (%s[%d], depth=%u, last_site=%s, last_ip=%pS, pkrs=%#x)\n",
		  current->comm, current->pid, depth,
		  current->thread.pks_last_site < PKS_NR_SITES ?
		  pks_site_names[current->thread.pks_last_site] : "none",
		  (void *)current->thread.pks_last_ip,
		  current->thread.pkrs);
	current->thread.pks_scope_depth = 0;
	if (open)
		pks_update_protection(PKS_KEY_PAGE_CACHE_W, PKEY_DISABLE_WRITE);
}

static int pks_status_show(struct seq_file *m, void *v)
{
	struct pks_zone_info info;
	unsigned long scopes = 0;
	unsigned int cpu;
	int i;

	pks_place_info(&info);
	scopes = pks_scope_total();

	seq_printf(m, "enabled:             %d\n", pcache_pks_is_enabled());
#ifdef CONFIG_PCACHE_PKS_BASELINES
	seq_printf(m, "placement:           %s\n", pks_place_name());
	seq_printf(m, "untag_pending:       %lu\n", info.pending_pages);
#endif
	seq_printf(m, "zone_blocks:         %lu\n",
		   info.tagged_pages >> pageblock_order);
	seq_printf(m, "zone_tagged_pages:   %lu\n", info.tagged_pages);
	seq_printf(m, "zone_free_pages:     %lu\n", info.free_pages);
	seq_printf(m, "scope_count:         %lu\n", scopes);
	seq_printf(m, "refused_grants:      %lu\n",
		   pks_stat_read(PKS_STAT_REFUSED));
	seq_printf(m, "tag_ops:             %lu\n",
		   pks_stat_read(PKS_STAT_TAG));
	seq_printf(m, "untag_ops:           %lu\n",
		   pks_stat_read(PKS_STAT_UNTAG));
	seq_printf(m, "tlb_flushes:         %lu\n",
		   pks_stat_read(PKS_STAT_FLUSH));
	seq_printf(m, "grow_failures:       %lu\n",
		   pks_stat_read(PKS_STAT_GROW_FAIL));
	seq_printf(m, "suppressed_faults:   %lu\n",
		   pks_stat_read(PKS_STAT_FAULT_COMPLETED));
	seq_printf(m, "stopped_faults:      %lu\n",
		   pks_stat_read(PKS_STAT_FAULT_STOPPED));
	for (i = 0; i < PKS_NR_INSN_CLASSES; i++)
		seq_printf(m, "fault_%-14s %lu\n", pks_insn_class_names[i],
			   pks_stat_read(PKS_STAT_FAULT_STORE + i));
	seq_printf(m, "scope_leaks:         %lu\n",
		   pks_stat_read(PKS_STAT_SCOPE_LEAK));

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
	int nid;

	for_each_online_node(nid) {
		struct zone *zone = &NODE_DATA(nid)->node_zones[ZONE_PKS];
		unsigned long pfn = ALIGN(zone->zone_start_pfn,
					  pageblock_nr_pages);
		unsigned long end = zone_end_pfn(zone);

		for (; pfn + pageblock_nr_pages <= end;
		     pfn += pageblock_nr_pages) {
			struct page *page = pfn_to_online_page(pfn);

			if (page && page_zone(page) == zone)
				seq_printf(m, "0x%lx 0x%lx\n",
					   pfn, pfn + pageblock_nr_pages);
		}
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(pks_blocks);

void __init pks_debug_init(void)
{
	struct dentry *dir = debugfs_create_dir("pcache_pks", NULL);

	debugfs_create_file("status", 0444, dir, NULL, &pks_status_fops);
	debugfs_create_file("blocks", 0444, dir, NULL, &pks_blocks_fops);
}
