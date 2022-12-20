// SPDX-License-Identifier: GPL-2.0
/*
 * Workingset detection
 *
 * Copyright (C) 2013 Red Hat, Inc., Johannes Weiner
 */

#include <linux/memcontrol.h>
#include <linux/writeback.h>
#include <linux/shmem_fs.h>
#include <linux/pagemap.h>
#include <linux/atomic.h>
#include <linux/module.h>
#include <linux/swap.h>
#include <linux/dax.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/mm_inline.h>

/*
 *		Double CLOCK lists
 *
 * Per node, two clock lists are maintained for file pages: the
 * inactive and the active list.  Freshly faulted pages start out at
 * the head of the inactive list and page reclaim scans pages from the
 * tail.  Pages that are accessed multiple times on the inactive list
 * are promoted to the active list, to protect them from reclaim,
 * whereas active pages are demoted to the inactive list when the
 * active list grows too big.
 *
 *   fault ------------------------+
 *                                 |
 *              +--------------+   |            +-------------+
 *   reclaim <- |   inactive   | <-+-- demotion |    active   | <--+
 *              +--------------+                +-------------+    |
 *                     |                                           |
 *                     +-------------- promotion ------------------+
 *
 *
 *		Access frequency and refault distance
 *
 * A workload is thrashing when its pages are frequently used but they
 * are evicted from the inactive list every time before another access
 * would have promoted them to the active list.
 *
 * In cases where the average access distance between thrashing pages
 * is bigger than the size of memory there is nothing that can be
 * done - the thrashing set could never fit into memory under any
 * circumstance.
 *
 * However, the average access distance could be bigger than the
 * inactive list, yet smaller than the size of memory.  In this case,
 * the set could fit into memory if it weren't for the currently
 * active pages - which may be used more, hopefully less frequently:
 *
 *      +-memory available to cache-+
 *      |                           |
 *      +-inactive------+-active----+
 *  a b | c d e f g h i | J K L M N |
 *      +---------------+-----------+
 *
 * It is prohibitively expensive to accurately track access frequency
 * of pages.  But a reasonable approximation can be made to measure
 * thrashing on the inactive list, after which refaulting pages can be
 * activated optimistically to compete with the existing active pages.
 *
 * Approximating inactive page access frequency - Observations:
 *
 * 1. When a page is accessed for the first time, it is added to the
 *    head of the inactive list, slides every existing inactive page
 *    towards the tail by one slot, and pushes the current tail page
 *    out of memory.
 *
 * 2. When a page is accessed for the second time, it is promoted to
 *    the active list, shrinking the inactive list by one slot.  This
 *    also slides all inactive pages that were faulted into the cache
 *    more recently than the activated page towards the tail of the
 *    inactive list.
 *
 * Thus:
 *
 * 1. The sum of evictions and activations between any two points in
 *    time indicate the minimum number of inactive pages accessed in
 *    between.
 *
 * 2. Moving one inactive page N page slots towards the tail of the
 *    list requires at least N inactive page accesses.
 *
 * Combining these:
 *
 * 1. When a page is finally evicted from memory, the number of
 *    inactive pages accessed while the page was in cache is at least
 *    the number of page slots on the inactive list.
 *
 * 2. In addition, measuring the sum of evictions and activations (E)
 *    at the time of a page's eviction, and comparing it to another
 *    reading (R) at the time the page faults back into memory tells
 *    the minimum number of accesses while the page was not cached.
 *    This is called the refault distance.
 *
 * Because the first access of the page was the fault and the second
 * access the refault, we combine the in-cache distance with the
 * out-of-cache distance to get the complete minimum access distance
 * of this page:
 *
 *      NR_inactive + (R - E)
 *
 * And knowing the minimum access distance of a page, we can easily
 * tell if the page would be able to stay in cache assuming all page
 * slots in the cache were available:
 *
 *   NR_inactive + (R - E) <= NR_inactive + NR_active
 *
 * which can be further simplified to
 *
 *   (R - E) <= NR_active
 *
 * Put into words, the refault distance (out-of-cache) can be seen as
 * a deficit in inactive list space (in-cache).  If the inactive list
 * had (R - E) more page slots, the page would not have been evicted
 * in between accesses, but activated instead.  And on a full system,
 * the only thing eating into inactive list space is active pages.
 *
 *
 *		Refaulting inactive pages
 *
 * All that is known about the active list is that the pages have been
 * accessed more than once in the past.  This means that at any given
 * time there is actually a good chance that pages on the active list
 * are no longer in active use.
 *
 * So when a refault distance of (R - E) is observed and there are at
 * least (R - E) active pages, the refaulting page is activated
 * optimistically in the hope that (R - E) active pages are actually
 * used less frequently than the refaulting page - or even not used at
 * all anymore.
 *
 * That means if inactive cache is refaulting with a suitable refault
 * distance, we assume the cache workingset is transitioning and put
 * pressure on the current active list.
 *
 * If this is wrong and demotion kicks in, the pages which are truly
 * used more frequently will be reactivated while the less frequently
 * used once will be evicted from memory.
 *
 * But if this is right, the stale pages will be pushed out of memory
 * and the used pages get to stay in cache.
 *
 *		Refaulting active pages
 *
 * If on the other hand the refaulting pages have recently been
 * deactivated, it means that the active list is no longer protecting
 * actively used cache from reclaim. The cache is NOT transitioning to
 * a different workingset; the existing workingset is thrashing in the
 * space allocated to the page cache.
 *
 *
 *		Implementation
 *
 * For each node's file LRU lists, a counter for inactive evictions
 * and activations is maintained (node->inactive_age).
 *
 * On eviction, a snapshot of this counter (along with some bits to
 * identify the node) is stored in the now empty page cache
 * slot of the evicted page.  This is called a shadow entry.
 *
 * On cache misses for which there are shadow entries, an eligible
 * refault distance will immediately activate the refaulting page.
 */

