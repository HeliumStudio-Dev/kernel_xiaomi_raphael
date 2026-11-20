/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LINUX_MM_INLINE_H
#define LINUX_MM_INLINE_H

#include <linux/fs.h>
#include <linux/huge_mm.h>
#include <linux/swap.h>

/**
 * page_is_file_cache - should the page be on a file LRU or anon LRU?
 * @page: the page to test
 *
 * Returns 1 if @page is page cache page backed by a regular filesystem,
 * or 0 if @page is anonymous, tmpfs or otherwise ram or swap backed.
 * Used by functions that manipulate the LRU lists, to sort a page
 * onto the right LRU list.
 *
 * We would like to get this info without a page flag, but the state
 * needs to survive until the page is last deleted from the LRU, which
 * could be as far down as __page_cache_release.
 */
static inline int page_is_file_cache(struct page *page)
{
	return !PageSwapBacked(page);
}

static __always_inline void __update_lru_size(struct lruvec *lruvec,
				enum lru_list lru, enum zone_type zid,
				int nr_pages)
{
	struct pglist_data *pgdat = lruvec_pgdat(lruvec);

	mod_node_page_state(pgdat, NR_LRU_BASE + lru, nr_pages);
	mod_zone_page_state(&pgdat->node_zones[zid],
				NR_ZONE_LRU_BASE + lru, nr_pages);
}

static __always_inline void update_lru_size(struct lruvec *lruvec,
				enum lru_list lru, enum zone_type zid,
				int nr_pages)
{
	__update_lru_size(lruvec, lru, zid, nr_pages);
#ifdef CONFIG_MEMCG
	mem_cgroup_update_lru_size(lruvec, lru, zid, nr_pages);
#endif
}

/**
 * __clear_page_lru_flags - clear page lru flags before releasing a page
 * @page: the page that was on lru and now has a zero reference
 */
static __always_inline void __clear_page_lru_flags(struct page *page)
{
	VM_BUG_ON_PAGE(!PageLRU(page), page);

	__ClearPageLRU(page);

	/* this shouldn't happen, so leave the flags to bad_page() */
	if (PageActive(page) && PageUnevictable(page))
		return;

	__ClearPageActive(page);
	__ClearPageUnevictable(page);
}

/**
 * page_lru - which LRU list should a page be on?
 * @page: the page to test
 *
 * Returns the LRU list a page should be on, as an index
 * into the array of LRU lists.
 */
static __always_inline enum lru_list page_lru(struct page *page)
{
	enum lru_list lru;

	VM_BUG_ON_PAGE(PageActive(page) && PageUnevictable(page), page);

	if (PageUnevictable(page))
		return LRU_UNEVICTABLE;

	lru = page_is_file_cache(page) ? LRU_INACTIVE_FILE : LRU_INACTIVE_ANON;
	if (PageActive(page))
		lru += LRU_ACTIVE;

	return lru;
}

static inline void lru_set_gen_flags(unsigned long *flags, int gen)
{
	VM_WARN_ON_ONCE(gen < -1 || gen >= (int)MAX_NR_GENS);
	*flags &= ~LRU_GEN_MASK;
	*flags |= (gen + 1UL) << LRU_GEN_PGOFF;
}

static inline int lru_get_gen_flags(unsigned long flags)
{
	return ((flags & LRU_GEN_MASK) >> LRU_GEN_PGOFF) - 1;
}

static inline void lru_set_refs_flags(unsigned long *flags, unsigned int refs)
{
	VM_WARN_ON_ONCE(refs > LRU_REFS_MAX);
	BUILD_BUG_ON(LRU_REFS_MASK & (BIT(PG_referenced) | BIT(PG_workingset)));
	*flags &= ~LRU_REFS_FLAGS;
	if (refs & BIT(0))
		*flags |= BIT(PG_referenced);
	if (refs & BIT(1))
		*flags |= BIT(PG_workingset);
	if (LRU_REFS_WIDTH)
		*flags |= ((unsigned long)refs >> 2) << LRU_REFS_PGOFF;
}

