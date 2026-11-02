#include "peg_pool.h"

#include "../compat/cpthread.h"
#include "../compat/cpthread_defs.h"
#include "../compat/ctime.h"
#include "../compat/memory_info.h"
#include "../def/peg_defs.h"
#include "../util/io_util.h"
#include "move_gen.h"
#include "move_gen_base.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
// ---------------------------------------------------------------------------
// Work-stealing pool
// ---------------------------------------------------------------------------
//
// Workers loop on two FIFO queues protected by a mutex + condition variable,
// taking re-entrant items first, since they finish work already started.
// Callers submit a "batch" of work items (each is a function pointer + opaque
// arg) and then call submit_and_wait to block until every item in the batch has
// run. While blocked, the waiter drains queue items itself (help-while-waiting)
// so deeply nested submissions can't deadlock.
//
// Queues grow on demand (push doubles capacity if full). Items carry a
// pointer back to their batch's pending counter + completion CV.

struct PegPoolBatch {
  atomic_int pending; // remaining items in this batch
  int total;          // items submitted
  cpthread_mutex_t mutex;
  cpthread_cond_t cv; // signaled when pending hits 0
};

typedef struct PegPoolItem {
  PegPoolFn fn;
  void *arg;
  PegPoolBatch *batch; // may be NULL for fire-and-forget items (unused today)
  // True iff this item is safe to run re-entrantly: i.e. a thread already
  // executing another job may run it while help-draining without corrupting
  // that job's state. Inner-peg scenario jobs (which work on the per-worker
  // nest-frame stack, not the fixed scratch games) set this; outer
  // cand/scenario jobs do not. A waiter blocked inside a re-entrant submit
  // only ever pulls re-entrant items, so it cannot start an outer job on a
  // worker that is mid-outer-job.
  bool reentrant;
  // The move's budget, from the thread that submitted the item: whichever
  // thread runs it charges that budget, and stops on it.
  MovegenBudget *movegen_budget;
} PegPoolItem;

typedef struct PegPoolQueue {
  PegPoolItem *items; // ring buffer
  int head;           // next pop index
  int count;          // items currently queued
  int cap;            // allocated capacity
} PegPoolQueue;

typedef struct PegPoolWorkerCtx {
  struct PegPool *pool;
  int worker_idx;
} PegPoolWorkerCtx;

struct PegPool {
  int num_workers;
  int thread_index_offset;
  cpthread_t *threads;
  PegPoolWorkerCtx *worker_ctxs;
  PegPoolQueue outer;     // items only a thread between jobs may run
  PegPoolQueue reentrant; // items any thread may run
  cpthread_mutex_t q_mutex;
  cpthread_cond_t q_cv_nonempty;
  bool shutdown;
  // Workers currently blocked in pp_pop_or_shutdown waiting for work — i.e.
  // genuinely idle cores. A snapshot read by peg_pool_idle_workers lets a
  // caller decide when to hand spare cores to other work (e.g. inject an
  // ABDADA worker into a long-running endgame solve).
  atomic_int idle_workers;
  // Threads asleep in submit_and_wait until the rest of their batch finishes,
  // whose cores are idle too, although they cannot take queued work.
  atomic_int blocked_waiters;
  // No-progress watchdog budget (seconds); 0 disables. Seeded from
  // PEG_POOL_STUCK_TIMEOUT_S at create; override via the setter.
  int stuck_timeout_s;
};

static void pp_batch_init(PegPoolBatch *batch, int n) {
  atomic_init(&batch->pending, n);
  batch->total = n;
  cpthread_mutex_init(&batch->mutex);
  cpthread_cond_init(&batch->cv);
}

static void pp_queue_init(PegPoolQueue *queue) {
  queue->cap = PEG_POOL_QUEUE_INIT_CAP;
  queue->items = malloc_or_die((size_t)queue->cap * sizeof(PegPoolItem));
  queue->head = 0;
  queue->count = 0;
}

