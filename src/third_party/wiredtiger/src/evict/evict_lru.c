/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int  __evict_clear_all_walks(WT_SESSION_IMPL *);
static int  WT_CDECL __evict_lru_cmp(const void *, const void *);
static int  __evict_lru_pages(WT_SESSION_IMPL *, bool);
static int  __evict_lru_walk(WT_SESSION_IMPL *);
static int  __evict_page(WT_SESSION_IMPL *, bool);
static int  __evict_pass(WT_SESSION_IMPL *);
static int  __evict_server(WT_SESSION_IMPL *, bool *);
static int  __evict_walk(WT_SESSION_IMPL *, WT_EVICT_QUEUE *);
static int  __evict_walk_file(
    WT_SESSION_IMPL *, WT_EVICT_QUEUE *, u_int, u_int *);

#define	WT_EVICT_HAS_WORKERS(s)				\
	(S2C(s)->evict_threads.current_threads > 1)

/*
 * __evict_entry_priority --
 *	Get the adjusted read generation for an eviction entry.
 */
static inline uint64_t
__evict_entry_priority(WT_SESSION_IMPL *session, WT_REF *ref)
{
	WT_BTREE *btree;
	WT_PAGE *page;
	uint64_t read_gen;

	btree = S2BT(session);
	page = ref->page;

	/* Any page set to the oldest generation should be discarded. */
	if (page->read_gen == WT_READGEN_OLDEST)
		return (WT_READGEN_OLDEST);

	/*
	 * Any leaf page from a dead tree is a great choice (not internal pages,
	 * they may have children and are not yet evictable).
	 */
	if (!WT_PAGE_IS_INTERNAL(page) &&
	    F_ISSET(btree->dhandle, WT_DHANDLE_DEAD))
		return (WT_READGEN_OLDEST);

	/* Any empty page (leaf or internal), is a good choice. */
	if (__wt_page_is_empty(page))
		return (WT_READGEN_OLDEST);

	/* Any large page in memory is likewise a good choice. */
	if (page->memory_footprint > btree->splitmempage)
		return (WT_READGEN_OLDEST);

	/*
	 * The base read-generation is skewed by the eviction priority.
	 * Internal pages are also adjusted, we prefer to evict leaf pages.
	 */
	if (page->modify != NULL &&
	    F_ISSET(S2C(session)->cache, WT_CACHE_EVICT_DIRTY) &&
	    !F_ISSET(S2C(session)->cache, WT_CACHE_EVICT_CLEAN))
		read_gen = page->modify->update_txn;
	else
		read_gen = page->read_gen;

	read_gen += btree->evict_priority;
	if (WT_PAGE_IS_INTERNAL(page))
		read_gen += WT_EVICT_INT_SKEW;

	return (read_gen);
}

/*
 * __evict_lru_cmp --
 *	Qsort function: sort the eviction array.
 */
static int WT_CDECL
__evict_lru_cmp(const void *a_arg, const void *b_arg)
{
	const WT_EVICT_ENTRY *a = a_arg, *b = b_arg;
	uint64_t a_score, b_score;

	a_score = (a->ref == NULL ? UINT64_MAX : a->score);
	b_score = (b->ref == NULL ? UINT64_MAX : b->score);

	return ((a_score < b_score) ? -1 : (a_score == b_score) ? 0 : 1);
}

/*
 * __evict_list_clear --
 *	Clear an entry in the LRU eviction list.
 */
static inline void
__evict_list_clear(WT_SESSION_IMPL *session, WT_EVICT_ENTRY *e)
{
	if (e->ref != NULL) {
		WT_ASSERT(session,
		    F_ISSET_ATOMIC(e->ref->page, WT_PAGE_EVICT_LRU));
		F_CLR_ATOMIC(e->ref->page, WT_PAGE_EVICT_LRU);
	}
	e->ref = NULL;
	e->btree = WT_DEBUG_POINT;
}

/*
 * __wt_evict_list_clear_page --
 *	Make sure a page is not in the LRU eviction list.  This called from the
 *	page eviction code to make sure there is no attempt to evict a child
 *	page multiple times.
 */
void
__wt_evict_list_clear_page(WT_SESSION_IMPL *session, WT_REF *ref)
{
	WT_CACHE *cache;
	WT_EVICT_ENTRY *evict;
	uint32_t i, elem, q;
	bool found;

	WT_ASSERT(session,
	    __wt_ref_is_root(ref) || ref->state == WT_REF_LOCKED);

	/* Fast path: if the page isn't on the queue, don't bother searching. */
	if (!F_ISSET_ATOMIC(ref->page, WT_PAGE_EVICT_LRU))
		return;

	cache = S2C(session)->cache;
	__wt_spin_lock(session, &cache->evict_queue_lock);

	found = false;
	for (q = 0; q < WT_EVICT_QUEUE_MAX && !found; q++) {
		__wt_spin_lock(session, &cache->evict_queues[q].evict_lock);
		elem = cache->evict_queues[q].evict_max;
		for (i = 0, evict = cache->evict_queues[q].evict_queue;
		    i < elem; i++, evict++)
			if (evict->ref == ref) {
				found = true;
				__evict_list_clear(session, evict);
				break;
			}
		__wt_spin_unlock(session, &cache->evict_queues[q].evict_lock);
	}
	WT_ASSERT(session,
	    !F_ISSET_ATOMIC(ref->page, WT_PAGE_EVICT_LRU));

	__wt_spin_unlock(session, &cache->evict_queue_lock);
}

/*
 * __evict_queue_empty --
 *	Is the queue empty?
 *
 *	Note that the eviction server is pessimistic and treats a half full
 *	queue as empty.
 */
static inline bool
__evict_queue_empty(WT_EVICT_QUEUE *queue, bool server_check)
{
	uint32_t candidates, used;

	if (queue->evict_current == NULL)
		return (true);

	/* The eviction server only considers half of the candidates. */
	candidates = queue->evict_candidates;
	if (server_check && candidates > 1)
		candidates /= 2;
	used = (uint32_t)(queue->evict_current - queue->evict_queue);
	return (used >= candidates);
}

/*
 * __evict_queue_full --
 *	Is the queue full (i.e., it has been populated with candidates and none
 *	of them have been evicted yet)?
 */
static inline bool
__evict_queue_full(WT_EVICT_QUEUE *queue)
{
	return (queue->evict_current == queue->evict_queue &&
	    queue->evict_candidates != 0);
}

/*
 * __wt_evict_server_wake --
 *	Wake the eviction server thread.
 */
void
__wt_evict_server_wake(WT_SESSION_IMPL *session)
{
	WT_CACHE *cache;
	WT_CONNECTION_IMPL *conn;

	conn = S2C(session);
	cache = conn->cache;

#ifdef HAVE_VERBOSE
	if (WT_VERBOSE_ISSET(session, WT_VERB_EVICTSERVER)) {
		uint64_t bytes_inuse, bytes_max;

		bytes_inuse = __wt_cache_bytes_inuse(cache);
		bytes_max = conn->cache_size;
		__wt_verbose(session, WT_VERB_EVICTSERVER,
		    "waking, bytes inuse %s max (%" PRIu64
		    "MB %s %" PRIu64 "MB)",
		    bytes_inuse <= bytes_max ? "<=" : ">",
		    bytes_inuse / WT_MEGABYTE,
		    bytes_inuse <= bytes_max ? "<=" : ">",
		    bytes_max / WT_MEGABYTE);
	}
#endif

	__wt_cond_auto_signal(session, cache->evict_cond);
}

/*
 * __wt_evict_thread_run --
 *	Starting point for an eviction thread.
 */
int
__wt_evict_thread_run(WT_SESSION_IMPL *session, WT_THREAD *thread)
{
	WT_CACHE *cache;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	bool did_work;

	conn = S2C(session);
	cache = conn->cache;

#ifdef HAVE_DIAGNOSTIC
	/*
	 * Ensure the cache stuck timer is initialized when starting eviction.
	 */
	if (thread->id == 0)
		__wt_epoch(session, &cache->stuck_ts);
#endif

	while (F_ISSET(conn, WT_CONN_EVICTION_RUN) &&
	    F_ISSET(thread, WT_THREAD_RUN)) {
		if (conn->evict_server_running &&
		    __wt_spin_trylock(session, &cache->evict_pass_lock) == 0) {
			/*
			 * Cannot use WT_WITH_PASS_LOCK because this is a try
			 * lock.  Fix when that is supported.  We set the flag
			 * on both sessions because we may call clear_walk when
			 * we are walking with the walk session, locked.
			 */
			F_SET(session, WT_SESSION_LOCKED_PASS);
			F_SET(cache->walk_session, WT_SESSION_LOCKED_PASS);
			ret = __evict_server(session, &did_work);
			F_CLR(cache->walk_session, WT_SESSION_LOCKED_PASS);
			F_CLR(session, WT_SESSION_LOCKED_PASS);
			__wt_spin_unlock(session, &cache->evict_pass_lock);
			WT_ERR(ret);
			__wt_verbose(session, WT_VERB_EVICTSERVER, "sleeping");
			/* Don't rely on signals: check periodically. */
			__wt_cond_auto_wait(
			    session, cache->evict_cond, did_work);
			__wt_verbose(session, WT_VERB_EVICTSERVER, "waking");
		} else
			WT_ERR(__evict_lru_pages(session, false));
	}

	/*
	 * The only time the first eviction thread is stopped is on shutdown:
	 * in case any trees are still open, clear all walks now so that they
	 * can be closed.
	 */
	if (thread->id == 0) {
		WT_WITH_PASS_LOCK(session, ret,
		    ret = __evict_clear_all_walks(session));
		WT_ERR(ret);
		/*
		 * The only two cases when the eviction server is expected to
		 * stop are when recovery is finished or when the connection is
		 * closing.
		 */
		WT_ASSERT(session,
		    F_ISSET(conn, WT_CONN_CLOSING | WT_CONN_RECOVERING));
	}

	__wt_verbose(
	    session, WT_VERB_EVICTSERVER, "cache eviction thread exiting");

	if (0) {
err:		WT_PANIC_MSG(session, ret, "cache eviction thread error");
	}
	return (ret);
}

