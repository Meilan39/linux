/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PCACHE_PKS_H
#define _LINUX_PCACHE_PKS_H

#include <linux/pcache_pks_page.h>
#include <linux/pks-keys.h>

#ifdef CONFIG_PCACHE_PKS

struct folio *pcache_pks_alloc_folio(gfp_t gfp, unsigned int order);
void pcache_pks_recycle_page(struct page *page);

#else /* !CONFIG_PCACHE_PKS */

static inline struct folio *pcache_pks_alloc_folio(gfp_t gfp,
						   unsigned int order)
{
	return NULL;
}
static inline void pcache_pks_recycle_page(struct page *page) {}

#endif /* CONFIG_PCACHE_PKS */

#endif /* _LINUX_PCACHE_PKS_H */