static void pp_queue_push(PegPoolQueue *queue, PegPoolItem item) {
  if (queue->count == queue->cap) {
    // Grow: copy items into a new linearized buffer.
    int new_cap = queue->cap * 2;
    PegPoolItem *new_items =
        malloc_or_die((size_t)new_cap * sizeof(PegPoolItem));
    for (int queue_idx = 0; queue_idx < queue->count; queue_idx++) {
      new_items[queue_idx] =
          queue->items[(queue->head + queue_idx) % queue->cap];
    }
    free(queue->items);
    queue->items = new_items;
    queue->cap = new_cap;
    queue->head = 0;
  }
  queue->items[(queue->head + queue->count) % queue->cap] = item;
  queue->count++;
}

static PegPoolItem pp_queue_pop(PegPoolQueue *queue) {
  const PegPoolItem item = queue->items[queue->head];
  queue->head = (queue->head + 1) % queue->cap;
  queue->count--;
  return item;
}

// The next item any thread between jobs may run, of those queued. Caller holds
// q_mutex, and has checked that one is queued.
static PegPoolItem pp_pop_any_locked(PegPool *pool) {
  return pool->reentrant.count > 0 ? pp_queue_pop(&pool->reentrant)
                                   : pp_queue_pop(&pool->outer);
}

static bool pp_any_queued_locked(const PegPool *pool) {
  return pool->reentrant.count > 0 || pool->outer.count > 0;
}

// Push one item onto its queue. Caller must NOT hold q_mutex.
static void pp_push(PegPool *pool, PegPoolItem item) {
  cpthread_mutex_lock(&pool->q_mutex);
  pp_queue_push(item.reentrant ? &pool->reentrant : &pool->outer, item);
  cpthread_cond_signal(&pool->q_cv_nonempty);
  cpthread_mutex_unlock(&pool->q_mutex);
}

// Try to pop one item without blocking. Returns true if popped.
static bool pp_try_pop(PegPool *pool, PegPoolItem *out) {
  cpthread_mutex_lock(&pool->q_mutex);
  const bool popped = pp_any_queued_locked(pool);
  if (popped) {
    *out = pp_pop_any_locked(pool);
  }
  cpthread_mutex_unlock(&pool->q_mutex);
  return popped;
}

// Pop the next re-entrant-safe item. Used by a waiter that is itself executing
// a job: it may only help with re-entrant items, never start an outer job that
// would clobber the in-progress one.
static bool pp_try_pop_reentrant(PegPool *pool, PegPoolItem *out) {
  cpthread_mutex_lock(&pool->q_mutex);
  const bool popped = pool->reentrant.count > 0;
  if (popped) {
    *out = pp_queue_pop(&pool->reentrant);
  }
  cpthread_mutex_unlock(&pool->q_mutex);
  return popped;
}

// Block until an item is available or shutdown is set. Returns false on
// shutdown with no item.
static bool pp_pop_or_shutdown(PegPool *pool, PegPoolItem *out) {
  cpthread_mutex_lock(&pool->q_mutex);
  if (!pp_any_queued_locked(pool) && !pool->shutdown) {
    // Count this worker as idle for exactly the span it blocks on the queue.
    atomic_fetch_add(&pool->idle_workers, 1);
    while (!pp_any_queued_locked(pool) && !pool->shutdown) {
      cpthread_cond_wait(&pool->q_cv_nonempty, &pool->q_mutex);
    }
    atomic_fetch_sub(&pool->idle_workers, 1);
  }
  const bool popped = pp_any_queued_locked(pool);
  if (popped) {
    *out = pp_pop_any_locked(pool);
  }
  cpthread_mutex_unlock(&pool->q_mutex);
  return popped;
}