/*
 * __evict_server --
 *	Thread to evict pages from the cache.
 */
static int
__evict_server(WT_SESSION_IMPL *session, bool *did_work)
{
	WT_CACHE *cache;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
#ifdef HAVE_DIAGNOSTIC
	struct timespec now;
#endif
	uint64_t orig_pages_evicted;
	u_int spins;

	conn = S2C(session);
	cache = conn->cache;
	WT_ASSERT(session, did_work != NULL);
	*did_work = false;
	orig_pages_evicted = cache->pages_evicted;

	/* Evict pages from the cache as needed. */
	WT_RET(__evict_pass(session));

	if (!F_ISSET(conn, WT_CONN_EVICTION_RUN))
		return (0);

	/*
	 * Clear the walks so we don't pin pages while asleep,
	 * otherwise we can block applications evicting large pages.
	 */
	if (!__wt_cache_stuck(session)) {
		for (spins = 0; (ret = __wt_spin_trylock(
		    session, &conn->dhandle_lock)) == EBUSY &&
		    cache->pass_intr == 0; spins++) {
			if (spins < WT_THOUSAND)
				__wt_yield();
			else
				__wt_sleep(0, WT_THOUSAND);
		}
		/*
		 * If we gave up acquiring the lock, that indicates a
		 * session is waiting for us to clear walks.  Do that
		 * as part of a normal pass (without the handle list
		 * lock) to avoid deadlock.
		 */
		if (ret == EBUSY)
			return (0);
		WT_RET(ret);
		ret = __evict_clear_all_walks(session);
		__wt_spin_unlock(session, &conn->dhandle_lock);
		WT_RET(ret);

		cache->pages_evicted = 0;
	} else if (cache->pages_evicted != cache->pages_evict) {
		cache->pages_evicted = cache->pages_evict;
#ifdef HAVE_DIAGNOSTIC
		__wt_epoch(session, &cache->stuck_ts);
	} else {
		/* After being stuck for 5 minutes, give up. */
		__wt_epoch(session, &now);
		if (WT_TIMEDIFF_SEC(now, cache->stuck_ts) > 300) {
			ret = ETIMEDOUT;
			__wt_err(session, ret,
			    "Cache stuck for too long, giving up");
			WT_TRET(__wt_cache_dump(session, NULL));
			return (ret);
		}
#endif
	}
	*did_work = cache->pages_evicted != orig_pages_evicted;
	return (0);
}

/*
 * __wt_evict_create --
 *	Start the eviction server.
 */
int
__wt_evict_create(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;

	conn = S2C(session);

	WT_ASSERT(session, conn->evict_threads_min > 0);
	/* Set first, the thread might run before we finish up. */
	F_SET(conn, WT_CONN_EVICTION_RUN);

	/* Create the eviction thread group */
	WT_RET(__wt_thread_group_create(session, &conn->evict_threads,
	    "eviction-server", conn->evict_threads_min,
	    conn->evict_threads_max, WT_THREAD_CAN_WAIT | WT_THREAD_PANIC_FAIL,
	    __wt_evict_thread_run));

	/*
	 * Allow queues to be populated now that the eviction threads
	 * are running.
	 */
	conn->evict_server_running = true;

	return (0);
}

/*
 * __wt_evict_destroy --
 *	Destroy the eviction threads.
 */
int
__wt_evict_destroy(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;

	conn = S2C(session);

	/* We are done if the eviction server didn't start successfully. */
	if (!conn->evict_server_running)
		return (0);

	/* Wait for any eviction thread group changes to stabilize. */
	__wt_writelock(session, conn->evict_threads.lock);

	/*
	 * Signal the threads to finish and stop populating the queue.
	 */
	F_CLR(conn, WT_CONN_EVICTION_RUN);
	conn->evict_server_running = false;
	__wt_evict_server_wake(session);

	__wt_verbose(
	    session, WT_VERB_EVICTSERVER, "waiting for helper threads");

	/*
	 * We call the destroy function still holding the write lock.
	 * It assumes it is called locked.
	 */
	WT_RET(__wt_thread_group_destroy(session, &conn->evict_threads));

	return (0);
}

/*
 * __evict_update_work --
 *	Configure eviction work state.
 */
static bool
__evict_update_work(WT_SESSION_IMPL *session)
{
	WT_CACHE *cache;
	WT_CONNECTION_IMPL *conn;
	uint64_t bytes_inuse, bytes_max, dirty_inuse;

	conn = S2C(session);
	cache = conn->cache;

	/* Clear previous state. */
	F_CLR(cache, WT_CACHE_EVICT_MASK);

	if (!F_ISSET(conn, WT_CONN_EVICTION_RUN))
		return (false);

	if (!__evict_queue_empty(cache->evict_urgent_queue, false))
		F_SET(cache, WT_CACHE_EVICT_URGENT);

	/*
	 * If we need space in the cache, try to find clean pages to evict.
	 *
	 * Avoid division by zero if the cache size has not yet been set in a
	 * shared cache.
	 */
	bytes_max = conn->cache_size + 1;
	bytes_inuse = __wt_cache_bytes_inuse(cache);
	if (__wt_eviction_clean_needed(session, NULL))
		F_SET(cache, WT_CACHE_EVICT_CLEAN | WT_CACHE_EVICT_CLEAN_HARD);
	else if (bytes_inuse > (cache->eviction_target * bytes_max) / 100)
		F_SET(cache, WT_CACHE_EVICT_CLEAN);

	dirty_inuse = __wt_cache_dirty_leaf_inuse(cache);
	if (__wt_eviction_dirty_needed(session, NULL))
		F_SET(cache, WT_CACHE_EVICT_DIRTY | WT_CACHE_EVICT_DIRTY_HARD);
	else if (dirty_inuse > (cache->eviction_dirty_target * bytes_max) / 100)
		F_SET(cache, WT_CACHE_EVICT_DIRTY);

	/*
	 * If application threads are blocked by the total volume of data in
	 * cache, try dirty pages as well.
	 */
	if (__wt_cache_aggressive(session) &&
	    F_ISSET(cache, WT_CACHE_EVICT_CLEAN_HARD))
		F_SET(cache, WT_CACHE_EVICT_DIRTY);

	/*
	 * Scrub dirty pages and keep them in cache if we are less than half
	 * way to the clean or dirty trigger.
	 */
	if (bytes_inuse < ((cache->eviction_target + cache->eviction_trigger) *
	    bytes_max) / 200 && dirty_inuse < (uint64_t)
	    ((cache->eviction_dirty_target + cache->eviction_dirty_trigger) *
	    bytes_max) / 200)
		F_SET(cache, WT_CACHE_EVICT_SCRUB);

	/*
	 * With an in-memory cache, we only do dirty eviction in order to scrub
	 * pages.
	 */
	if (F_ISSET(conn, WT_CONN_IN_MEMORY)) {
		if (F_ISSET(cache, WT_CACHE_EVICT_CLEAN))
			F_SET(cache, WT_CACHE_EVICT_DIRTY);
		if (F_ISSET(cache, WT_CACHE_EVICT_CLEAN_HARD))
			F_SET(cache, WT_CACHE_EVICT_DIRTY_HARD);
		F_CLR(cache, WT_CACHE_EVICT_CLEAN | WT_CACHE_EVICT_CLEAN_HARD);
	}

	WT_STAT_CONN_SET(session, cache_eviction_state,
	    F_MASK(cache, WT_CACHE_EVICT_MASK));

	return (F_ISSET(cache, WT_CACHE_EVICT_ALL | WT_CACHE_EVICT_URGENT));
}

/*
 * __evict_pass --
 *	Evict pages from memory.
 */
