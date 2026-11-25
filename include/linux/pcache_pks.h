/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_PCACHE_PKS_H
#define _LINUX_PCACHE_PKS_H

#include <linux/pcache_pks_page.h>

#ifdef CONFIG_PCACHE_PKS

#include <linux/pagemap.h>

static inline bool pcache_pks_mapping(const struct address_space *mapping)
{
	return pcache_pks_is_enabled() &&
	       mapping && mapping_pks_protected(mapping);
}

static inline bool pcache_pks_file(const struct file *file)
{
	return pcache_pks_mapping(file->f_mapping);
}

static inline bool pcache_pks_folio_fits(const struct address_space *mapping,
					 struct folio *folio)
{
	return pcache_pks_page(&folio->page) == pcache_pks_mapping(mapping);
}

#else /* !CONFIG_PCACHE_PKS */

struct address_space;
struct file;
struct folio;

static inline bool pcache_pks_mapping(const struct address_space *mapping)
{
	return false;
}

static inline bool pcache_pks_file(const struct file *file)
{
	return false;
}

static inline bool pcache_pks_folio_fits(const struct address_space *mapping,
					 struct folio *folio)
{
	return true;
}

#endif /* CONFIG_PCACHE_PKS */

struct pcache_pks_scope {
	u8 old;
	bool active;
#ifdef CONFIG_PCACHE_PKS_DEBUG
	u8 site;
	u64 start;
#endif
};

#ifdef CONFIG_PCACHE_PKS
#define DEFINE_PCACHE_PKS_SCOPE(name) \
	struct pcache_pks_scope name = { .active = false }
#else
#define DEFINE_PCACHE_PKS_SCOPE(name) \
	struct pcache_pks_scope name __maybe_unused = { .active = false }
#endif

static inline void pcache_pks_scope_clamp(struct pcache_pks_scope *scope)
{
	scope->active = false;
}

static inline void pcache_pks_scope_end(struct pcache_pks_scope *scope)
{
	scope->active = false;
}

#endif /* _LINUX_PCACHE_PKS_H */