static inline int lru_get_refs_flags(unsigned long flags)
{
	int refs = (flags & BIT(PG_referenced)) ? BIT(0) : 0;

	refs += (flags & BIT(PG_workingset)) ? BIT(1) : 0;
	refs += ((flags & LRU_REFS_MASK) >> LRU_REFS_PGOFF) << 2;
	return refs;
}

static inline int page_lru_refs(struct page *page)
{
	return lru_get_refs_flags(READ_ONCE(page->flags));
}

static inline void __page_set_lru_refs(struct page *page, unsigned int refs)
{
	unsigned long new_flags, old_flags;

	do {
		old_flags = READ_ONCE(page->flags);
		VM_WARN_ON_ONCE(lru_get_gen_flags(old_flags) != -1);
		new_flags = old_flags;
		lru_set_refs_flags(&new_flags, refs);
	} while (cmpxchg(&page->flags, old_flags, new_flags) != old_flags);
}

#ifdef CONFIG_LRU_GEN
void page_inc_lru_refs(struct page *page, unsigned int flags);
bool page_reset_lru_refs(struct page *page);
#else
static inline void page_inc_lru_refs(struct page *page, unsigned int flags)
{
	WARN_ON_ONCE(1);
}
static inline bool page_reset_lru_refs(struct page *page)
{
	return false;
}
#endif

static inline void page_migrate_refs(struct page *newpage, struct page *page)
{
	VM_WARN_ON_ONCE_PAGE(PageLRU(page), page);
	VM_WARN_ON_ONCE_PAGE(PageLRU(newpage), newpage);
	__page_set_lru_refs(newpage, page_lru_refs(page));
}

#ifdef CONFIG_LRU_GEN

static inline bool lru_gen_switching(void)
{
	DECLARE_STATIC_KEY_FALSE(lru_switch);

	return static_branch_unlikely(&lru_switch);
}

#ifdef CONFIG_LRU_GEN_ENABLED
static inline bool lru_gen_enabled(void)
{
	DECLARE_STATIC_KEY_TRUE(lru_gen_caps[NR_LRU_GEN_CAPS]);

	return static_branch_likely(&lru_gen_caps[LRU_GEN_CORE]);
}
#else
static inline bool lru_gen_enabled(void)
{
	DECLARE_STATIC_KEY_FALSE(lru_gen_caps[NR_LRU_GEN_CAPS]);

	return static_branch_unlikely(&lru_gen_caps[LRU_GEN_CORE]);
}
#endif

static inline bool lru_gen_in_fault(void)
{
	return current->in_lru_fault;
}

static inline int lru_gen_from_seq(unsigned long seq)
{
	return seq % MAX_NR_GENS;
}

static inline int lru_hist_from_seq(unsigned long seq)
{
	return seq % NR_HIST_GENS;
}

static inline int lru_tier_from_refs(unsigned int refs)
{
	BUILD_BUG_ON(fls(LRU_REFS_MAX - 1) > MAX_NR_TIERS - 1);
	VM_WARN_ON_ONCE(refs > LRU_REFS_MAX);
	if (refs < LRU_REFS_WORKINGSET)
		return 0;
	return fls(refs - 1);
}

static inline int page_lru_gen(struct page *page)
{
	unsigned long flags = READ_ONCE(page->flags);

	return ((flags & LRU_GEN_MASK) >> LRU_GEN_PGOFF) - 1;
}

static inline bool lru_refs_is_active(unsigned int refs)
{
	VM_WARN_ON_ONCE(refs > LRU_REFS_MAX);
	return refs >= LRU_REFS_ACTIVATED;
}

static inline void lru_gen_update_size(struct lruvec *lruvec, int type,
		struct page *page, int old_gen, int new_gen)
{
	int zone = page_zonenum(page);
	int delta = hpage_nr_pages(page);
	struct lru_gen_folio *lrugen = &lruvec->lrugen;