static int
__evict_pass(WT_SESSION_IMPL *session)
{
	WT_CACHE *cache;
	WT_CONNECTION_IMPL *conn;
	WT_TXN_GLOBAL *txn_global;
	struct timespec now, prev;
	uint64_t oldest_id, pages_evicted, prev_oldest_id;
	u_int loop;

	conn = S2C(session);
	cache = conn->cache;
	txn_global = &conn->txn_global;

	/* Track whether pages are being evicted and progress is made. */
	pages_evicted = cache->pages_evict;
	prev_oldest_id = txn_global->oldest_id;
	WT_CLEAR(prev);

	/* Evict pages from the cache. */
	for (loop = 0; cache->pass_intr == 0; loop++) {
		__wt_epoch(session, &now);
		if (loop == 0)
			prev = now;

		/*
		 * Increment the shared read generation. Do this occasionally
		 * even if eviction is not currently required, so that pages
		 * have some relative read generation when the eviction server
		 * does need to do some work.
		 */
		__wt_cache_read_gen_incr(session);
		++cache->evict_pass_gen;

		/*
		 * Update the oldest ID: we use it to decide whether pages are
		 * candidates for eviction.  Without this, if all threads are
		 * blocked after a long-running transaction (such as a
		 * checkpoint) completes, we may never start evicting again.
		 *
		 * Do this every time the eviction server wakes up, regardless
		 * of whether the cache is full, to prevent the oldest ID
		 * falling too far behind.  Don't wait to lock the table: with
		 * highly threaded workloads, that creates a bottleneck.
		 */
		WT_RET(__wt_txn_update_oldest(session, WT_TXN_OLDEST_STRICT));

		if (!__evict_update_work(session))
			break;

		/*
		 * Try to start a new thread if we have capacity and haven't
		 * reached the eviction targets.
		 */
		if (F_ISSET(cache, WT_CACHE_EVICT_ALL))
			WT_RET(__wt_thread_group_start_one(
			    session, &conn->evict_threads, false));

		__wt_verbose(session, WT_VERB_EVICTSERVER,
		    "Eviction pass with: Max: %" PRIu64
		    " In use: %" PRIu64 " Dirty: %" PRIu64,
		    conn->cache_size, cache->bytes_inmem,
		    cache->bytes_dirty_intl + cache->bytes_dirty_leaf);

		if (F_ISSET(cache, WT_CACHE_EVICT_ALL))
			WT_RET(__evict_lru_walk(session));

		/*
		 * If the queue has been empty recently, keep queuing more
		 * pages to evict.  If the rate of queuing pages is high
		 * enough, this score will go to zero, in which case the
		 * eviction server might as well help out with eviction.
		 *
		 * Also, if there is a single eviction server thread with no
		 * workers, it must service the urgent queue in case all
		 * application threads are busy.
		 */
		if (cache->evict_empty_score < WT_EVICT_SCORE_CUTOFF ||
		    (!WT_EVICT_HAS_WORKERS(session) &&
		    !__evict_queue_empty(cache->evict_urgent_queue, false)))
			WT_RET(__evict_lru_pages(session, true));

		if (cache->pass_intr != 0)
			break;

		/*
		 * If we're making progress, keep going; if we're not making
		 * any progress at all, mark the cache "stuck" and go back to
		 * sleep, it's not something we can fix.
		 *
		 * We check for progress every 20ms, the idea being that the
		 * aggressive score will reach 10 after 200ms if we aren't
		 * making progress and eviction will start considering more
		 * pages.  If there is still no progress after 2s, we will
		 * treat the cache as stuck and start rolling back
		 * transactions and writing updates to the lookaside table.
		 */
		if (pages_evicted == cache->pages_evict) {
			if (WT_TIMEDIFF_MS(now, prev) >= 20 &&
			    F_ISSET(cache, WT_CACHE_EVICT_CLEAN_HARD |
			    WT_CACHE_EVICT_DIRTY_HARD)) {
				if (cache->evict_aggressive_score < 100)
					++cache->evict_aggressive_score;
				oldest_id = txn_global->oldest_id;
				if (prev_oldest_id == oldest_id &&
				    txn_global->current != oldest_id &&
				    cache->evict_aggressive_score < 100)
					++cache->evict_aggressive_score;
				WT_STAT_CONN_SET(session,
				    cache_eviction_aggressive_set,
				    cache->evict_aggressive_score);
				prev = now;
				prev_oldest_id = oldest_id;
			}

			/*
			 * Keep trying for long enough that we should be able
			 * to evict a page if the server isn't interfering.
			 */
			if (loop < 100 || cache->evict_aggressive_score < 100) {
				/*
				 * Back off if we aren't making progress: walks
				 * hold the handle list lock, blocking other
				 * operations that can free space in cache,
				 * such as LSM discarding handles.
				 *
				 * Allow this wait to be interrupted (e.g. if a
				 * checkpoint completes): make sure we wait for
				 * a non-zero number of microseconds).
				 */
				WT_STAT_CONN_INCR(session,
				    cache_eviction_server_slept);
				__wt_cond_wait(
				    session, cache->evict_cond, WT_THOUSAND);
				continue;
			}

			WT_STAT_CONN_INCR(session, cache_eviction_slow);
			__wt_verbose(session, WT_VERB_EVICTSERVER,
			    "unable to reach eviction goal");
			break;
		} else {
			if (cache->evict_aggressive_score > 0) {
				--cache->evict_aggressive_score;
				WT_STAT_CONN_SET(session,
				    cache_eviction_aggressive_set,
				    cache->evict_aggressive_score);
			}
			loop = 0;
			pages_evicted = cache->pages_evict;
		}
	}
	return (0);
}

/*
 * __evict_clear_walk --
 *	Clear a single walk point.
 */
static int
__evict_clear_walk(WT_SESSION_IMPL *session, bool count_stat)
{
	WT_BTREE *btree;
	WT_CACHE *cache;
	WT_DECL_RET;
	WT_REF *ref;

	btree = S2BT(session);
	cache = S2C(session)->cache;

	WT_ASSERT(session, F_ISSET(session, WT_SESSION_LOCKED_PASS));
	if (session->dhandle == cache->evict_file_next)
		cache->evict_file_next = NULL;

	if ((ref = btree->evict_ref) == NULL)
		return (0);

	if (count_stat)
		WT_STAT_CONN_INCR(session, cache_eviction_walks_abandoned);

	/*
	 * Clear evict_ref first, in case releasing it forces eviction (we
	 * assert we never try to evict the current eviction walk point).
	 */
	btree->evict_ref = NULL;
	WT_WITH_DHANDLE(cache->walk_session, session->dhandle,
	    (ret = __wt_page_release(cache->walk_session,
	    ref, WT_READ_NO_EVICT)));
	return (ret);
}

/*
 * __evict_clear_all_walks --
 *	Clear the eviction walk points for all files a session is waiting on.
 */
static int
__evict_clear_all_walks(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;
	WT_DATA_HANDLE *dhandle;
	WT_DECL_RET;

	conn = S2C(session);

	TAILQ_FOREACH(dhandle, &conn->dhqh, q)
		if (WT_PREFIX_MATCH(dhandle->name, "file:"))
			WT_WITH_DHANDLE(session, dhandle,
			    WT_TRET(__evict_clear_walk(session, true)));
	return (ret);
}

/*
 * __wt_evict_file_exclusive_on --
 *	Get exclusive eviction access to a file and discard any of the file's
 *	blocks queued for eviction.
 */
int
__wt_evict_file_exclusive_on(WT_SESSION_IMPL *session)
{
	WT_BTREE *btree;
	WT_CACHE *cache;
	WT_DECL_RET;
	WT_EVICT_ENTRY *evict;
	u_int i, elem, q;

	btree = S2BT(session);
	cache = S2C(session)->cache;

	/*
	 * Hold the walk lock to set the no-eviction flag.
	 *
	 * The no-eviction flag can be set permanently, in which case we never
	 * increment the no-eviction count.
	 */
	__wt_spin_lock(session, &cache->evict_walk_lock);
	if (F_ISSET(btree, WT_BTREE_NO_EVICTION)) {
		if (btree->evict_disabled != 0)
			++btree->evict_disabled;
		__wt_spin_unlock(session, &cache->evict_walk_lock);
		return (0);
	}
	++btree->evict_disabled;

	/*
	 * Ensure no new pages from the file will be queued for eviction after
	 * this point.
	 */
	F_SET(btree, WT_BTREE_NO_EVICTION);
	(void)__wt_atomic_addv32(&cache->pass_intr, 1);

	/* Clear any existing LRU eviction walk for the file. */
	WT_WITH_PASS_LOCK(session, ret,
	    ret = __evict_clear_walk(session, true));
	(void)__wt_atomic_subv32(&cache->pass_intr, 1);
	WT_ERR(ret);

	/*
	 * The eviction candidate list might reference pages from the file,
	 * clear it. Hold the evict lock to remove queued pages from a file.
	 */
	__wt_spin_lock(session, &cache->evict_queue_lock);

	for (q = 0; q < WT_EVICT_QUEUE_MAX; q++) {
		__wt_spin_lock(session, &cache->evict_queues[q].evict_lock);
		elem = cache->evict_queues[q].evict_max;
		for (i = 0, evict = cache->evict_queues[q].evict_queue;
		    i < elem; i++, evict++)
			if (evict->btree == btree)
				__evict_list_clear(session, evict);
		__wt_spin_unlock(session, &cache->evict_queues[q].evict_lock);
	}

	__wt_spin_unlock(session, &cache->evict_queue_lock);

	/*
	 * We have disabled further eviction: wait for concurrent LRU eviction
	 * activity to drain.
	 */
	while (btree->evict_busy > 0)
		__wt_yield();

	if (0) {
err:		--btree->evict_disabled;
		F_CLR(btree, WT_BTREE_NO_EVICTION);
	}
	__wt_spin_unlock(session, &cache->evict_walk_lock);
	return (ret);
}

