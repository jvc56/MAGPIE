#include "peg_pool_test.h"

#include "../src/compat/ctime.h"
#include "../src/impl/peg_pool.h"
#include "../src/util/io_util.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>

// Each item adds its value to a shared sum and records whether it ran on a
// valid worker index (inside the pool's [offset, offset + num_workers) range,
// or the helper index used by the calling thread while it help-drains).
typedef struct {
  atomic_long *sum;
  int value;
  int offset;
  int num_workers;
  int helper_idx;
  atomic_int *bad_idx;
} AddItem;

static void add_fn(void *arg, int worker_idx) {
  AddItem *item = (AddItem *)arg;
  atomic_fetch_add(item->sum, (long)item->value);
  const bool in_pool = worker_idx >= item->offset &&
                       worker_idx < item->offset + item->num_workers;
  if (!in_pool && worker_idx != item->helper_idx) {
    atomic_fetch_add(item->bad_idx, 1);
  }
}

static void test_peg_pool_basic(void) {
  const int num_workers = 4;
  const int offset = 10;
  PegPool *pool = peg_pool_create(num_workers, offset);
  assert(peg_pool_num_workers(pool) == num_workers);
  assert(peg_pool_thread_index_offset(pool) == offset);

  // Empty submit is a no-op.
  peg_pool_submit_and_wait(pool, add_fn, NULL, 0, 999);

  enum { N = 2000 };
  // A helper index outside the pool's worker range, used by the calling thread.
  const int helper_idx = offset + num_workers + 1;
  atomic_long sum;
  atomic_init(&sum, 0);
  atomic_int bad_idx;
  atomic_init(&bad_idx, 0);

  AddItem *items = malloc_or_die(N * sizeof(AddItem));
  void **args = malloc_or_die(N * sizeof(void *));
  for (int i = 0; i < N; i++) {
    items[i] = (AddItem){.sum = &sum,
                         .value = i + 1,
                         .offset = offset,
                         .num_workers = num_workers,
                         .helper_idx = helper_idx,
                         .bad_idx = &bad_idx};
    args[i] = &items[i];
  }
  peg_pool_submit_and_wait(pool, add_fn, args, N, helper_idx);

  // Every item ran exactly once: the parallel sum equals 1 + 2 + ... + N.
  assert(atomic_load(&sum) == (long)N * (N + 1) / 2);
  // No item saw an unexpected worker index.
  assert(atomic_load(&bad_idx) == 0);
  // The queue is drained and idle workers is a sane snapshot once we return.
  assert(peg_pool_queue_count(pool) == 0);
  const int idle = peg_pool_idle_workers(pool);
  assert(idle >= 0 && idle <= num_workers);

  free(items);
  free(args);
  peg_pool_destroy(pool);
}

// Reentrant nested submit: each outer item, while running on a worker, submits
// a sub-batch and waits for it. The pool must help-drain the inner items on the
// waiting worker rather than deadlocking.
typedef struct {
  PegPool *pool;
  atomic_long *inner_count;
} NestItem;

static void inner_fn(void *arg, int worker_idx) {
  (void)worker_idx;
  atomic_fetch_add((atomic_long *)arg, 1L);
}

enum { NEST_INNER = 8 };

static void outer_fn(void *arg, int worker_idx) {
  NestItem *nest = (NestItem *)arg;
  void *inner_args[NEST_INNER];
  for (int k = 0; k < NEST_INNER; k++) {
    inner_args[k] = nest->inner_count;
  }
  // Re-enter the pool from inside a worker; helper idx is this worker's own
  // idx.
  peg_pool_submit_and_wait(nest->pool, inner_fn, inner_args, NEST_INNER,
                           worker_idx);
}

static void test_peg_pool_nested(void) {
  const int num_workers = 3;
  PegPool *pool = peg_pool_create(num_workers, 0);
  // Disable the no-progress watchdog; the nested reentry is legitimate work.
  peg_pool_set_stuck_timeout_seconds(pool, 0);

  enum { M = 50 };
  atomic_long inner_count;
  atomic_init(&inner_count, 0);
  NestItem items[M];
  void *args[M];
  for (int i = 0; i < M; i++) {
    items[i] = (NestItem){.pool = pool, .inner_count = &inner_count};
    args[i] = &items[i];
  }
  // Caller's helper idx is outside [0, num_workers).
  peg_pool_submit_and_wait(pool, outer_fn, args, M, num_workers + 10);

  assert(atomic_load(&inner_count) == (long)M * NEST_INNER);
  peg_pool_destroy(pool);
}

// A single worker must still run a multi-item batch (the calling thread
// help-drains alongside it) — a degenerate case that should not deadlock.
static void test_peg_pool_single_worker(void) {
  PegPool *pool = peg_pool_create(1, 0);
  enum { N = 100 };
  atomic_long sum;
  atomic_init(&sum, 0);
  atomic_int bad_idx;
  atomic_init(&bad_idx, 0);
  AddItem items[N];
  void *args[N];
  for (int i = 0; i < N; i++) {
    items[i] = (AddItem){.sum = &sum,
                         .value = 1,
                         .offset = 0,
                         .num_workers = 1,
                         .helper_idx = 5,
                         .bad_idx = &bad_idx};
    args[i] = &items[i];
  }
  peg_pool_submit_and_wait(pool, add_fn, args, N, 5);
  assert(atomic_load(&sum) == N);
  assert(atomic_load(&bad_idx) == 0);
  peg_pool_destroy(pool);
}

