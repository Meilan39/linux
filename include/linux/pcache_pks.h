/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PCACHE_PKS_H
#define _LINUX_PCACHE_PKS_H

#include <linux/jump_label.h>
#include <linux/mm.h>
#include <linux/pks.h>
#include <linux/pks-keys.h>

#ifdef CONFIG_PCACHE_PKS

DECLARE_STATIC_KEY_FALSE(pcache_pks_enabled);
extern unsigned long pcache_pks_start_pfn;
extern unsigned long pcache_pks_end_pfn;

void pcache_pks_pool_reserve(void);
struct folio *pcache_pks_alloc_folio(gfp_t gfp, unsigned int order);
void pcache_pks_recycle_page(struct page *page);

/* Is @page part of the PKS page-cache pool? */
static inline bool pcache_pks_page(struct page *page)
{
	unsigned long pfn;

	if (!static_branch_unlikely(&pcache_pks_enabled))
		return false;
	pfn = page_to_pfn(page);
	return pfn >= pcache_pks_start_pfn && pfn < pcache_pks_end_pfn;
}

#else /* !CONFIG_PCACHE_PKS */

static inline void pcache_pks_pool_reserve(void) {}
static inline struct folio *pcache_pks_alloc_folio(gfp_t gfp,
						   unsigned int order)
{
	return NULL;
}
static inline void pcache_pks_recycle_page(struct page *page) {}
static inline bool pcache_pks_page(struct page *page) { return false; }

#endif /* CONFIG_PCACHE_PKS */

#endif /* _LINUX_PCACHE_PKS_H */