/*
 * __wt_evict_file_exclusive_off --
 *	Release exclusive eviction access to a file.
 */
void
__wt_evict_file_exclusive_off(WT_SESSION_IMPL *session)
{
	WT_BTREE *btree;
	WT_CACHE *cache;

	btree = S2BT(session);
	cache = S2C(session)->cache;

	/*
	 * We have seen subtle bugs with multiple threads racing to turn
	 * eviction on/off.  Make races more likely in diagnostic builds.
	 */
	WT_DIAGNOSTIC_YIELD;

	WT_ASSERT(session,
	    btree->evict_ref == NULL && F_ISSET(btree, WT_BTREE_NO_EVICTION));

	/*
	 * The no-eviction flag can be set permanently, in which case we never
	 * increment the no-eviction count.
	 */
	__wt_spin_lock(session, &cache->evict_walk_lock);
	if (btree->evict_disabled > 0 && --btree->evict_disabled == 0)
		F_CLR(btree, WT_BTREE_NO_EVICTION);
	__wt_spin_unlock(session, &cache->evict_walk_lock);
}

/*
 * __evict_lru_pages --
 *	Get pages from the LRU queue to evict.
 */
static int
__evict_lru_pages(WT_SESSION_IMPL *session, bool is_server)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;

	conn = S2C(session);

	/*
	 * Reconcile and discard some pages: EBUSY is returned if a page fails
	 * eviction because it's unavailable, continue in that case.
	 */
	while (F_ISSET(S2C(session), WT_CONN_EVICTION_RUN) && ret == 0)
		if ((ret = __evict_page(session, is_server)) == EBUSY)
			ret = 0;

	/* If a worker thread found the queue empty, pause. */
	if (ret == WT_NOTFOUND && !is_server &&
	    F_ISSET(S2C(session), WT_CONN_EVICTION_RUN))
		__wt_cond_wait(session, conn->evict_threads.wait_cond, 10000);

	return (ret == WT_NOTFOUND ? 0 : ret);
}

/*
 * __evict_lru_walk --
 *	Add pages to the LRU queue to be evicted from cache.
 */
static int
__evict_lru_walk(WT_SESSION_IMPL *session)
{
	WT_CACHE *cache;
	WT_DECL_RET;
	WT_EVICT_QUEUE *queue, *other_queue;
	uint64_t read_gen_oldest;
	uint32_t candidates, entries;

	cache = S2C(session)->cache;

	/* Age out the score of how much the queue has been empty recently. */
	if (cache->evict_empty_score > 0) {
		--cache->evict_empty_score;
		WT_STAT_CONN_SET(session, cache_eviction_empty_score,
		    cache->evict_empty_score);
	}

	/* Fill the next queue (that isn't the urgent queue). */
	queue = cache->evict_fill_queue;
	other_queue = cache->evict_queues + (1 - (queue - cache->evict_queues));
	cache->evict_fill_queue = other_queue;

	/* If this queue is full, try the other one. */
	if (__evict_queue_full(queue) && !__evict_queue_full(other_queue))
		queue = other_queue;

	/*
	 * If both queues are full and haven't been empty on recent refills,
	 * we're done.
	 */
	if (__evict_queue_full(queue) &&
	    cache->evict_empty_score < WT_EVICT_SCORE_CUTOFF)
		return (0);

	/* Get some more pages to consider for eviction. */
	if ((ret = __evict_walk(cache->walk_session, queue)) == EBUSY)
		return (0);	/* An interrupt was requested, give up. */
	WT_RET_NOTFOUND_OK(ret);

	/*
	 * If the queue we are filling is empty, pages are being requested
	 * faster than they are being queued.
	 */
	if (__evict_queue_empty(queue, false)) {
		if (F_ISSET(cache,
		    WT_CACHE_EVICT_CLEAN_HARD | WT_CACHE_EVICT_DIRTY_HARD)) {
			cache->evict_empty_score = WT_MIN(
			    cache->evict_empty_score + WT_EVICT_SCORE_BUMP,
			    WT_EVICT_SCORE_MAX);
			WT_STAT_CONN_SET(session,
			    cache_eviction_empty_score,
			    cache->evict_empty_score);
		}
		WT_STAT_CONN_INCR(session, cache_eviction_queue_empty);
	} else
		WT_STAT_CONN_INCR(session, cache_eviction_queue_not_empty);

	/* Sort the list into LRU order and restart. */
	__wt_spin_lock(session, &queue->evict_lock);

	/*
	 * We have locked the queue: in the (unusual) case where we are filling
	 * the current queue, mark it empty so that subsequent requests switch
	 * to the other queue.
	 */
	if (queue == cache->evict_current_queue)
		queue->evict_current = NULL;

	entries = queue->evict_entries;
	qsort(queue->evict_queue,
	    entries, sizeof(WT_EVICT_ENTRY), __evict_lru_cmp);

	/* Trim empty entries from the end. */
	while (entries > 0 && queue->evict_queue[entries - 1].ref == NULL)
		--entries;

	/*
	 * If we have more entries than the maximum tracked between walks,
	 * clear them.  Do this before figuring out how many of the entries are
	 * candidates so we never end up with more candidates than entries.
	 */
	while (entries > WT_EVICT_WALK_BASE)
		__evict_list_clear(session, &queue->evict_queue[--entries]);

	queue->evict_entries = entries;

	if (entries == 0) {
		/*
		 * If there are no entries, there cannot be any candidates.
		 * Make sure application threads don't read past the end of the
		 * candidate list, or they may race with the next walk.
		 */
		queue->evict_candidates = 0;
		queue->evict_current = NULL;
		__wt_spin_unlock(session, &queue->evict_lock);
		return (0);
	}

	/* Decide how many of the candidates we're going to try and evict. */
	if (__wt_cache_aggressive(session))
		queue->evict_candidates = entries;
	else {
		/*
		 * Find the oldest read generation apart that we have in the
		 * queue, used to set the initial value for pages read into the
		 * system.  The queue is sorted, find the first "normal"
		 * generation.
		 */
		read_gen_oldest = WT_READGEN_OLDEST;
		for (candidates = 0; candidates < entries; ++candidates) {
			read_gen_oldest = queue->evict_queue[candidates].score;
			if (read_gen_oldest != WT_READGEN_OLDEST)
				break;
		}

		/*
		 * Take all candidates if we only gathered pages with an oldest
		 * read generation set.
		 *
		 * We normally never take more than 50% of the entries but if
		 * 50% of the entries were at the oldest read generation, take
		 * all of them.
		 */
		if (read_gen_oldest == WT_READGEN_OLDEST)
			queue->evict_candidates = entries;
		else if (candidates > entries / 2)
			queue->evict_candidates = candidates;
		else {
			/*
			 * Take all of the urgent pages plus a third of
			 * ordinary candidates (which could be expressed as
			 * WT_EVICT_WALK_INCR / WT_EVICT_WALK_BASE).  In the
			 * steady state, we want to get as many candidates as
			 * the eviction walk adds to the queue.
			 *
			 * That said, if there is only one entry, which is
			 * normal when populating an empty file, don't exclude
			 * it.
			 */
			queue->evict_candidates =
			    1 + candidates + ((entries - candidates) - 1) / 3;
			cache->read_gen_oldest = read_gen_oldest;
		}
	}

	queue->evict_current = queue->evict_queue;
	__wt_spin_unlock(session, &queue->evict_lock);

	/*
	 * Signal any application or helper threads that may be waiting
	 * to help with eviction.
	 */
	__wt_cond_signal(session, S2C(session)->evict_threads.wait_cond);

	return (0);
}

/*
 * __evict_walk --
 *	Fill in the array by walking the next set of pages.
 */