// Run an item and decrement its batch's pending counter, signaling the
// batch's CV when the last one drains.
//
// IMPORTANT: take batch->mutex BEFORE the decrement, not just around the
// broadcast. Otherwise the waiter (in submit_and_wait's outer loop) can
// observe pending == 0 via atomic_load and exit submit_and_wait *before*
// this thread reaches cpthread_mutex_lock — by which point the batch is
// on a now-popped stack frame and the mutex is invalid. Holding the lock
// across the sub means the waiter only ever sees pending == 0 from a
// state where the broadcaster has either already broadcast+unlocked
// (waiter has lock-acquire dependency) or hasn't yet acquired the lock
// (waiter will hit the broadcast on its next check). Diagnostics under
// PEG_POOL_TRACE confirmed the pre-fix race: e.g.
//   worker=101 prev=1 (about to lock)
//   waiter sees pending=0, returns, batch on stack invalidated
//   worker=101 cpthread_mutex_lock -> FATAL
static void pp_run_item(PegPoolItem *item, int worker_idx) {
  MovegenBudget *previous = gen_set_movegen_budget(item->movegen_budget);
  item->fn(item->arg, worker_idx);
  (void)gen_set_movegen_budget(previous);
  if (item->batch) {
    cpthread_mutex_lock(&item->batch->mutex);
    const int prev = atomic_fetch_sub(&item->batch->pending, 1);
    if (prev == 1) {
      cpthread_cond_broadcast(&item->batch->cv);
    }
    cpthread_mutex_unlock(&item->batch->mutex);
  }
}

static void *pp_worker_main(void *arg) {
  PegPoolWorkerCtx *ctx = (PegPoolWorkerCtx *)arg;
  while (true) {
    PegPoolItem item;
    if (!pp_pop_or_shutdown(ctx->pool, &item)) {
      break;
    }
    pp_run_item(&item, ctx->worker_idx);
  }
  return NULL;
}

// Queues `n` (fn, args[i]) items as one batch.
static void pp_push_batch(PegPool *pool, PegPoolBatch *batch, PegPoolFn fn,
                          void *const *args, int n, bool reentrant) {
  MovegenBudget *movegen_budget = gen_get_movegen_budget();
  for (int item_idx = 0; item_idx < n; item_idx++) {
    const PegPoolItem item = {
        .fn = fn,
        .arg = args[item_idx],
        .batch = batch,
        .reentrant = reentrant,
        .movegen_budget = movegen_budget,
    };
    pp_push(pool, item);
  }
}