#define EVICTION_SHIFT		(BITS_PER_XA_VALUE - MEM_CGROUP_ID_SHIFT - NODES_SHIFT)
#define EVICTION_MASK		(BIT(EVICTION_SHIFT) - 1)
#define WORKINGSET_WIDTH	1

/*
 * Eviction timestamps need to be able to cover the full range of
 * actionable refaults. However, bits are tight in the xarray
 * entry, and after storing the identifier for the lruvec there might
 * not be enough left to represent every single actionable refault. In
 * that case, we have to sacrifice granularity for distance, and group
 * evictions into coarser buckets by shaving off lower timestamp bits.
 */
static unsigned int bucket_order __read_mostly;

static void *pack_shadow(int memcg_id, struct pglist_data *pgdat, unsigned long val)
{
	val = (val << MEM_CGROUP_ID_SHIFT) | memcg_id;
	val = (val << NODES_SHIFT) | pgdat->node_id;

	return xa_mk_value(val);
}

static unsigned long unpack_shadow(void *shadow, int *memcg_id, struct pglist_data **pgdat)
{
	unsigned long val = xa_to_value(shadow);

	*pgdat = NODE_DATA(val & (BIT(NODES_SHIFT) - 1));
	val >>= NODES_SHIFT;
	*memcg_id = val & (BIT(MEM_CGROUP_ID_SHIFT) - 1);

	return val >> MEM_CGROUP_ID_SHIFT;
}

#ifdef CONFIG_LRU_GEN

#if LRU_GEN_SHIFT + LRU_USAGE_SHIFT >= EVICTION_SHIFT
#error "Please try smaller NODES_SHIFT, NR_LRU_GENS and TIERS_PER_GEN configurations"
#endif

static void page_set_usage(struct page *page, int usage)
{
	unsigned long old_flags, new_flags;

	VM_BUG_ON(usage > BIT(LRU_USAGE_WIDTH));

	if (!usage)
		return;

	do {
		old_flags = READ_ONCE(page->flags);
		new_flags = (old_flags & ~LRU_USAGE_MASK) | LRU_TIER_FLAGS |
			    ((usage - 1UL) << LRU_USAGE_PGOFF);
	} while (new_flags != old_flags &&
		 cmpxchg(&page->flags, old_flags, new_flags) != old_flags);
}

