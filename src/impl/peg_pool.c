#include "peg_pool.h"

#include "../compat/cpthread.h"
#include "../def/cpthread_defs.h"
#include "../util/io_util.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

enum {
  PEG_POOL_QUEUE_INIT_CAP = 1024,
  // Per-worker stack. Workers recurse into nested solves while help-draining
  // the queue (bounded by the PEG fork-nesting cap), so the small default
  // secondary-thread stack is not enough. 64 MiB is virtual address space,
  // committed lazily, so only the nesting depth actually reached is paid for.
  PEG_POOL_WORKER_STACK_BYTES = 64 * 1024 * 1024,
};

// ---------------------------------------------------------------------------
// Work-stealing pool
// ---------------------------------------------------------------------------
//
// Workers loop on a shared FIFO queue protected by a mutex + condition
// variable. Callers submit a "batch" of work items (each is a function
// pointer + opaque arg) and then call submit_and_wait to block until every
// item in the batch has run. While blocked, the waiter drains queue items
// itself (help-while-waiting) so deeply nested submissions can't deadlock.
//
// Queue grows on demand (push doubles capacity if full). Items carry a
// pointer back to their batch's pending counter + completion CV.

typedef struct PegPoolBatch {
  atomic_int pending; // remaining items in this batch
  cpthread_mutex_t mutex;
  cpthread_cond_t cv; // signaled when pending hits 0
} PegPoolBatch;

typedef struct PegPoolItem {
  PegPoolFn fn;
  void *arg;
  PegPoolBatch *batch; // may be NULL for fire-and-forget items (unused today)
} PegPoolItem;

typedef struct PegPoolWorkerCtx {
  struct PegPool *pool;
  int worker_idx;
} PegPoolWorkerCtx;

struct PegPool {
  int num_workers;
  int thread_index_offset;
  cpthread_t *threads;
  PegPoolWorkerCtx *worker_ctxs;
  // Ring-ish queue. Simpler: linear buffer with head/tail; grow on overflow.
  PegPoolItem *queue;
  int q_head;  // next pop index
  int q_count; // items currently queued
  int q_cap;   // allocated capacity
  cpthread_mutex_t q_mutex;
  cpthread_cond_t q_cv_nonempty;
  bool shutdown;
};

static void pp_batch_init(PegPoolBatch *batch, int n) {
  atomic_init(&batch->pending, n);
  cpthread_mutex_init(&batch->mutex);
  cpthread_cond_init(&batch->cv);
}

// Push one item onto the queue. Caller must NOT hold q_mutex.
static void pp_push(PegPool *pool, PegPoolItem item) {
  cpthread_mutex_lock(&pool->q_mutex);
  if (pool->q_count == pool->q_cap) {
    // Grow: copy items into a new linearized buffer.
    int new_cap = pool->q_cap * 2;
    PegPoolItem *new_q = malloc_or_die((size_t)new_cap * sizeof(PegPoolItem));
    for (int queue_idx = 0; queue_idx < pool->q_count; queue_idx++) {
      new_q[queue_idx] = pool->queue[(pool->q_head + queue_idx) % pool->q_cap];
    }
    free(pool->queue);
    pool->queue = new_q;
    pool->q_cap = new_cap;
    pool->q_head = 0;
  }
  int tail = (pool->q_head + pool->q_count) % pool->q_cap;
  pool->queue[tail] = item;
  pool->q_count++;
  cpthread_cond_signal(&pool->q_cv_nonempty);
  cpthread_mutex_unlock(&pool->q_mutex);
}

// Try to pop one item without blocking. Returns true if popped.
static bool pp_try_pop(PegPool *pool, PegPoolItem *out) {
  cpthread_mutex_lock(&pool->q_mutex);
  if (pool->q_count == 0) {
    cpthread_mutex_unlock(&pool->q_mutex);
    return false;
  }
  *out = pool->queue[pool->q_head];
  pool->q_head = (pool->q_head + 1) % pool->q_cap;
  pool->q_count--;
  cpthread_mutex_unlock(&pool->q_mutex);
  return true;
}