// Waits for a queued batch to complete. The calling thread helps drain the
// queue while waiting so nested submissions don't deadlock.
// `helper_worker_idx` is the thread index used for cache keying when the helper
// runs items; pass the calling worker's idx if you're inside a worker, else any
// idx outside [thread_index_offset, thread_index_offset + num_workers).
static void pp_wait_batch(PegPool *pool, PegPoolBatch *batch,
                          int helper_worker_idx, bool reentrant) {
  // Help-while-waiting. Exit MUST go through the locked batch.mutex
  // check — otherwise we could observe pending == 0 via atomic_load
  // while a decrementer (with prev == 1) is still mid-broadcast, and
  // return submit_and_wait before the broadcast completes. The popped
  // stack frame then has the batch's mutex/cv torn out from under the
  // broadcaster → FATAL. Pair this with pp_run_item taking batch->mutex
  // *before* the atomic_sub.
  //
  // PEG_POOL_STUCK_TIMEOUT_S overrides the total no-progress budget
  // (default 60s). Split into 6 iterations, so each timed wake-up is
  // timeout/6 seconds. PEG_POOL_STUCK_TIMEOUT_S=0 disables the watchdog —
  // the calling thread still helps drain the queue but never aborts.
  const int n_total = batch->total;
  const int debug_on = getenv("PEG_POOL_DEBUG") != NULL;
  // Per-pool budget (seconds); the env still seeds the default at create, but a
  // caller can disable the watchdog for legitimately long work (e.g. deep PEG
  // endgames) via peg_pool_set_stuck_timeout_seconds(pool, 0).
  int stuck_timeout_s = pool->stuck_timeout_s;
  const bool watchdog_on = stuck_timeout_s > 0;
  if (watchdog_on && stuck_timeout_s < 6) {
    stuck_timeout_s = 6;
  }
  // Iteration wakeup. When the watchdog is disabled, still wake up periodically
  // so the help-while-waiting drain stays responsive — every 60s is fine.
  const int iter_s = watchdog_on ? stuck_timeout_s / 6 : 60;
  int last_logged_pending = -1;
  int stuck_iterations = 0;
  while (true) {
    PegPoolItem item;
    // A re-entrant waiter (one already executing a job) must only help with
    // re-entrant-safe items; a top-level waiter (the main thread on the outer
    // submit) may help with anything queued.
    const bool got =
        reentrant ? pp_try_pop_reentrant(pool, &item) : pp_try_pop(pool, &item);
    if (got) {
      pp_run_item(&item, helper_worker_idx);
      stuck_iterations = 0;
      continue;
    }
    // Queue empty. Acquire batch.mutex and check pending under the lock.
    // If pending == 0 here, we're guaranteed that any decrementer with
    // prev == 1 has already released the lock (since they hold it across
    // the sub+broadcast+unlock window).
    cpthread_mutex_lock(&batch->mutex);
    if (atomic_load(&batch->pending) == 0) {
      cpthread_mutex_unlock(&batch->mutex);
      break;
    }
    TimeSpec ts;
    ctimer_clock_gettime_realtime(&ts);
    ts.tv_sec += iter_s; // diagnostic wake-up
    // A wake re-checks pending at the loop top; a timeout drives the
    // diagnostic watchdog below. Any other failure ends the process rather
    // than spinning here forever with no diagnostic.
    atomic_fetch_add(&pool->blocked_waiters, 1);
    const bool woken = cpthread_cond_timedwait(&batch->cv, &batch->mutex, &ts);
    atomic_fetch_sub(&pool->blocked_waiters, 1);
    if (!woken) {
      const int p_after = atomic_load(&batch->pending);
      cpthread_mutex_lock(&pool->q_mutex);
      const int q_count = pool->outer.count + pool->reentrant.count;
      const bool q_shutdown = pool->shutdown;
      cpthread_mutex_unlock(&pool->q_mutex);
      if (watchdog_on && (debug_on || p_after == last_logged_pending)) {
        stuck_iterations++;
        (void)fprintf(stderr,
                      "[peg_pool WAIT-STUCK] pending=%d/%d, q_count=%d, "
                      "shutdown=%d, helper_worker=%d, stuck_iters=%d\n",
                      p_after, n_total, q_count, q_shutdown ? 1 : 0,
                      helper_worker_idx, stuck_iterations);
        (void)fflush(stderr);
      }
      last_logged_pending = p_after;
      if (watchdog_on && stuck_iterations >= 6) {
        (void)fprintf(stderr,
                      "[peg_pool FATAL] deadlock detected: pending=%d/%d,"
                      " q_count=%d, no progress for %ds\n",
                      p_after, n_total, q_count, stuck_timeout_s);
        (void)fflush(stderr);
        cpthread_mutex_unlock(&batch->mutex);
        abort();
      }
    }
    cpthread_mutex_unlock(&batch->mutex);
  }
}

PegPool *peg_pool_create(int num_workers, int thread_index_offset) {
  if (num_workers < 1) {
    num_workers = 1;
  }
  PegPool *pool = malloc_or_die(sizeof(*pool));
  pool->num_workers = num_workers;
  pool->thread_index_offset = thread_index_offset;
  pp_queue_init(&pool->outer);
  pp_queue_init(&pool->reentrant);
  pool->shutdown = false;
  atomic_init(&pool->idle_workers, 0);
  atomic_init(&pool->blocked_waiters, 0);
  const char *to_env = getenv("PEG_POOL_STUCK_TIMEOUT_S");
  pool->stuck_timeout_s = to_env ? (int)strtol(to_env, NULL, 10) : 60;
  cpthread_mutex_init(&pool->q_mutex);
  cpthread_cond_init(&pool->q_cv_nonempty);
  pool->threads = malloc_or_die((size_t)num_workers * sizeof(cpthread_t));
  pool->worker_ctxs =
      malloc_or_die((size_t)num_workers * sizeof(PegPoolWorkerCtx));
  // Workers help-drain the queue while blocked on a submitted batch, so a
  // worker can recurse into nested solves on its own stack (bounded by the
  // PEG fork-nesting cap). The 512 KB default secondary-thread stack overflows
  // there; request a large stack (lazily committed, so only the depth actually
  // used is paid for) to keep deep nesting stack-safe.
  for (int worker_idx = 0; worker_idx < num_workers; worker_idx++) {
    pool->worker_ctxs[worker_idx].pool = pool;
    pool->worker_ctxs[worker_idx].worker_idx = thread_index_offset + worker_idx;
    cpthread_create_with_stack(&pool->threads[worker_idx], pp_worker_main,
                               &pool->worker_ctxs[worker_idx],
                               PEG_POOL_WORKER_STACK_BYTES);
  }
  return pool;
}

