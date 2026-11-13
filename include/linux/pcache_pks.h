/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PCACHE_PKS_H
#define _LINUX_PCACHE_PKS_H

#include <linux/pagemap.h>
#include <linux/pcache_pks_page.h>
#include <linux/pks-keys.h>

#ifdef CONFIG_PCACHE_PKS

struct folio *pcache_pks_alloc_folio(gfp_t gfp, unsigned int order);
void pcache_pks_recycle_page(struct page *page);

/* Does @mapping keep its page cache in protected pages? */
static inline bool pcache_pks_mapping(const struct address_space *mapping)
{
	return pcache_pks_is_enabled() &&
	       mapping && mapping_pks_protected(mapping);
}

#else /* !CONFIG_PCACHE_PKS */

static inline struct folio *pcache_pks_alloc_folio(gfp_t gfp,
						   unsigned int order)
{
	return NULL;
}
static inline void pcache_pks_recycle_page(struct page *page) {}
static inline bool pcache_pks_mapping(const struct address_space *mapping)
{
	return false;
}

#endif /* CONFIG_PCACHE_PKS */

static inline bool pcache_pks_file(const struct file *file)
{
	return pcache_pks_mapping(file->f_mapping);
}

/*
 * Admission rule: a protected mapping holds only protected folios, and a
 * protected folio belongs only to a protected mapping.
 */
static inline bool pcache_pks_folio_fits(const struct address_space *mapping,
					 struct folio *folio)
{
	return pcache_pks_page(&folio->page) == pcache_pks_mapping(mapping);
}

#endif /* _LINUX_PCACHE_PKS_H */