/* Return a token to be stored in the shadow entry of a page being evicted. */
void *lru_gen_eviction(struct page *page)
{
	int hist, tier;
	unsigned long token;
	unsigned long min_seq;
	struct lruvec *lruvec;
	struct lrugen *lrugen;
	int type = page_is_file_cache(page);
	int usage = page_tier_usage(page);
	struct mem_cgroup *memcg = page_memcg(page);
	struct pglist_data *pgdat = page_pgdat(page);

	if (!mem_cgroup_disabled() && !memcg)
		return NULL;

	lruvec = mem_cgroup_lruvec(pgdat, memcg);
	lrugen = &lruvec->evictable;
	min_seq = READ_ONCE(lrugen->min_seq[type]);
	token = (min_seq << LRU_USAGE_SHIFT) | usage;

	hist = hist_from_seq_or_gen(min_seq);
	tier = lru_tier_from_usage(usage);
	atomic_long_add(hpage_nr_pages(page), &lrugen->evicted[hist][type][tier]);

	return pack_shadow(mem_cgroup_id(memcg), pgdat, token);
}

/* Account a refaulted page based on the token stored in its shadow entry. */
void lru_gen_refault(struct page *page, void *shadow)
{
	int hist, tier, usage;
	int memcg_id;
	unsigned long token;
	unsigned long min_seq;
	struct lruvec *lruvec;
	struct lrugen *lrugen;
	struct pglist_data *pgdat;
	struct mem_cgroup *memcg;
	int type = page_is_file_cache(page);

	token = unpack_shadow(shadow, &memcg_id, &pgdat);
	if (page_pgdat(page) != pgdat)
		return;

	rcu_read_lock();
	memcg = mem_cgroup_from_id(memcg_id);
	if (!mem_cgroup_disabled() && !memcg)
		goto unlock;

	usage = token & (BIT(LRU_USAGE_SHIFT) - 1);
	token >>= LRU_USAGE_SHIFT;

	lruvec = mem_cgroup_lruvec(pgdat, memcg);
	lrugen = &lruvec->evictable;
	min_seq = READ_ONCE(lrugen->min_seq[type]);
	if (token != (min_seq & (EVICTION_MASK >> LRU_USAGE_SHIFT)))
		goto unlock;

	page_set_usage(page, usage);

	hist = hist_from_seq_or_gen(min_seq);
	tier = lru_tier_from_usage(usage);
	atomic_long_add(hpage_nr_pages(page), &lrugen->refaulted[hist][type][tier]);
	inc_lruvec_state(lruvec, WORKINGSET_REFAULT);
	if (tier)
		inc_lruvec_state(lruvec, WORKINGSET_RESTORE);
unlock:
	rcu_read_unlock();
}

#endif /* CONFIG_LRU_GEN */

/**
 * workingset_eviction - note the eviction of a page from memory
 * @page: the page being evicted
 *
 * Returns a shadow entry to be stored in @page->mapping->i_pages in place
 * of the evicted @page so that a later refault can be detected.
 */
void *workingset_eviction(struct page *page)
{
	struct pglist_data *pgdat = page_pgdat(page);
	struct mem_cgroup *memcg = page_memcg(page);
	int memcgid = mem_cgroup_id(memcg);
	unsigned long eviction;
	struct lruvec *lruvec;

	/* Page is fully exclusive and pins page->mem_cgroup */
	VM_BUG_ON_PAGE(PageLRU(page), page);
	VM_BUG_ON_PAGE(page_count(page), page);
	VM_BUG_ON_PAGE(!PageLocked(page), page);

	if (lru_gen_enabled())
		return lru_gen_eviction(page);

	lruvec = mem_cgroup_lruvec(pgdat, memcg);
	eviction = atomic_long_inc_return(&lruvec->inactive_age);
	eviction >>= bucket_order;
	eviction = (eviction << WORKINGSET_WIDTH) | PageWorkingset(page);
	return pack_shadow(memcgid, pgdat, eviction);
}