static int
__evict_walk(WT_SESSION_IMPL *session, WT_EVICT_QUEUE *queue)
{
	WT_BTREE *btree;
	WT_CACHE *cache;
	WT_CONNECTION_IMPL *conn;
	WT_DATA_HANDLE *dhandle;
	WT_DECL_RET;
	u_int max_entries, retries, slot, spins, start_slot, total_candidates;
	bool dhandle_locked, incr;

	conn = S2C(session);
	cache = S2C(session)->cache;
	btree = NULL;
	dhandle = NULL;
	dhandle_locked = incr = false;
	retries = 0;

	/*
	 * Set the starting slot in the queue and the maximum pages added
	 * per walk.
	 */
	start_slot = slot = queue->evict_entries;
	max_entries = WT_MIN(slot + WT_EVICT_WALK_INCR, cache->evict_slots);

	/*
	 * Another pathological case: if there are only a tiny number of
	 * candidate pages in cache, don't put all of them on one queue.
	 */
	total_candidates = (u_int)(F_ISSET(cache, WT_CACHE_EVICT_CLEAN) ?
	    __wt_cache_pages_inuse(cache) : cache->pages_dirty_leaf);
	max_entries = WT_MIN(max_entries, 1 + total_candidates / 2);

retry:	while (slot < max_entries) {
		/*
		 * If another thread is waiting on the eviction server to clear
		 * the walk point in a tree, give up.
		 */
		if (cache->pass_intr != 0)
			WT_ERR(EBUSY);

		/*
		 * Lock the dhandle list to find the next handle and bump its
		 * reference count to keep it alive while we sweep.
		 */
		if (!dhandle_locked) {
			for (spins = 0; (ret = __wt_spin_trylock(
			    session, &conn->dhandle_lock)) == EBUSY &&
			    cache->pass_intr == 0;
			    spins++) {
				if (spins < WT_THOUSAND)
					__wt_yield();
				else
					__wt_sleep(0, WT_THOUSAND);
			}
			WT_ERR(ret);
			dhandle_locked = true;
		}

		if (dhandle == NULL) {
			/*
			 * On entry, continue from wherever we got to in the
			 * scan last time through.  If we don't have a saved
			 * handle, start from the beginning of the list.
			 */
			if ((dhandle = cache->evict_file_next) != NULL)
				cache->evict_file_next = NULL;
			else
				dhandle = TAILQ_FIRST(&conn->dhqh);
		} else {
			if (incr) {
				WT_ASSERT(session, dhandle->session_inuse > 0);
				(void)__wt_atomic_subi32(
				    &dhandle->session_inuse, 1);
				incr = false;
				cache->evict_file_next = NULL;
			}
			dhandle = TAILQ_NEXT(dhandle, q);
		}

		/* If we reach the end of the list, we're done. */
		if (dhandle == NULL)
			break;

		/* Ignore non-file handles, or handles that aren't open. */
		if (!WT_PREFIX_MATCH(dhandle->name, "file:") ||
		    !F_ISSET(dhandle, WT_DHANDLE_OPEN))
			continue;

		/* Skip files that don't allow eviction. */
		btree = dhandle->handle;
		if (F_ISSET(btree, WT_BTREE_NO_EVICTION))
			continue;

		/*
		 * Skip files that are checkpointing if we are only looking for
		 * dirty pages.
		 */
		if (btree->checkpointing != WT_CKPT_OFF &&
		    !F_ISSET(cache, WT_CACHE_EVICT_CLEAN))
			continue;

		/*
		 * Skip files that are configured to stick in cache until we
		 * become aggressive.
		 */
		if (btree->evict_priority != 0 &&
		    !__wt_cache_aggressive(session))
			continue;

		/* Skip files if we have used all available hazard pointers. */
		if (btree->evict_ref == NULL && session->nhazard >=
		    conn->hazard_max - WT_MIN(conn->hazard_max / 2, 10))
			continue;

		/*
		 * If we are filling the queue, skip files that haven't been
		 * useful in the past.
		 */
		if (btree->evict_walk_period != 0 &&
		    btree->evict_walk_skips++ < btree->evict_walk_period)
			continue;
		btree->evict_walk_skips = 0;

		(void)__wt_atomic_addi32(&dhandle->session_inuse, 1);
		incr = true;
		__wt_spin_unlock(session, &conn->dhandle_lock);
		dhandle_locked = false;

		/*
		 * Re-check the "no eviction" flag, used to enforce exclusive
		 * access when a handle is being closed. If not set, remember
		 * the file to visit first, next loop.
		 *
		 * Only try to acquire the lock and simply continue if we fail;
		 * the lock is held while the thread turning off eviction clears
		 * the tree's current eviction point, and part of the process is
		 * waiting on this thread to acknowledge that action.
		 */
		if (!F_ISSET(btree, WT_BTREE_NO_EVICTION) &&
		    !__wt_spin_trylock(session, &cache->evict_walk_lock)) {
			if (!F_ISSET(btree, WT_BTREE_NO_EVICTION)) {
				cache->evict_file_next = dhandle;
				WT_WITH_DHANDLE(session, dhandle, ret =
				    __evict_walk_file(session, queue,
				    max_entries, &slot));
				WT_ASSERT(session, session->split_gen == 0);
			}
			__wt_spin_unlock(session, &cache->evict_walk_lock);
			WT_ERR(ret);
		}
	}

	if (incr) {
		WT_ASSERT(session, dhandle->session_inuse > 0);
		(void)__wt_atomic_subi32(&dhandle->session_inuse, 1);
		incr = false;
	}

	/*
	 * Walk the list of files a few times if we don't find enough pages.
	 * Try two passes through all the files, give up when we have some
	 * candidates and we aren't finding more.
	 */
	if (slot < max_entries && (retries < 2 ||
	    (retries < 10 &&
	    (slot == queue->evict_entries || slot > start_slot)))) {
		start_slot = slot;
		++retries;
		goto retry;
	}

err:	if (dhandle_locked) {
		__wt_spin_unlock(session, &conn->dhandle_lock);
		dhandle_locked = false;
	}

	/*
	 * If we didn't find any entries on a walk when we weren't interrupted,
	 * let our caller know.
	 */
	if (queue->evict_entries == slot && cache->pass_intr == 0)
		return (WT_NOTFOUND);

	queue->evict_entries = slot;
	return (ret);
}

/*
 * __evict_push_candidate --
 *	Initialize a WT_EVICT_ENTRY structure with a given page.
 */
static bool
__evict_push_candidate(WT_SESSION_IMPL *session,
    WT_EVICT_QUEUE *queue, WT_EVICT_ENTRY *evict, WT_REF *ref)
{
	u_int slot;
	uint8_t orig_flags, new_flags;

	/*
	 * Threads can race to queue a page (e.g., an ordinary LRU walk can
	 * race with a page being queued for urgent eviction).
	 */
	orig_flags = new_flags = ref->page->flags_atomic;
	FLD_SET(new_flags, WT_PAGE_EVICT_LRU);
	if (orig_flags == new_flags ||
	    !__wt_atomic_cas8(&ref->page->flags_atomic, orig_flags, new_flags))
		return (false);

	/* Keep track of the maximum slot we are using. */
	slot = (u_int)(evict - queue->evict_queue);
	if (slot >= queue->evict_max)
		queue->evict_max = slot + 1;

	if (evict->ref != NULL)
		__evict_list_clear(session, evict);

	evict->btree = S2BT(session);
	evict->ref = ref;
	evict->score = __evict_entry_priority(session, ref);

	/* Adjust for size when doing dirty eviction. */
	if (F_ISSET(S2C(session)->cache, WT_CACHE_EVICT_DIRTY) &&
	    evict->score != WT_READGEN_OLDEST && evict->score != UINT64_MAX &&
	    !__wt_page_is_modified(ref->page))
		evict->score += WT_MEGABYTE -
		    WT_MIN(WT_MEGABYTE, ref->page->memory_footprint);

	return (true);
}

/*
 * __evict_walk_file --
 *	Get a few page eviction candidates from a single underlying file.
 */