void peg_pool_destroy(PegPool *pool) {
  if (!pool) {
    return;
  }
  cpthread_mutex_lock(&pool->q_mutex);
  pool->shutdown = true;
  cpthread_cond_broadcast(&pool->q_cv_nonempty);
  cpthread_mutex_unlock(&pool->q_mutex);
  for (int worker_idx = 0; worker_idx < pool->num_workers; worker_idx++) {
    cpthread_join(pool->threads[worker_idx]);
  }
  free(pool->threads);
  free(pool->worker_ctxs);
  free(pool->outer.items);
  free(pool->reentrant.items);
  free(pool);
}

int peg_pool_num_workers(const PegPool *pool) {
  return pool ? pool->num_workers : 0;
}

int peg_pool_thread_index_offset(const PegPool *pool) {
  return pool ? pool->thread_index_offset : 0;
}

int peg_pool_queue_count(PegPool *pool) {
  if (!pool) {
    return 0;
  }
  cpthread_mutex_lock(&pool->q_mutex);
  const int n = pool->outer.count + pool->reentrant.count;
  cpthread_mutex_unlock(&pool->q_mutex);
  return n;
}

int peg_pool_idle_workers(PegPool *pool) {
  if (!pool) {
    return 0;
  }
  return atomic_load(&pool->idle_workers);
}

int peg_pool_spare_cores(PegPool *pool) {
  if (!pool) {
    return 0;
  }
  return atomic_load(&pool->idle_workers) + atomic_load(&pool->blocked_waiters);
}

void peg_pool_set_stuck_timeout_seconds(PegPool *pool, int seconds) {
  if (pool) {
    pool->stuck_timeout_s = seconds;
  }
}

static void pp_submit_batch(PegPool *pool, PegPoolFn fn, void *const *args,
                            int n, int helper_worker_idx, bool reentrant) {
  if (n <= 0) {
    return;
  }
  if (pool == NULL) {
    // No pool: run the batch inline on the calling thread. Consistent with the
    // NULL-tolerant accessors above, this lets a caller conditionally use a
    // pool (NULL => run inline) without a separate code path, and the work
    // still executes (rather than crashing or being silently dropped).
    for (int item_idx = 0; item_idx < n; item_idx++) {
      fn(args[item_idx], helper_worker_idx);
    }
    return;
  }
  PegPoolBatch batch;
  pp_batch_init(&batch, n);
  pp_push_batch(pool, &batch, fn, args, n, reentrant);
  pp_wait_batch(pool, &batch, helper_worker_idx, reentrant);
}

void peg_pool_submit_and_wait(PegPool *pool, PegPoolFn fn, void *const *args,
                              int n, int helper_worker_idx) {
  pp_submit_batch(pool, fn, args, n, helper_worker_idx, false);
}

void peg_pool_submit_and_wait_reentrant(PegPool *pool, PegPoolFn fn,
                                        void *const *args, int n,
                                        int helper_worker_idx) {
  pp_submit_batch(pool, fn, args, n, helper_worker_idx, true);
}

PegPoolBatch *peg_pool_submit(PegPool *pool, PegPoolFn fn, void *const *args,
                              int n) {
  PegPoolBatch *batch = malloc_or_die(sizeof(*batch));
  pp_batch_init(batch, n);
  pp_push_batch(pool, batch, fn, args, n, /*reentrant=*/false);
  return batch;
}

void peg_pool_wait(PegPool *pool, PegPoolBatch *batch, int helper_worker_idx) {
  pp_wait_batch(pool, batch, helper_worker_idx, /*reentrant=*/false);
  free(batch);
}
