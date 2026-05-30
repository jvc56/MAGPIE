#ifndef PEG_POOL_H
#define PEG_POOL_H

// ---------------------------------------------------------------------------
// Work-stealing thread pool
// ---------------------------------------------------------------------------
//
// Persistent N-worker thread pool used by the pre-endgame solver to
// dispatch (cand × scenario) leaves, opp-perception inner work, and any
// other batched parallel jobs. Callers submit a batch of items and block
// until all complete; while blocked, the calling thread helps drain the
// queue so deeply nested submissions don't deadlock.
//
// Workers handle re-entry via help-while-waiting: a worker that submits a
// batch and waits for completion drains the shared queue while waiting, so
// nested submissions don't deadlock as long as queue items make forward
// progress without waiting on the same worker.
typedef struct PegPool PegPool;

// Generic work-item signature. `worker_idx` is the pool's thread index used
// for cache keying (movegen / endgame caches keyed by this number).
typedef void (*PegPoolFn)(void *arg, int worker_idx);

// Create a pool with `num_workers` persistent worker threads. Movegen and
// endgame caches keyed by thread index will use indices in
// [thread_index_offset, thread_index_offset + num_workers).
PegPool *peg_pool_create(int num_workers, int thread_index_offset);

// Drain in-flight work, join workers, free. It is an error to destroy a
// pool while a submit_and_wait is still running on it.
void peg_pool_destroy(PegPool *pool);

// Number of workers / thread_index_offset queried back so external callers
// can pick a non-colliding helper_worker_idx and per-thread scratch sizes.
int peg_pool_num_workers(const PegPool *pool);
int peg_pool_thread_index_offset(const PegPool *pool);

// Current number of items queued (snapshot, taken under the queue mutex). Lets
// a caller gauge spare capacity: if below num_workers, workers are about to
// starve, so spawning more parallel sub-work is worthwhile (otherwise it's
// just redundant work contending for already-busy cores).
int peg_pool_queue_count(PegPool *pool);

// Submit a batch of `n` items as `(fn, args[i])` pairs and block until all
// complete. While blocked, the calling thread helps drain queue items so
// nested submissions don't deadlock. `helper_worker_idx` is the index used
// for cache keying when the helper runs items; pass the calling worker's
// idx if invoked from inside a worker, otherwise any idx outside the
// pool's [offset, offset + num_workers) range.
void peg_pool_submit_and_wait(PegPool *pool, PegPoolFn fn,
                              void *const *args, int n,
                              int helper_worker_idx);

#endif
