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
  assert(atomic_load(&sum) == 2 * N);
  assert(atomic_load(&bad_idx) == 0); // all ran inline with the helper idx
}

void test_peg_pool(void) {
  test_peg_pool_basic();
  test_peg_pool_nested();
  test_peg_pool_single_worker();
  test_peg_pool_null_inline();
}