/**
 * workingset_refault - evaluate the refault of a previously evicted page
 * @page: the freshly allocated replacement page
 * @shadow: shadow entry of the evicted page
 *
 * Calculates and evaluates the refault distance of the previously
 * evicted page in the context of the node it was allocated in.
 */
void workingset_refault(struct page *page, void *shadow)
{
	unsigned long refault_distance;
	struct pglist_data *pgdat;
	unsigned long active_file;
	struct mem_cgroup *memcg;
	unsigned long eviction;
	struct lruvec *lruvec;
	unsigned long refault;
	bool workingset;
	int memcgid;

	if (lru_gen_enabled()) {
		lru_gen_refault(page, shadow);
		return;
	}

	eviction = unpack_shadow(shadow, &memcgid, &pgdat);

	rcu_read_lock();
	/*
	 * Look up the memcg associated with the stored ID. It might
	 * have been deleted since the page's eviction.
	 *
	 * Note that in rare events the ID could have been recycled
	 * for a new cgroup that refaults a shared page. This is
	 * impossible to tell from the available data. However, this
	 * should be a rare and limited disturbance, and activations
	 * are always speculative anyway. Ultimately, it's the aging
	 * algorithm's job to shake out the minimum access frequency
	 * for the active cache.
	 *
	 * XXX: On !CONFIG_MEMCG, this will always return NULL; it
	 * would be better if the root_mem_cgroup existed in all
	 * configurations instead.
	 */
	memcg = mem_cgroup_from_id(memcgid);
	if (!mem_cgroup_disabled() && !memcg)
		goto out;
	lruvec = mem_cgroup_lruvec(pgdat, memcg);
	refault = atomic_long_read(&lruvec->inactive_age);
	workingset = eviction & (BIT(WORKINGSET_WIDTH) - 1);
	eviction = (eviction >> WORKINGSET_WIDTH) << bucket_order;
	active_file = lruvec_lru_size(lruvec, LRU_ACTIVE_FILE, MAX_NR_ZONES);

	/*
	 * Calculate the refault distance
	 *
	 * The unsigned subtraction here gives an accurate distance
	 * across inactive_age overflows in most cases. There is a
	 * special case: usually, shadow entries have a short lifetime
	 * and are either refaulted or reclaimed along with the inode
	 * before they get too old.  But it is not impossible for the
	 * inactive_age to lap a shadow entry in the field, which can
	 * then result in a false small refault distance, leading to a
	 * false activation should this old entry actually refault
	 * again.  However, earlier kernels used to deactivate
	 * unconditionally with *every* reclaim invocation for the
	 * longest time, so the occasional inappropriate activation
	 * leading to pressure on the active list is not a problem.
	 */
	refault_distance = (refault - eviction) & (EVICTION_MASK >> WORKINGSET_WIDTH);

	inc_lruvec_state(lruvec, WORKINGSET_REFAULT);

	/*
	 * Compare the distance to the existing workingset size. We
	 * don't act on pages that couldn't stay resident even if all
	 * the memory was available to the page cache.
	 */
	if (refault_distance > active_file)
		goto out;

	SetPageActive(page);
	atomic_long_inc(&lruvec->inactive_age);
	inc_lruvec_state(lruvec, WORKINGSET_ACTIVATE);

	/* Page was active prior to eviction */
	if (workingset) {
		SetPageWorkingset(page);
		inc_lruvec_state(lruvec, WORKINGSET_RESTORE);
	}
out:
	rcu_read_unlock();
}

/**
 * workingset_activation - note a page activation
 * @page: page that is being activated
 */
void workingset_activation(struct page *page)
{
	struct mem_cgroup *memcg;
	struct lruvec *lruvec;

	rcu_read_lock();
	/*
	 * Filter non-memcg pages here, e.g. unmap can call
	 * mark_page_accessed() on VDSO pages.
	 *
	 * XXX: See workingset_refault() - this should return
	 * root_mem_cgroup even for !CONFIG_MEMCG.
	 */
	memcg = page_memcg_rcu(page);
	if (!mem_cgroup_disabled() && !memcg)
		goto out;
	lruvec = mem_cgroup_lruvec(page_pgdat(page), memcg);
	atomic_long_inc(&lruvec->inactive_age);
out:
	rcu_read_unlock();
}

