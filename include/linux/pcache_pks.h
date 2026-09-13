/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PCACHE_PKS_H
#define _LINUX_PCACHE_PKS_H

#include <linux/pks.h>
#include <linux/pks-keys.h>
#include <linux/jump_label.h>
#include <linux/mm_types.h>

#ifdef CONFIG_PCACHE_PKS

DECLARE_STATIC_KEY_FALSE(pcache_pks_enabled);

void pcache_pks_pool_reserve(void);
bool pcache_pks_page(struct page *page);
struct folio *pcache_pks_alloc_folio(gfp_t gfp, unsigned int order);
void pcache_pks_recycle_page(struct page *page);

struct pcache_pks_scope {
	u8 old;
	bool active;
};

#ifdef CONFIG_PCACHE_PKS_DEBUG
extern atomic64_t pcache_pks_scope_begin_count;
extern atomic64_t pcache_pks_scope_end_count;

static inline void pcache_pks_scope_begin_inc(void)
{
	atomic64_inc(&pcache_pks_scope_begin_count);
}

static inline void pcache_pks_scope_end_inc(void)
{
	atomic64_inc(&pcache_pks_scope_end_count);
}
#else
static inline void pcache_pks_scope_begin_inc(void) {}
static inline void pcache_pks_scope_end_inc(void) {}
#endif

static inline void pcache_pks_scope_begin(struct pcache_pks_scope *scope,
					  bool active, u8 protection)
{
	scope->active = active;
	if (active) {
		pcache_pks_scope_begin_inc();
		scope->old = pks_update_protection(PKS_KEY_PAGE_CACHE,
						   protection);
	}
}

static inline void pcache_pks_scope_end(struct pcache_pks_scope *scope)
{
	if (scope->active) {
		pks_update_protection(PKS_KEY_PAGE_CACHE, scope->old);
		pcache_pks_scope_end_inc();
	}
	scope->active = false;
}

#else /* !CONFIG_PCACHE_PKS */

static inline void pcache_pks_pool_reserve(void) {}
static inline bool pcache_pks_page(struct page *page) { return false; }
static inline struct folio *pcache_pks_alloc_folio(gfp_t gfp, unsigned int order)
{
	return NULL;
}
static inline void pcache_pks_recycle_page(struct page *page) {}

struct pcache_pks_scope {
	u8 old;
	bool active;
};

static inline void pcache_pks_scope_begin(struct pcache_pks_scope *scope,
					  bool active, u8 protection) {}
static inline void pcache_pks_scope_end(struct pcache_pks_scope *scope) {}

#endif /* CONFIG_PCACHE_PKS */

#endif /* _LINUX_PCACHE_PKS_H */