	VM_WARN_ON_ONCE(old_gen != -1 && old_gen >= MAX_NR_GENS);
	VM_WARN_ON_ONCE(new_gen != -1 && new_gen >= MAX_NR_GENS);
	VM_WARN_ON_ONCE(old_gen == -1 && new_gen == -1);

	if (old_gen >= 0)
		atomic_long_sub(delta, &lrugen->nr_pages[old_gen][type][zone]);
	if (new_gen >= 0)
		atomic_long_add(delta, &lrugen->nr_pages[new_gen][type][zone]);
}

static inline unsigned long lru_gen_page_seq(struct lruvec *lruvec, struct page *page,
					      bool reclaiming)
{
	int gen;
	int refs = page_lru_refs(page);
	int type = page_is_file_cache(page);
	struct lru_gen_folio *lrugen = &lruvec->lrugen;

	/* PG_active places isolated or refaulted pages in the newest gens. */
	if (PageActive(page))
		gen = MIN_NR_GENS - (refs >= LRU_REFS_WORKINGSET);
	else if (reclaiming)
		gen = MAX_NR_GENS;
	else if ((!page_is_file_cache(page) && !PageSwapCache(page)) ||
		 (PageReclaim(page) &&
		  (PageDirty(page) || PageWriteback(page))))
		gen = MIN_NR_GENS;
	else
		gen = MAX_NR_GENS - (refs >= LRU_REFS_WORKINGSET);

	return max(READ_ONCE(lrugen->max_seq) - gen + 1, READ_ONCE(lrugen->min_seq[type]));
}

static inline bool lru_gen_add_page(struct lruvec *lruvec, struct page *page, bool reclaiming)
{
	unsigned long seq;
	unsigned long flags;
	int gen, refs;
	int type = page_is_file_cache(page);
	int zone = page_zonenum(page);
	int delta = hpage_nr_pages(page);
	enum lru_list lru = type * LRU_INACTIVE_FILE;
	struct lru_gen_folio *lrugen = &lruvec->lrugen;

	BUILD_BUG_ON(BIT(LRU_GEN_WIDTH - 1) != MAX_NR_GENS);
	VM_WARN_ON_ONCE_PAGE(page_lru_gen(page) != -1, page);

	if (PageUnevictable(page) || !lrugen->enabled)
		return false;

	seq = lru_gen_page_seq(lruvec, page, reclaiming);
	gen = lru_gen_from_seq(seq);
	flags = (gen + 1UL) << LRU_GEN_PGOFF;
	/* see the comment on MIN_NR_GENS about PG_active */
	flags = set_mask_bits(&page->flags, LRU_GEN_MASK | BIT(PG_active), flags);
	/* Refs are unchanged by the mask, so the new snapshot suffices. */
	refs = lru_get_refs_flags(flags);

	lru_gen_update_size(lruvec, type, page, -1, gen);
	if (lru_refs_is_active(refs))
		lru += LRU_ACTIVE;
	update_lru_size(lruvec, lru, zone, delta);
	/* for rotate_reclaimable_page() */
	if (reclaiming)
		list_add_tail(&page->lru, &lrugen->folios[gen][type][zone]);
	else
		list_add(&page->lru, &lrugen->folios[gen][type][zone]);

	return true;
}

static inline bool lru_gen_del_page(struct lruvec *lruvec, struct page *page, bool reclaiming)
{
	unsigned long old_flags, new_flags;
	unsigned long max_seq = READ_ONCE(lruvec->lrugen.max_seq);
	int gen, refs;
	int type = page_is_file_cache(page);
	int zone = page_zonenum(page);
	int delta = hpage_nr_pages(page);
	enum lru_list lru = page_is_file_cache(page) * LRU_INACTIVE_FILE;

	/* 4.14 set_mask_bits() returns the new flags; retain the old snapshot. */
	do {
		old_flags = READ_ONCE(page->flags);
		new_flags = old_flags & ~LRU_GEN_MASK;
	} while (cmpxchg(&page->flags, old_flags, new_flags) != old_flags);
	gen = lru_get_gen_flags(old_flags);
	refs = lru_get_refs_flags(old_flags);
	if (gen < 0)
		return false;

	VM_WARN_ON_ONCE_PAGE(PageActive(page), page);
	VM_WARN_ON_ONCE_PAGE(PageUnevictable(page), page);

	/* Retain a hot generation's placement hint across migration. */
	if (!reclaiming && ((max_seq - gen) % MAX_NR_GENS) < MIN_NR_GENS)
		SetPageActive(page);

	lru_gen_update_size(lruvec, type, page, gen, -1);
	if (lru_refs_is_active(refs))
		lru += LRU_ACTIVE;
	update_lru_size(lruvec, lru, zone, -delta);
	list_del(&page->lru);

	return true;
}