// Block until an item is available or shutdown is set. Returns false on
// shutdown with no item.
static bool pp_pop_or_shutdown(PegPool *pool, PegPoolItem *out) {
  cpthread_mutex_lock(&pool->q_mutex);
  while (pool->q_count == 0 && !pool->shutdown) {
    cpthread_cond_wait(&pool->q_cv_nonempty, &pool->q_mutex);
  }
  if (pool->q_count == 0) {
    cpthread_mutex_unlock(&pool->q_mutex);
    return false;
  }
  *out = pool->queue[pool->q_head];
  pool->q_head = (pool->q_head + 1) % pool->q_cap;
  pool->q_count--;
  cpthread_mutex_unlock(&pool->q_mutex);
  return true;
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
  const bool trace = getenv("PEG_POOL_TRACE") != NULL;
  if (trace) {
    fprintf(stderr,
            "[peg_pool TRACE] worker=%d START item=%p batch=%p fn=%p\n",
            worker_idx, (void *)item, (void *)item->batch,
            (void *)(uintptr_t)item->fn);
    fflush(stderr);
  }
  item->fn(item->arg, worker_idx);
  if (trace) {
    fprintf(stderr, "[peg_pool TRACE] worker=%d FN-DONE item=%p batch=%p\n",
            worker_idx, (void *)item, (void *)item->batch);
    fflush(stderr);
  }
  if (item->batch) {
    cpthread_mutex_lock(&item->batch->mutex);
    int prev = atomic_fetch_sub(&item->batch->pending, 1);
    if (prev == 1) {
      cpthread_cond_broadcast(&item->batch->cv);
    }
    cpthread_mutex_unlock(&item->batch->mutex);
    if (trace) {
      fprintf(stderr, "[peg_pool TRACE] worker=%d DEC batch=%p prev=%d\n",
              worker_idx, (void *)item->batch, prev);
      fflush(stderr);
    }
  } else if (trace) {
    fprintf(stderr, "[peg_pool TRACE] worker=%d NULL-BATCH item=%p\n",
            worker_idx, (void *)item);
    fflush(stderr);
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

// Submit a contiguous array of items as one batch and wait for all to
// complete. The calling thread helps drain the queue while waiting so
// nested submissions don't deadlock. `helper_worker_idx` is the thread
// index used for cache keying when the helper runs items; pass the
// calling worker's idx if you're inside a worker, else any idx outside
// [thread_index_offset, thread_index_offset + num_workers).
static void pp_submit_and_wait(PegPool *pool, PegPoolItem *items, int n,
                               int helper_worker_idx) {
  PegPoolBatch batch;
  pp_batch_init(&batch, n);
  for (int item_idx = 0; item_idx < n; item_idx++) {
    items[item_idx].batch = &batch;
    pp_push(pool, items[item_idx]);
  }
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
  const int n_total = n;
  const int debug_on = getenv("PEG_POOL_DEBUG") != NULL;
  const char *to_env = getenv("PEG_POOL_STUCK_TIMEOUT_S");
  int stuck_timeout_s = to_env ? atoi(to_env) : 60;
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
    if (pp_try_pop(pool, &item)) {
      pp_run_item(&item, helper_worker_idx);
      stuck_iterations = 0;
      continue;
    }
    // Queue empty. Acquire batch.mutex and check pending under the lock.
    // If pending == 0 here, we're guaranteed that any decrementer with
    // prev == 1 has already released the lock (since they hold it across
    // the sub+broadcast+unlock window).
    cpthread_mutex_lock(&batch.mutex);
    if (atomic_load(&batch.pending) == 0) {
      cpthread_mutex_unlock(&batch.mutex);
      break;
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += iter_s;  // diagnostic wake-up
    int ret = pthread_cond_timedwait(&batch.cv, &batch.mutex, &ts);
    if (ret == ETIMEDOUT) {
      const int p_after = atomic_load(&batch.pending);
      cpthread_mutex_lock(&pool->q_mutex);
      const int q_count = pool->q_count;
      const bool q_shutdown = pool->shutdown;
      cpthread_mutex_unlock(&pool->q_mutex);
      if (watchdog_on && (debug_on || p_after == last_logged_pending)) {
        stuck_iterations++;
        fprintf(stderr,
                "[peg_pool WAIT-STUCK] pending=%d/%d, q_count=%d, "
                "shutdown=%d, helper_worker=%d, stuck_iters=%d\n",
                p_after, n_total, q_count, q_shutdown ? 1 : 0,
                helper_worker_idx, stuck_iterations);
        fflush(stderr);
      }
      last_logged_pending = p_after;
      if (watchdog_on && stuck_iterations >= 6) {
        fprintf(stderr,
                "[peg_pool FATAL] deadlock detected: pending=%d/%d,"
                " q_count=%d, no progress for %ds\n",
                p_after, n_total, q_count, stuck_timeout_s);
        fflush(stderr);
        cpthread_mutex_unlock(&batch.mutex);
        abort();
      }
    }
    cpthread_mutex_unlock(&batch.mutex);
  }
}

PegPool *peg_pool_create(int num_workers, int thread_index_offset) {
  if (num_workers < 1) {
    num_workers = 1;
  }
  PegPool *pool = malloc_or_die(sizeof(*pool));
  pool->num_workers = num_workers;
  pool->thread_index_offset = thread_index_offset;
  pool->q_cap = PEG_POOL_QUEUE_INIT_CAP;
  pool->queue = malloc_or_die((size_t)pool->q_cap * sizeof(PegPoolItem));
  pool->q_head = 0;
  pool->q_count = 0;
  pool->shutdown = false;
  cpthread_mutex_init(&pool->q_mutex);
  cpthread_cond_init(&pool->q_cv_nonempty);
  pool->threads = malloc_or_die((size_t)num_workers * sizeof(cpthread_t));
  pool->worker_ctxs = malloc_or_die((size_t)num_workers * sizeof(PegPoolWorkerCtx));
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
  free(pool->queue);
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
  const int n = pool->q_count;
  cpthread_mutex_unlock(&pool->q_mutex);
  return n;
}

void peg_pool_submit_and_wait(PegPool *pool, PegPoolFn fn, void *const *args,
                              int n, int helper_worker_idx) {
  if (n <= 0) {
    return;
  }
  PegPoolItem *items = malloc_or_die((size_t)n * sizeof(*items));
  for (int item_idx = 0; item_idx < n; item_idx++) {
    items[item_idx].fn = fn;
    items[item_idx].arg = args[item_idx];
    items[item_idx].batch = NULL;
  }
  pp_submit_and_wait(pool, items, n, helper_worker_idx);
  free(items);
}