static int
__evict_walk_file(WT_SESSION_IMPL *session, WT_EVICT_QUEUE *queue,
    u_int max_entries, u_int *slotp)
{
	WT_BTREE *btree;
	WT_CACHE *cache;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_EVICT_ENTRY *end, *evict, *start;
	WT_PAGE *page;
	WT_PAGE_MODIFY *mod;
	WT_REF *ref;
	WT_TXN_GLOBAL *txn_global;
	uint64_t btree_inuse, bytes_per_slot, cache_inuse, min_pages;
	uint64_t pages_seen, pages_queued, refs_walked;
	uint32_t remaining_slots, total_slots, walk_flags;
	uint32_t target_pages_clean, target_pages_dirty, target_pages;
	int internal_pages, restarts;
	bool give_up, modified, urgent_queued;

	conn = S2C(session);
	btree = S2BT(session);
	cache = conn->cache;
	txn_global = &conn->txn_global;
	internal_pages = restarts = 0;
	give_up = urgent_queued = false;

	/*
	 * Figure out how many slots to fill from this tree.
	 * Note that some care is taken in the calculation to avoid overflow.
	 */
	start = queue->evict_queue + *slotp;
	remaining_slots = max_entries - *slotp;
	total_slots = max_entries - queue->evict_entries;

	/*
	 * The target number of pages for this tree is proportional to the
	 * space it is taking up in cache.  Round to the nearest number of
	 * slots so we assign all of the slots to a tree filling 99+% of the
	 * cache (and only have to walk it once).
	 */
	if (F_ISSET(cache, WT_CACHE_EVICT_CLEAN)) {
		btree_inuse = __wt_btree_bytes_inuse(session);
		cache_inuse = __wt_cache_bytes_inuse(cache);
		bytes_per_slot = 1 + cache_inuse / total_slots;
		target_pages_clean = (uint32_t)(
		    (btree_inuse + bytes_per_slot / 2) / bytes_per_slot);
	} else
		target_pages_clean = 0;

	if (F_ISSET(cache, WT_CACHE_EVICT_DIRTY)) {
		btree_inuse = __wt_btree_dirty_leaf_inuse(session);
		cache_inuse = __wt_cache_dirty_leaf_inuse(cache);
		bytes_per_slot = 1 + cache_inuse / total_slots;
		target_pages_dirty = (uint32_t)(
		    (btree_inuse + bytes_per_slot / 2) / bytes_per_slot);
	} else
		target_pages_dirty = 0;

	target_pages = WT_MAX(target_pages_clean, target_pages_dirty);

	if (target_pages == 0) {
		/*
		 * Randomly walk trees with a tiny fraction of the cache in
		 * case there are so many trees that none of them use enough of
		 * the cache to be allocated slots.  Walk small trees 1% of the
		 * time.
		 */
		if (__wt_random(&session->rnd) > UINT32_MAX / 100)
			return (0);
		target_pages = 10;
	}

	if (F_ISSET(session->dhandle, WT_DHANDLE_DEAD) ||
	    target_pages > remaining_slots)
		target_pages = remaining_slots;
	end = start + target_pages;

	walk_flags =
	    WT_READ_CACHE | WT_READ_NO_EVICT | WT_READ_NO_GEN | WT_READ_NO_WAIT;

	/* Randomize the walk direction. */
	if (btree->evict_walk_reverse)
		FLD_SET(walk_flags, WT_READ_PREV);

	/*
	 * Examine at least a reasonable number of pages before deciding
	 * whether to give up.  When we are only looking for dirty pages,
	 * search the tree for longer.
	 */
	min_pages = 10 * target_pages;
	if (F_ISSET(cache, WT_CACHE_EVICT_DIRTY) &&
	    !F_ISSET(cache, WT_CACHE_EVICT_CLEAN))
		min_pages *= 10;

	/*
	 * Get some more eviction candidate pages.
	 *
	 * !!! Take care terminating this loop.
	 *
	 * Don't make an extra call to __wt_tree_walk after we hit the end of a
	 * tree: that will leave a page pinned, which may prevent any work from
	 * being done.
	 *
	 * Once we hit the page limit, do one more step through the walk in
	 * case we are appending and only the last page in the file is live.
	 */
	for (evict = start, pages_queued = pages_seen = refs_walked = 0;
	    evict < end && (ret == 0 || ret == WT_NOTFOUND);
	    ret = __wt_tree_walk_count(
	    session, &btree->evict_ref, &refs_walked, walk_flags)) {
		/*
		 * Check whether we're finding a good ratio of candidates vs
		 * pages seen.  Some workloads create "deserts" in trees where
		 * no good eviction candidates can be found.  Abandon the walk
		 * if we get into that situation.
		 */
		give_up = !__wt_cache_aggressive(session) &&
		    pages_seen > min_pages &&
		    (pages_queued == 0 || (pages_seen / pages_queued) >
		    (min_pages / target_pages));
		if (give_up)
			break;

		if ((ref = btree->evict_ref) == NULL) {
			if (++restarts == 2)
				break;
			WT_STAT_CONN_INCR(
			    session, cache_eviction_walks_started);
			continue;
		}

		++pages_seen;

		/* Ignore root pages entirely. */
		if (__wt_ref_is_root(ref))
			continue;

		page = ref->page;
		modified = __wt_page_is_modified(page);
		page->evict_pass_gen = cache->evict_pass_gen;

		/*
		 * Use the EVICT_LRU flag to avoid putting pages onto the list
		 * multiple times.
		 */
		if (F_ISSET_ATOMIC(page, WT_PAGE_EVICT_LRU))
			continue;

		/*
		 * It's possible (but unlikely) to visit a page without a read
		 * generation, if we race with the read instantiating the page.
		 * Set the page's read generation here to ensure a bug doesn't
		 * somehow leave a page without a read generation.
		 */
		if (page->read_gen == WT_READGEN_NOTSET)
			__wt_cache_read_gen_new(session, page);

		/* Pages we no longer need (clean or dirty), are found money. */
		if (page->read_gen == WT_READGEN_OLDEST ||
		    page->memory_footprint >= btree->splitmempage) {
			WT_STAT_CONN_INCR(
			    session, cache_eviction_pages_queued_oldest);
			if (__wt_page_evict_urgent(session, ref))
				urgent_queued = true;
			continue;
		}

		/* Pages that are empty or from dead trees are also good. */
		if (__wt_page_is_empty(page) ||
		    F_ISSET(session->dhandle, WT_DHANDLE_DEAD))
			goto fast;

		/* Skip clean pages if appropriate. */
		if (!modified && !F_ISSET(cache, WT_CACHE_EVICT_CLEAN))
			continue;

		/* Skip dirty pages if appropriate. */
		if (modified && !F_ISSET(cache, WT_CACHE_EVICT_DIRTY))
			continue;

		/* Limit internal pages to 50% of the total. */
		if (WT_PAGE_IS_INTERNAL(page) &&
		    internal_pages >= (int)(evict - start) / 2)
			continue;

		/* If eviction gets aggressive, anything else is fair game. */
		if (__wt_cache_aggressive(session))
			goto fast;

		/*
		 * If the oldest transaction hasn't changed since the last time
		 * this page was written, it's unlikely we can make progress.
		 * Similarly, if the most recent update on the page is not yet
		 * globally visible, eviction will fail.  These heuristics
		 * attempt to avoid repeated attempts to evict the same page.
		 */
		mod = page->modify;
		if (modified && txn_global->current != txn_global->oldest_id &&
		    (mod->last_eviction_id == __wt_txn_oldest_id(session) ||
		    !__wt_txn_visible_all(session, mod->update_txn)))
			continue;

fast:		/* If the page can't be evicted, give up. */
		if (!__wt_page_can_evict(session, ref, NULL))
			continue;

		WT_ASSERT(session, evict->ref == NULL);
		if (!__evict_push_candidate(session, queue, evict, ref))
			continue;
		++evict;
		++pages_queued;

		if (WT_PAGE_IS_INTERNAL(page))
		    ++internal_pages;

		__wt_verbose(session, WT_VERB_EVICTSERVER,
		    "select: %p, size %" WT_SIZET_FMT,
		    (void *)page, page->memory_footprint);
	}
	WT_RET_NOTFOUND_OK(ret);

	*slotp += (u_int)(evict - start);
	WT_STAT_CONN_INCRV(
	    session, cache_eviction_pages_queued, (u_int)(evict - start));

	/*
	 * If we didn't find any candidates in the file, reverse the direction
	 * of the walk and skip it next time.
	 */
	if (give_up)
		btree->evict_walk_reverse = !btree->evict_walk_reverse;
	if (give_up && !urgent_queued)
		btree->evict_walk_period = WT_MIN(
		    WT_MAX(1, 2 * btree->evict_walk_period), 100);
	else if (pages_queued == target_pages)
		btree->evict_walk_period = 0;

	/*
	 * If we happen to end up on the root page or a page requiring urgent
	 * eviction, clear it.  We have to track hazard pointers, and the root
	 * page complicates that calculation.
	 *
	 * Likewise if we found no new candidates during the walk: there is no
	 * point keeping a page pinned, since it may be the only candidate in
	 * an idle tree.
	 *
	 * If we land on a page requiring forced eviction, move on to the next
	 * page: we want this page evicted as quickly as possible.
	 */
	if ((ref = btree->evict_ref) != NULL) {
		/* Give up the walk occasionally. */
		if (__wt_ref_is_root(ref) || evict == start || give_up ||
		    ref->page->read_gen == WT_READGEN_OLDEST ||
		    ref->page->memory_footprint >= btree->splitmempage)
			WT_RET(__evict_clear_walk(session, restarts == 0));
		else if (ref->page->read_gen == WT_READGEN_OLDEST)
			WT_RET_NOTFOUND_OK(__wt_tree_walk_count(
			    session, &btree->evict_ref,
			    &refs_walked, walk_flags));
	}

	WT_STAT_CONN_INCRV(session, cache_eviction_walk, refs_walked);
	WT_STAT_CONN_INCRV(session, cache_eviction_pages_seen, pages_seen);

	return (0);
}

/*
 * __evict_get_ref --
 *	Get a page for eviction.
 */