#else /* !CONFIG_LRU_GEN */


static inline bool lru_gen_enabled(void)
{
	return false;
}

static inline bool lru_gen_switching(void)
{
	return false;
}

static inline bool lru_gen_in_fault(void)
{
	return false;
}

static inline bool lru_gen_add_page(struct lruvec *lruvec, struct page *page, bool reclaiming)
{
	return false;
}

static inline bool lru_gen_del_page(struct lruvec *lruvec, struct page *page, bool reclaiming)
{
	return false;
}

#endif /* CONFIG_LRU_GEN */

static __always_inline int PageWorkingset(struct page *page)
{
	return page_lru_refs(compound_head(page)) >= LRU_REFS_WORKINGSET;
}

/* Classical reclaim may still run during MGLRU switching. */
static __always_inline void SetPageWorkingset(struct page *page)
{
	VM_WARN_ON_ONCE(lru_gen_enabled() && !lru_gen_switching());
	set_bit(PG_workingset, &compound_head(page)->flags);
}


static __always_inline void page_inc_lru_refs_fast(struct page *page)
{
	unsigned long new_flags, old_flags;

	page = compound_head(page);

	if (!lru_gen_enabled()) {
		SetPageReferenced(page);
		return;
	}

	BUILD_BUG_ON(LRU_REFS_REFERENCED >= LRU_REFS_ACTIVATED);
	old_flags = READ_ONCE(page->flags);
	do {
		old_flags = READ_ONCE(page->flags);
		if (lru_get_refs_flags(old_flags))
			break;
		new_flags = old_flags;
		lru_set_refs_flags(&new_flags, LRU_REFS_REFERENCED);
	} while (cmpxchg(&page->flags, old_flags, new_flags) != old_flags);
}

#define lru_to_page(head) (list_entry((head)->prev, struct page, lru))

static __always_inline void add_page_to_lru_list(struct page *page,
				struct lruvec *lruvec)
{
	enum lru_list lru = page_lru(page);

	if (lru_gen_add_page(lruvec, page, false))
		return;

	update_lru_size(lruvec, lru, page_zonenum(page), hpage_nr_pages(page));
	list_add(&page->lru, &lruvec->lists[lru]);
}

static __always_inline void add_page_to_lru_list_tail(struct page *page,
				struct lruvec *lruvec)
{
	enum lru_list lru = page_lru(page);

	if (lru_gen_add_page(lruvec, page, true))
		return;

	update_lru_size(lruvec, lru, page_zonenum(page), hpage_nr_pages(page));
	list_add_tail(&page->lru, &lruvec->lists[lru]);
}

static __always_inline void del_page_from_lru_list(struct page *page,
				struct lruvec *lruvec)
{
	if (lru_gen_del_page(lruvec, page, false))
		return;

	list_del(&page->lru);
	update_lru_size(lruvec, page_lru(page), page_zonenum(page),
			-hpage_nr_pages(page));
}

static inline bool vma_has_recency(struct vm_area_struct *vma)
{
	if (vma->vm_flags & (VM_SEQ_READ | VM_RAND_READ))
		return false;

	if (vma->vm_file && (vma->vm_file->f_mode & FMODE_NOREUSE))
		return false;

	return true;
}

#endif