/*
 * Shadow entries reflect the share of the working set that does not
 * fit into memory, so their number depends on the access pattern of
 * the workload.  In most cases, they will refault or get reclaimed
 * along with the inode, but a (malicious) workload that streams
 * through files with a total size several times that of available
 * memory, while preventing the inodes from being reclaimed, can
 * create excessive amounts of shadow nodes.  To keep a lid on this,
 * track shadow nodes and reclaim them when they grow way past the
 * point where they would still be useful.
 */

static struct list_lru shadow_nodes;

void workingset_update_node(struct xa_node *node)
{
	/*
	 * Track non-empty nodes that contain only shadow entries;
	 * unlink those that contain pages or are being freed.
	 *
	 * Avoid acquiring the list_lru lock when the nodes are
	 * already where they should be. The list_empty() test is safe
	 * as node->private_list is protected by the i_pages lock.
	 */
	VM_WARN_ON_ONCE(!irqs_disabled());  /* For __inc_lruvec_page_state */

	if (node->count && node->count == node->nr_values) {
		if (list_empty(&node->private_list)) {
			list_lru_add(&shadow_nodes, &node->private_list);
			__inc_lruvec_slab_state(node, WORKINGSET_NODES);
		}
	} else {
		if (!list_empty(&node->private_list)) {
			list_lru_del(&shadow_nodes, &node->private_list);
			__dec_lruvec_slab_state(node, WORKINGSET_NODES);
		}
	}
}

static unsigned long count_shadow_nodes(struct shrinker *shrinker,
					struct shrink_control *sc)
{
	unsigned long max_nodes;
	unsigned long nodes;
	unsigned long pages;

	nodes = list_lru_shrink_count(&shadow_nodes, sc);

	/*
	 * Approximate a reasonable limit for the nodes
	 * containing shadow entries. We don't need to keep more
	 * shadow entries than possible pages on the active list,
	 * since refault distances bigger than that are dismissed.
	 *
	 * The size of the active list converges toward 100% of
	 * overall page cache as memory grows, with only a tiny
	 * inactive list. Assume the total cache size for that.
	 *
	 * Nodes might be sparsely populated, with only one shadow
	 * entry in the extreme case. Obviously, we cannot keep one
	 * node for every eligible shadow entry, so compromise on a
	 * worst-case density of 1/8th. Below that, not all eligible
	 * refaults can be detected anymore.
	 *
	 * On 64-bit with 7 xa_nodes per page and 64 slots
	 * each, this will reclaim shadow entries when they consume
	 * ~1.8% of available memory:
	 *
	 * PAGE_SIZE / xa_nodes / node_entries * 8 / PAGE_SIZE
	 */
#ifdef CONFIG_MEMCG
	if (sc->memcg) {
		struct lruvec *lruvec;
		int i;

		lruvec = mem_cgroup_lruvec(NODE_DATA(sc->nid), sc->memcg);
		for (pages = 0, i = 0; i < NR_LRU_LISTS; i++)
			pages += lruvec_page_state_local(lruvec,
							 NR_LRU_BASE + i);
		pages += lruvec_page_state_local(lruvec, NR_SLAB_RECLAIMABLE);
		pages += lruvec_page_state_local(lruvec, NR_SLAB_UNRECLAIMABLE);
	} else
#endif
		pages = node_present_pages(sc->nid);

	max_nodes = pages >> (XA_CHUNK_SHIFT - 3);

	if (!nodes)
		return SHRINK_EMPTY;

	if (nodes <= max_nodes)
		return 0;
	return nodes - max_nodes;
}

