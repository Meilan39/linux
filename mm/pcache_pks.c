// SPDX-License-Identifier: GPL-2.0
#include <linux/mm.h>
#include <linux/memblock.h>
#include <linux/pagemap.h>
#include <linux/fs.h>
#include <linux/pcache_pks.h>
#include <linux/spinlock.h>
#include <linux/printk.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/atomic.h>
#include <asm/set_memory.h>
#include "internal.h"

DEFINE_STATIC_KEY_FALSE(pcache_pks_enabled);
EXPORT_SYMBOL_GPL(pcache_pks_enabled);

static phys_addr_t pool_base __ro_after_init;
static phys_addr_t pool_end __ro_after_init;
static unsigned long pool_start_pfn __ro_after_init;
static unsigned long pool_end_pfn __ro_after_init;

#ifdef CONFIG_PCACHE_PKS_DEBUG
atomic64_t pcache_pks_scope_count = ATOMIC64_INIT(0);
EXPORT_SYMBOL_GPL(pcache_pks_scope_count);

static int pcache_pks_status_show(struct seq_file *m, void *v)
{
	seq_printf(m, "pool_start_pfn: 0x%lx\n", pool_start_pfn);
	seq_printf(m, "pool_end_pfn:   0x%lx\n", pool_end_pfn);
	seq_printf(m, "scope_count:    %lld\n",
		   (long long)atomic64_read(&pcache_pks_scope_count));
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(pcache_pks_status);

static int __init pcache_pks_debugfs_init(void)
{
	struct dentry *dir;

	dir = debugfs_create_dir("pcache_pks", NULL);
	if (!dir)
		return -ENOMEM;

	debugfs_create_file("status", 0444, dir, NULL, &pcache_pks_status_fops);
	return 0;
}
late_initcall(pcache_pks_debugfs_init);
#endif /* CONFIG_PCACHE_PKS_DEBUG */

static DEFINE_SPINLOCK(pool_lock);
static LIST_HEAD(pool_free);

static bool pcache_pks_param __initdata;

static int __init setup_pcache_pks(char *str)
{
	if (!str)
		return -EINVAL;
	if (strcmp(str, "on") == 0)
		pcache_pks_param = true;
	else if (strcmp(str, "off") == 0)
		pcache_pks_param = false;
	return 0;
}
early_param("pcache_pks", setup_pcache_pks);

void __init pcache_pks_pool_reserve(void)
{
	if (!pcache_pks_param || !pks_available())
		return;

	pool_base = memblock_phys_alloc_range(SZ_256M, PMD_SIZE,
					      SZ_1M, PFN_PHYS(max_pfn));
	if (!pool_base) {
		pr_err("pcache_pks: cannot reserve pool\n");
		return;
	}

	pool_end = pool_base + SZ_256M;
	pool_start_pfn = PHYS_PFN(pool_base);
	pool_end_pfn = PHYS_PFN(pool_end);
}

static int __init pcache_pks_init(void)
{
	unsigned long pfn;
	int ret;

	if (!pool_base)
		return 0;

	if (want_init_on_alloc(0) || want_init_on_free() ||
	    page_poisoning_enabled_static() || debug_pagealloc_enabled()) {
		pr_err("pcache_pks: incompatible page initialization active\n");
		return 0;
	}

	if (!IS_ALIGNED(pool_base, PMD_SIZE) ||
	    !IS_ALIGNED(pool_end, PMD_SIZE) ||
	    !IS_ALIGNED((unsigned long)__va(pool_base), PMD_SIZE) ||
	    !IS_ALIGNED((unsigned long)__va(pool_end), PMD_SIZE)) {
		pr_err("pcache_pks: pool is not PMD aligned\n");
		return 0;
	}

	memset(__va(pool_base), 0, SZ_256M);

	for (pfn = pool_start_pfn; pfn < pool_end_pfn; pfn++) {
		struct page *page = pfn_to_page(pfn);

		if (!PageReserved(page) || page_count(page) != 1 ||
		    PageCompound(page) || PageLRU(page)) {
			pr_err("pcache_pks: invalid page state in pool\n");
			INIT_LIST_HEAD(&pool_free);
			return 0;
		}
		set_page_count(page, 0);
		list_add_tail(&page->lru, &pool_free);
	}

	ret = set_memory_pkey((unsigned long)__va(pool_base),
			      SZ_256M >> PAGE_SHIFT, PKS_KEY_PAGE_CACHE);
	if (ret) {
		pr_err("pcache_pks: set_memory_pkey failed: %d\n", ret);
		INIT_LIST_HEAD(&pool_free);
		return 0;
	}

	static_branch_enable(&pcache_pks_enabled);
	pr_info("pcache_pks: initialized 256MB pool at %pa\n", &pool_base);
	return 0;
}
early_initcall(pcache_pks_init);

bool pcache_pks_page(struct page *page)
{
	unsigned long pfn;

	if (!page || !static_branch_unlikely(&pcache_pks_enabled))
		return false;
	pfn = page_to_pfn(page);
	return pfn >= pool_start_pfn && pfn < pool_end_pfn;
}
EXPORT_SYMBOL_GPL(pcache_pks_page);

bool pcache_pks_mapping(const struct address_space *mapping)
{
	return static_branch_unlikely(&pcache_pks_enabled) &&
	       mapping_pks_protected(mapping);
}
EXPORT_SYMBOL_GPL(pcache_pks_mapping);

bool pcache_pks_file(const struct file *file)
{
	return file && S_ISREG(file_inode(file)->i_mode) &&
	       pcache_pks_mapping(file->f_mapping);
}
EXPORT_SYMBOL_GPL(pcache_pks_file);

int pcache_pks_reject_file(const struct file *file)
{
	if (unlikely(pcache_pks_file(file)))
		return -EOPNOTSUPP;
	return 0;
}
EXPORT_SYMBOL_GPL(pcache_pks_reject_file);

int pcache_pks_validate_folio(const struct address_space *mapping,
			      struct folio *folio)
{
	bool protected = mapping && mapping_pks_protected(mapping);
	bool pool = folio && pcache_pks_page(&folio->page);

	if (protected)
		return pool && !folio_order(folio) ? 0 : -EPERM;
	return pool ? -EPERM : 0;
}
EXPORT_SYMBOL_GPL(pcache_pks_validate_folio);

struct folio *pcache_pks_alloc_folio(gfp_t gfp, unsigned int order)
{
	struct page *page;
	unsigned long flags;

	if (WARN_ON_ONCE(order != 0) || (gfp & __GFP_ZERO))
		return NULL;

	spin_lock_irqsave(&pool_lock, flags);
	if (list_empty(&pool_free)) {
		spin_unlock_irqrestore(&pool_lock, flags);
		pr_warn_ratelimited("pcache_pks: pool exhausted\n");
		return NULL;
	}
	page = list_first_entry(&pool_free, struct page, lru);
	if (WARN_ON_ONCE(!PageReserved(page) || page_count(page) ||
			 PageLRU(page) || PageCompound(page))) {
		spin_unlock_irqrestore(&pool_lock, flags);
		return NULL;
	}
	list_del(&page->lru);
	spin_unlock_irqrestore(&pool_lock, flags);

	__ClearPageReserved(page);
	post_alloc_hook(page, 0, gfp);
	return page_folio(page);
}
EXPORT_SYMBOL_GPL(pcache_pks_alloc_folio);

void pcache_pks_recycle_page(struct page *page)
{
	unsigned long flags;

	if (WARN_ON_ONCE(!pcache_pks_page(page)))
		return;

	__SetPageReserved(page);
	spin_lock_irqsave(&pool_lock, flags);
	list_add(&page->lru, &pool_free);
	spin_unlock_irqrestore(&pool_lock, flags);
}
EXPORT_SYMBOL_GPL(pcache_pks_recycle_page);