static int
__evict_get_ref(
    WT_SESSION_IMPL *session, bool is_server, WT_BTREE **btreep, WT_REF **refp)
{
	WT_CACHE *cache;
	WT_EVICT_ENTRY *evict;
	WT_EVICT_QUEUE *queue, *other_queue, *urgent_queue;
	uint32_t candidates;
	bool is_app, server_only, urgent_ok;

	cache = S2C(session)->cache;
	is_app = !F_ISSET(session, WT_SESSION_INTERNAL);
	server_only = is_server && !WT_EVICT_HAS_WORKERS(session);
	urgent_ok = (!is_app && !is_server) ||
	    !WT_EVICT_HAS_WORKERS(session) ||
	    (is_app && __wt_cache_aggressive(session));
	urgent_queue = cache->evict_urgent_queue;
	*btreep = NULL;
	*refp = NULL;

	WT_STAT_CONN_INCR(session, cache_eviction_get_ref);

	/* Avoid the LRU lock if no pages are available. */
	if (__evict_queue_empty(cache->evict_current_queue, is_server) &&
	    __evict_queue_empty(cache->evict_other_queue, is_server) &&
	    (!urgent_ok || __evict_queue_empty(urgent_queue, false))) {
		WT_STAT_CONN_INCR(session, cache_eviction_get_ref_empty);
		return (WT_NOTFOUND);
	}

	/*
	 * The server repopulates whenever the other queue is not full, as long
	 * as at least one page has been evicted out of the current queue.
	 *
	 * Note that there are pathological cases where there are only enough
	 * eviction candidates in the cache to fill one queue.  In that case,
	 * we will continually evict one page and attempt to refill the queues.
	 * Such cases are extremely rare in real applications.
	 */
	if (is_server &&
	    (!urgent_ok || __evict_queue_empty(urgent_queue, false)) &&
	    !__evict_queue_full(cache->evict_current_queue) &&
	    !__evict_queue_full(cache->evict_fill_queue) &&
	    (cache->evict_empty_score > WT_EVICT_SCORE_CUTOFF ||
	    __evict_queue_empty(cache->evict_fill_queue, false)))
		return (WT_NOTFOUND);

	__wt_spin_lock(session, &cache->evict_queue_lock);

	/* Check the urgent queue first. */
	if (urgent_ok && !__evict_queue_empty(urgent_queue, false))
		queue = urgent_queue;
	else {
		/*
		 * Check if the current queue needs to change.
		 *
		 * The server will only evict half of the pages before looking
		 * for more, but should only switch queues if there are no
		 * other eviction workers.
		 */
		queue = cache->evict_current_queue;
		other_queue = cache->evict_other_queue;
		if (__evict_queue_empty(queue, server_only) &&
		    !__evict_queue_empty(other_queue, server_only)) {
			cache->evict_current_queue = other_queue;
			cache->evict_other_queue = queue;
		}
	}

	__wt_spin_unlock(session, &cache->evict_queue_lock);

	/*
	 * We got the queue lock, which should be fast, and chose a queue.
	 * Now we want to get the lock on the individual queue.
	 */
	for (;;) {
		/* Verify there are still pages available. */
		if (__evict_queue_empty(
		    queue, is_server && queue != urgent_queue)) {
			WT_STAT_CONN_INCR(
			    session, cache_eviction_get_ref_empty2);
			return (WT_NOTFOUND);
		}
		if (!is_server)
			__wt_spin_lock(session, &queue->evict_lock);
		else if (__wt_spin_trylock(session, &queue->evict_lock) != 0)
			continue;
		break;
	}

	/*
	 * Only evict half of the pages before looking for more. The remainder
	 * are left to eviction workers (if configured), or application thread
	 * if necessary.
	 */
	candidates = queue->evict_candidates;
	if (is_server && queue != urgent_queue && candidates > 1)
		candidates /= 2;

	/* Get the next page queued for eviction. */
	for (evict = queue->evict_current;
	    evict >= queue->evict_queue &&
	    evict < queue->evict_queue + candidates;
	    ++evict) {
		if (evict->ref == NULL)
			continue;
		WT_ASSERT(session, evict->btree != NULL);

		/*
		 * Evicting a dirty page in the server thread could stall
		 * during a write and prevent eviction from finding new work.
		 *
		 * However, we can't skip entries in the urgent queue or they
		 * may never be found again.
		 *
		 * Don't force application threads to evict dirty pages if they
		 * aren't stalled by the amount of dirty data in cache.
		 */
		if (!urgent_ok && (is_server ||
		    !F_ISSET(cache, WT_CACHE_EVICT_DIRTY_HARD)) &&
		    __wt_page_is_modified(evict->ref->page)) {
			--evict;
			break;
		}

		/*
		 * Lock the page while holding the eviction mutex to prevent
		 * multiple attempts to evict it.  For pages that are already
		 * being evicted, this operation will fail and we will move on.
		 */
		if (!__wt_atomic_casv32(
		    &evict->ref->state, WT_REF_MEM, WT_REF_LOCKED)) {
			__evict_list_clear(session, evict);
			continue;
		}

		/*
		 * Increment the busy count in the btree handle to prevent it
		 * from being closed under us.
		 */
		(void)__wt_atomic_addv32(&evict->btree->evict_busy, 1);

		*btreep = evict->btree;
		*refp = evict->ref;

		/*
		 * Remove the entry so we never try to reconcile the same page
		 * on reconciliation error.
		 */
		__evict_list_clear(session, evict);
		break;
	}

	/* Move to the next item. */
	if (evict != NULL &&
	    evict + 1 < queue->evict_queue + queue->evict_candidates)
		queue->evict_current = evict + 1;
	else /* Clear the current pointer if there are no more candidates. */
		queue->evict_current = NULL;

	__wt_spin_unlock(session, &queue->evict_lock);

	return (*refp == NULL ? WT_NOTFOUND : 0);
}

/*
 * __evict_page --
 *	Called by both eviction and application threads to evict a page.
 */
static int
__evict_page(WT_SESSION_IMPL *session, bool is_server)
{
	struct timespec enter, leave;
	WT_BTREE *btree;
	WT_CACHE *cache;
	WT_DECL_RET;
	WT_REF *ref;
	bool app_timer;

	WT_RET(__evict_get_ref(session, is_server, &btree, &ref));
	WT_ASSERT(session, ref->state == WT_REF_LOCKED);

	app_timer = false;
	cache = S2C(session)->cache;

	/*
	 * An internal session flags either the server itself or an eviction
	 * worker thread.
	 */
	if (is_server) {
		WT_STAT_CONN_INCR(session, cache_eviction_server_evicting);
		cache->server_evicts++;
	} else if (F_ISSET(session, WT_SESSION_INTERNAL)) {
		WT_STAT_CONN_INCR(session, cache_eviction_worker_evicting);
		cache->worker_evicts++;
	} else {
		if (__wt_page_is_modified(ref->page))
			WT_STAT_CONN_INCR(session, cache_eviction_app_dirty);
		WT_STAT_CONN_INCR(session, cache_eviction_app);
		cache->app_evicts++;
		if (WT_STAT_ENABLED(session)) {
			app_timer = true;
			__wt_epoch(session, &enter);
		}
	}

	/*
	 * In case something goes wrong, don't pick the same set of pages every
	 * time.
	 *
	 * We used to bump the page's read generation only if eviction failed,
	 * but that isn't safe: at that point, eviction has already unlocked
	 * the page and some other thread may have evicted it by the time we
	 * look at it.
	 */
	__wt_cache_read_gen_bump(session, ref->page);

	WT_WITH_BTREE(session, btree, ret = __wt_evict(session, ref, false));

	(void)__wt_atomic_subv32(&btree->evict_busy, 1);

	if (app_timer) {
		__wt_epoch(session, &leave);
		WT_STAT_CONN_INCRV(session,
		    application_evict_time, WT_TIMEDIFF_US(leave, enter));
	}
	return (ret);
}

/*
 * __wt_cache_eviction_worker --
 *	Worker function for __wt_cache_eviction_check: evict pages if the cache
 * crosses its boundaries.
 */
int
__wt_cache_eviction_worker(WT_SESSION_IMPL *session, bool busy, u_int pct_full)
{
	struct timespec enter, leave;
	WT_CACHE *cache;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_TXN_GLOBAL *txn_global;
	WT_TXN_STATE *txn_state;
	uint64_t init_evict_count, max_pages_evicted;

	conn = S2C(session);
	cache = conn->cache;
	txn_global = &conn->txn_global;
	txn_state = WT_SESSION_TXN_STATE(session);

	/*
	 * It is not safe to proceed if the eviction server threads aren't
	 * setup yet.
	 */
	if (!conn->evict_server_running)
		return (0);

	if (busy && pct_full < 100)
		return (0);

	/* Wake the eviction server if we need to do work. */
	__wt_evict_server_wake(session);

	/* Track how long application threads spend doing eviction. */
	if (WT_STAT_ENABLED(session) && !F_ISSET(session, WT_SESSION_INTERNAL))
		__wt_epoch(session, &enter);

	for (init_evict_count = cache->pages_evict;; ret = 0) {
		/*
		 * A pathological case: if we're the oldest transaction in the
		 * system and the eviction server is stuck trying to find space,
		 * abort the transaction to give up all hazard pointers before
		 * trying again.
		 */
		if (__wt_cache_stuck(session) && __wt_txn_am_oldest(session)) {
			--cache->evict_aggressive_score;
			WT_STAT_CONN_INCR(session, txn_fail_cache);
			WT_ERR(WT_ROLLBACK);
		}

		/*
		 * Check if we have become busy.
		 *
		 * If we're busy (because of the transaction check we just did
		 * or because our caller is waiting on a longer-than-usual event
		 * such as a page read), and the cache level drops below 100%,
		 * limit the work to 5 evictions and return. If that's not the
		 * case, we can do more.
		 */
		if (!busy && txn_state->pinned_id != WT_TXN_NONE &&
		    txn_global->current != txn_global->oldest_id)
			busy = true;
		max_pages_evicted = busy ? 5 : 20;

		/* See if eviction is still needed. */
		if (!__wt_eviction_needed(session, busy, &pct_full) ||
		    (pct_full < 100 &&
		    cache->pages_evict > init_evict_count + max_pages_evicted))
			break;

		/*
		 * Don't make application threads participate in scrubbing for
		 * checkpoints.  Just throttle updates instead.
		 */
		if (busy && WT_EVICT_HAS_WORKERS(session) &&
		    cache->eviction_scrub_limit > 0.0 &&
		    !F_ISSET(cache, WT_CACHE_EVICT_CLEAN_HARD)) {
			__wt_yield();
			continue;
		}

		/* Evict a page. */
		switch (ret = __evict_page(session, false)) {
		case 0:
			if (busy)
				goto err;
			/* FALLTHROUGH */
		case EBUSY:
			break;
		case WT_NOTFOUND:
			/* Allow the queue to re-populate before retrying. */
			__wt_cond_wait(
			    session, conn->evict_threads.wait_cond, 10000);
			cache->app_waits++;
			break;
		default:
			goto err;
		}
	}

err:	if (WT_STAT_ENABLED(session) &&
	    !F_ISSET(session, WT_SESSION_INTERNAL)) {
		__wt_epoch(session, &leave);
		WT_STAT_CONN_INCRV(session,
		    application_cache_time, WT_TIMEDIFF_US(leave, enter));
	}

	return (ret);
	/* NOTREACHED */
}