static enum lru_status shadow_lru_isolate(struct list_head *item,
					  struct list_lru_one *lru,
					  spinlock_t *lru_lock,
					  void *arg) __must_hold(lru_lock)
{
	struct xa_node *node = container_of(item, struct xa_node, private_list);
	XA_STATE(xas, node->array, 0);
	struct address_space *mapping;
	int ret;

	/*
	 * Page cache insertions and deletions synchroneously maintain
	 * the shadow node LRU under the i_pages lock and the
	 * lru_lock.  Because the page cache tree is emptied before
	 * the inode can be destroyed, holding the lru_lock pins any
	 * address_space that has nodes on the LRU.
	 *
	 * We can then safely transition to the i_pages lock to
	 * pin only the address_space of the particular node we want
	 * to reclaim, take the node off-LRU, and drop the lru_lock.
	 */

	mapping = container_of(node->array, struct address_space, i_pages);

	/* Coming from the list, invert the lock order */
	if (!xa_trylock(&mapping->i_pages)) {
		spin_unlock_irq(lru_lock);
		ret = LRU_RETRY;
		goto out;
	}

	list_lru_isolate(lru, item);
	__dec_lruvec_slab_state(node, WORKINGSET_NODES);

	spin_unlock(lru_lock);

	/*
	 * The nodes should only contain one or more shadow entries,
	 * no pages, so we expect to be able to remove them all and
	 * delete and free the empty node afterwards.
	 */
	if (WARN_ON_ONCE(!node->nr_values))
		goto out_invalid;
	if (WARN_ON_ONCE(node->count != node->nr_values))
		goto out_invalid;
	mapping->nrexceptional -= node->nr_values;
	xas.xa_node = xa_parent_locked(&mapping->i_pages, node);
	xas.xa_offset = node->offset;
	xas.xa_shift = node->shift + XA_CHUNK_SHIFT;
	xas_set_update(&xas, workingset_update_node);
	/*
	 * We could store a shadow entry here which was the minimum of the
	 * shadow entries we were tracking ...
	 */
	xas_store(&xas, NULL);
	__inc_lruvec_slab_state(node, WORKINGSET_NODERECLAIM);

out_invalid:
	xa_unlock_irq(&mapping->i_pages);
	ret = LRU_REMOVED_RETRY;
out:
	cond_resched();
	spin_lock_irq(lru_lock);
	return ret;
}

static unsigned long scan_shadow_nodes(struct shrinker *shrinker,
				       struct shrink_control *sc)
{
	/* list_lru lock nests inside the IRQ-safe i_pages lock */
	return list_lru_shrink_walk_irq(&shadow_nodes, sc, shadow_lru_isolate,
					NULL);
}

static struct shrinker workingset_shadow_shrinker = {
	.count_objects = count_shadow_nodes,
	.scan_objects = scan_shadow_nodes,
	.seeks = 0, /* ->count reports only fully expendable nodes */
	.flags = SHRINKER_NUMA_AWARE | SHRINKER_MEMCG_AWARE,
};

/*
 * Our list_lru->lock is IRQ-safe as it nests inside the IRQ-safe
 * i_pages lock.
 */
static struct lock_class_key shadow_nodes_key;

static int __init workingset_init(void)
{
	unsigned int timestamp_bits;
	unsigned int max_order;
	int ret;

	BUILD_BUG_ON(EVICTION_SHIFT < WORKINGSET_WIDTH);
	/*
	 * Calculate the eviction bucket size to cover the longest
	 * actionable refault distance, which is currently half of
	 * memory (totalram_pages/2). However, memory hotplug may add
	 * some more pages at runtime, so keep working with up to
	 * double the initial memory by using totalram_pages as-is.
	 */
	timestamp_bits = EVICTION_SHIFT - WORKINGSET_WIDTH;
	max_order = fls_long(totalram_pages() - 1);
	if (max_order > timestamp_bits)
		bucket_order = max_order - timestamp_bits;
	pr_info("workingset: timestamp_bits=%d max_order=%d bucket_order=%u\n",
	       timestamp_bits, max_order, bucket_order);

	ret = prealloc_shrinker(&workingset_shadow_shrinker);
	if (ret)
		goto err;
	ret = __list_lru_init(&shadow_nodes, true, &shadow_nodes_key,
			      &workingset_shadow_shrinker);
	if (ret)
		goto err_list_lru;
	register_shrinker_prepared(&workingset_shadow_shrinker);
	return 0;
err_list_lru:
	free_prealloced_shrinker(&workingset_shadow_shrinker);
err:
	return ret;
}
module_init(workingset_init);