// A NULL pool runs the batch inline on the calling thread.
static void test_peg_pool_null_inline(void) {
  enum { N = 64 };
  atomic_long sum;
  atomic_init(&sum, 0);
  atomic_int bad_idx;
  atomic_init(&bad_idx, 0);
  AddItem items[N];
  void *args[N];
  for (int i = 0; i < N; i++) {
    // num_workers 0 + helper_idx 7 means "valid only if it ran on idx 7".
    items[i] = (AddItem){.sum = &sum,
                         .value = 2,
                         .offset = 0,
                         .num_workers = 0,
                         .helper_idx = 7,
                         .bad_idx = &bad_idx};
    args[i] = &items[i];
  }
  peg_pool_submit_and_wait(NULL, add_fn, args, N, 7);
  assert(atomic_load(&sum) == 2L * N);
  assert(atomic_load(&bad_idx) == 0); // all ran inline with the helper idx
}

// Batches queued without waiting all run, and can be waited for in any order.
static void test_peg_pool_queued_batches(void) {
  enum { NUM_BATCHES = 3, BATCH_SIZE = 200 };
  const int num_workers = 3;
  PegPool *pool = peg_pool_create(num_workers, 0);
  peg_pool_set_stuck_timeout_seconds(pool, 0);
  atomic_long sums[NUM_BATCHES];
  atomic_int bad_idx;
  atomic_init(&bad_idx, 0);
  AddItem items[NUM_BATCHES][BATCH_SIZE];
  void *args[NUM_BATCHES][BATCH_SIZE];
  PegPoolBatch *batches[NUM_BATCHES];
  for (int batch_idx = 0; batch_idx < NUM_BATCHES; batch_idx++) {
    atomic_init(&sums[batch_idx], 0);
    for (int item_idx = 0; item_idx < BATCH_SIZE; item_idx++) {
      items[batch_idx][item_idx] = (AddItem){.sum = &sums[batch_idx],
                                             .value = batch_idx + 1,
                                             .offset = 0,
                                             .num_workers = num_workers,
                                             .helper_idx = num_workers,
                                             .bad_idx = &bad_idx};
      args[batch_idx][item_idx] = &items[batch_idx][item_idx];
    }
    batches[batch_idx] =
        peg_pool_submit(pool, add_fn, args[batch_idx], BATCH_SIZE);
  }
  for (int batch_idx = NUM_BATCHES - 1; batch_idx >= 0; batch_idx--) {
    peg_pool_wait(pool, batches[batch_idx], num_workers);
    assert(atomic_load(&sums[batch_idx]) == (long)(batch_idx + 1) * BATCH_SIZE);
  }
  assert(atomic_load(&bad_idx) == 0);
  // An empty batch is already done.
  peg_pool_wait(pool, peg_pool_submit(pool, add_fn, NULL, 0), num_workers);
  peg_pool_destroy(pool);
}

// A thread asleep on the rest of its batch leaves its core spare, though it
// takes no queued work: it is not idle.
typedef struct {
  PegPool *pool;
  atomic_int submitter_idx;
  atomic_int started;
  atomic_bool release;
} BlockedWaiter;

// Holds until both of the batch's items have a thread, so one thread cannot run
// them both. The thread that submitted them then goes to sleep on the batch,
// and the other holds until the test has seen that.
static void blocked_inner_fn(void *arg, int worker_idx) {
  BlockedWaiter *blocked = (BlockedWaiter *)arg;
  atomic_fetch_add(&blocked->started, 1);
  while (atomic_load(&blocked->started) < 2) {
    ctime_nap(0.001);
  }
  while (worker_idx != atomic_load(&blocked->submitter_idx) &&
         !atomic_load(&blocked->release)) {
    ctime_nap(0.001);
  }
}

static void blocked_outer_fn(void *arg, int worker_idx) {
  BlockedWaiter *blocked = (BlockedWaiter *)arg;
  atomic_store(&blocked->submitter_idx, worker_idx);
  void *inner_args[2] = {blocked, blocked};
  peg_pool_submit_and_wait_reentrant(blocked->pool, blocked_inner_fn,
                                     inner_args, 2, worker_idx);
}

static void test_peg_pool_spare_cores(void) {
  assert(peg_pool_spare_cores(NULL) == 0);
  PegPool *pool = peg_pool_create(2, 0);
  peg_pool_set_stuck_timeout_seconds(pool, 0);
  BlockedWaiter blocked = {.pool = pool};
  atomic_init(&blocked.submitter_idx, -1);
  atomic_init(&blocked.started, 0);
  atomic_init(&blocked.release, false);
  void *outer_args[1] = {&blocked};
  PegPoolBatch *batch = peg_pool_submit(pool, blocked_outer_fn, outer_args, 1);
  while (peg_pool_spare_cores(pool) == 0 || peg_pool_idle_workers(pool) > 0) {
    ctime_nap(0.001);
  }
  assert(peg_pool_idle_workers(pool) == 0);
  assert(peg_pool_spare_cores(pool) == 1);
  atomic_store(&blocked.release, true);
  peg_pool_wait(pool, batch, 2);
  peg_pool_destroy(pool);
}

void test_peg_pool(void) {
  test_peg_pool_basic();
  test_peg_pool_nested();
  test_peg_pool_single_worker();
  test_peg_pool_null_inline();
  test_peg_pool_queued_batches();
  test_peg_pool_spare_cores();
}