/*
 * __wt_page_evict_urgent --
 *      Set a page to be evicted as soon as possible.
 */
bool
__wt_page_evict_urgent(WT_SESSION_IMPL *session, WT_REF *ref)
{
	WT_CACHE *cache;
	WT_EVICT_ENTRY *evict;
	WT_EVICT_QUEUE *urgent_queue;
	WT_PAGE *page;
	bool queued;

	/* Root pages should never be evicted via LRU. */
	WT_ASSERT(session, !__wt_ref_is_root(ref));

	page = ref->page;
	if (F_ISSET_ATOMIC(page, WT_PAGE_EVICT_LRU) ||
	    F_ISSET(S2BT(session), WT_BTREE_NO_EVICTION))
		return (false);

	/* Append to the urgent queue if we can. */
	cache = S2C(session)->cache;
	urgent_queue = &cache->evict_queues[WT_EVICT_URGENT_QUEUE];
	queued = false;

	__wt_spin_lock(session, &cache->evict_queue_lock);
	if (F_ISSET_ATOMIC(page, WT_PAGE_EVICT_LRU) ||
	    F_ISSET(S2BT(session), WT_BTREE_NO_EVICTION))
		goto done;

	__wt_spin_lock(session, &urgent_queue->evict_lock);
	if (__evict_queue_empty(urgent_queue, false)) {
		urgent_queue->evict_current = urgent_queue->evict_queue;
		urgent_queue->evict_candidates = 0;
	}
	evict = urgent_queue->evict_queue + urgent_queue->evict_candidates;
	if (evict < urgent_queue->evict_queue + cache->evict_slots &&
	    __evict_push_candidate(session, urgent_queue, evict, ref)) {
		++urgent_queue->evict_candidates;
		queued = true;
	}
	__wt_spin_unlock(session, &urgent_queue->evict_lock);

done:	__wt_spin_unlock(session, &cache->evict_queue_lock);
	if (queued) {
		WT_STAT_CONN_INCR(session, cache_eviction_pages_queued_urgent);
		if (WT_EVICT_HAS_WORKERS(session))
			__wt_cond_signal(session,
			    S2C(session)->evict_threads.wait_cond);
		else
			__wt_evict_server_wake(session);
	}

	return (queued);
}

/*
 * __wt_evict_priority_set --
 *	Set a tree's eviction priority.
 */
void
__wt_evict_priority_set(WT_SESSION_IMPL *session, uint64_t v)
{
	S2BT(session)->evict_priority = v;
}

/*
 * __wt_evict_priority_clear --
 *	Clear a tree's eviction priority.
 */
void
__wt_evict_priority_clear(WT_SESSION_IMPL *session)
{
	S2BT(session)->evict_priority = 0;
}

#ifdef HAVE_DIAGNOSTIC
/*
 * __wt_cache_dump --
 *	Dump debugging information to a file (default stderr) about the size of
 *	the files in the cache.
 */
int
__wt_cache_dump(WT_SESSION_IMPL *session, const char *ofile)
{
	FILE *fp;
	WT_CONNECTION_IMPL *conn;
	WT_DATA_HANDLE *dhandle, *saved_dhandle;
	WT_PAGE *page;
	WT_REF *next_walk;
	uint64_t intl_bytes, intl_bytes_max, intl_dirty_bytes;
	uint64_t intl_dirty_bytes_max, intl_dirty_pages, intl_pages;
	uint64_t leaf_bytes, leaf_bytes_max, leaf_dirty_bytes;
	uint64_t leaf_dirty_bytes_max, leaf_dirty_pages, leaf_pages;
	uint64_t total_bytes, total_dirty_bytes;
	size_t size;

	conn = S2C(session);
	total_bytes = total_dirty_bytes = 0;

	if (ofile == NULL)
		fp = stderr;
	else if ((fp = fopen(ofile, "w")) == NULL)
		return (EIO);

	/* Note: odd string concatenation avoids spelling errors. */
	(void)fprintf(fp, "==========\n" "cache dump\n");

	saved_dhandle = session->dhandle;
	TAILQ_FOREACH(dhandle, &conn->dhqh, q) {
		if (!WT_PREFIX_MATCH(dhandle->name, "file:") ||
		    !F_ISSET(dhandle, WT_DHANDLE_OPEN))
			continue;

		intl_bytes = intl_bytes_max = intl_dirty_bytes = 0;
		intl_dirty_bytes_max = intl_dirty_pages = intl_pages = 0;
		leaf_bytes = leaf_bytes_max = leaf_dirty_bytes = 0;
		leaf_dirty_bytes_max = leaf_dirty_pages = leaf_pages = 0;

		next_walk = NULL;
		session->dhandle = dhandle;
		while (__wt_tree_walk(session, &next_walk,
		    WT_READ_CACHE | WT_READ_NO_EVICT | WT_READ_NO_WAIT) == 0 &&
		    next_walk != NULL) {
			page = next_walk->page;
			size = page->memory_footprint;

			if (WT_PAGE_IS_INTERNAL(page)) {
				++intl_pages;
				intl_bytes += size;
				intl_bytes_max = WT_MAX(intl_bytes_max, size);
				if (__wt_page_is_modified(page)) {
					++intl_dirty_pages;
					intl_dirty_bytes += size;
					intl_dirty_bytes_max =
					    WT_MAX(intl_dirty_bytes_max, size);
				}
			} else {
				++leaf_pages;
				leaf_bytes += size;
				leaf_bytes_max = WT_MAX(leaf_bytes_max, size);
				if (__wt_page_is_modified(page)) {
					++leaf_dirty_pages;
					leaf_dirty_bytes += size;
					leaf_dirty_bytes_max =
					    WT_MAX(leaf_dirty_bytes_max, size);
				}
			}
		}
		session->dhandle = NULL;

		if (dhandle->checkpoint == NULL)
			(void)fprintf(fp, "%s(<live>): \n", dhandle->name);
		else
			(void)fprintf(fp, "%s(checkpoint=%s): \n",
			    dhandle->name, dhandle->checkpoint);
		if (intl_pages != 0)
			(void)fprintf(fp,
			    "\t" "internal: "
			    "%" PRIu64 " pages, "
			    "%" PRIu64 "MB, "
			    "%" PRIu64 "/%" PRIu64 " clean/dirty pages, "
			    "%" PRIu64 "/%" PRIu64 " clean/dirty MB, "
			    "%" PRIu64 "MB max page, "
			    "%" PRIu64 "MB max dirty page\n",
			    intl_pages,
			    intl_bytes >> 20,
			    intl_pages - intl_dirty_pages,
			    intl_dirty_pages,
			    (intl_bytes - intl_dirty_bytes) >> 20,
			    intl_dirty_bytes >> 20,
			    intl_bytes_max >> 20,
			    intl_dirty_bytes_max >> 20);
		if (leaf_pages != 0)
			(void)fprintf(fp,
			    "\t" "leaf: "
			    "%" PRIu64 " pages, "
			    "%" PRIu64 "MB, "
			    "%" PRIu64 "/%" PRIu64 " clean/dirty pages, "
			    "%" PRIu64 "/%" PRIu64 " clean/dirty MB, "
			    "%" PRIu64 "MB max page, "
			    "%" PRIu64 "MB max dirty page\n",
			    leaf_pages,
			    leaf_bytes >> 20,
			    leaf_pages - leaf_dirty_pages,
			    leaf_dirty_pages,
			    (leaf_bytes - leaf_dirty_bytes) >> 20,
			    leaf_dirty_bytes >> 20,
			    leaf_bytes_max >> 20,
			    leaf_dirty_bytes_max >> 20);

		total_bytes += intl_bytes + leaf_bytes;
		total_dirty_bytes += intl_dirty_bytes + leaf_dirty_bytes;
	}
	session->dhandle = saved_dhandle;

	/*
	 * Apply the overhead percentage so our total bytes are comparable with
	 * the tracked value.
	 */
	total_bytes = __wt_cache_bytes_plus_overhead(conn->cache, total_bytes);

	(void)fprintf(fp,
	    "cache dump: "
	    "total found = %" PRIu64 "MB vs tracked inuse %" PRIu64 "MB\n"
	    "total dirty bytes = %" PRIu64 "MB\n",
	    total_bytes >> 20, __wt_cache_bytes_inuse(conn->cache) >> 20,
	    total_dirty_bytes >> 20);
	(void)fprintf(fp, "==========\n");

	if (ofile != NULL && fclose(fp) != 0)
		return (EIO);
	return (0);
}
#endif
