// Browser allocator telemetry and probes have no native equivalent.
#ifdef __EMSCRIPTEN__
#include "../src/def/peg_defs.h"
#include <emscripten/heap.h>
#include <malloc.h>
#include <stddef.h>
#include <stdlib.h>
#include <unistd.h>

// Sample only while the engine is idle. These describe this WASM allocator,
// not available system RAM or a guarantee that one contiguous block fits.
size_t wasm_memory_allocated_bytes(void) { return mallinfo().uordblks; }

size_t wasm_memory_free_bytes(void) {
  const struct mallinfo info = mallinfo();
  return emscripten_get_heap_size() - (size_t)sbrk(0) + info.fordblks;
}

// Conservative launch headroom, separate from the engine's TT budget. This
// allows the browser allocator to grow for a requested search, without probing
// to exhaustion or assuming that the configured maximum is actually available.
// The shared reserve covers the UI's 32 MiB maximum TT budget plus the helper
// and shared search state. Each worker also needs endgame move lists,
// arenas, and helper contexts. A 16 MiB scratch allowance admitted four
// workers at a 512 MiB heap limit but could still exhaust that heap during
// a one-tile-in-bag solve; reserve 64 MiB per worker for this state.
enum {
  WASM_PEG_SHARED_RESERVE_BYTES = 64 * 1024 * 1024,
  WASM_PEG_WORKER_SCRATCH_BYTES = 64 * 1024 * 1024,
  WASM_MAX_SEARCH_THREADS = 32,
  WASM_PEG_SCRATCH_PROBE_BYTES = 8 * 1024 * 1024,
  WASM_PEG_MAX_RESERVATIONS =
      WASM_PEG_SHARED_RESERVE_BYTES / WASM_PEG_SCRATCH_PROBE_BYTES +
      WASM_MAX_SEARCH_THREADS *
          (WASM_PEG_WORKER_SCRATCH_BYTES / WASM_PEG_SCRATCH_PROBE_BYTES + 1),
};

int wasm_peg_memory_fits(int workers) {
  if (workers < 1 || workers > WASM_MAX_SEARCH_THREADS) {
    return 0;
  }
  // The single-thread path runs inline without creating a PEG worker pool.
  const size_t stacks =
      workers > 1 ? (size_t)workers * PEG_POOL_WORKER_STACK_BYTES : 0;
  const size_t required = WASM_PEG_SHARED_RESERVE_BYTES + stacks +
                          ((size_t)workers * WASM_PEG_WORKER_SCRATCH_BYTES);
  const size_t available = emscripten_get_heap_max() -
                           emscripten_get_heap_size() +
                           wasm_memory_free_bytes();
  if (required > available) {
    return 0;
  }
  // Probe separate, simultaneously held blocks like the solver's per-worker
  // stacks and scratch allocations. A single combined allocation can reject
  // ample reusable memory fragmented by earlier simulations or map builds.
  // ABORTING_MALLOC=0 lets refused growth return NULL. Release every block,
  // including after a failed probe, before trying a smaller worker count.
  // Scratch is many smaller allocations (including ~5 MiB move lists), not
  // one 64 MiB buffer per worker. Keep the full reserve but probe it in chunks.
  void *reservations[WASM_PEG_MAX_RESERVATIONS];
  const int stack_count = workers > 1 ? workers : 0;
  const int count =
      stack_count + (int)((required - stacks) / WASM_PEG_SCRATCH_PROBE_BYTES);
  int reserved = 0;
  for (; reserved < count; reserved++) {
    const size_t bytes = reserved < stack_count ? PEG_POOL_WORKER_STACK_BYTES
                                                : WASM_PEG_SCRATCH_PROBE_BYTES;
    reservations[reserved] = malloc(bytes);
    if (!reservations[reserved]) {
      break;
    }
  }
  const int fits = reserved == count;
  for (int index = 0; index < reserved; index++) {
    free(reservations[index]);
  }
  return fits;
}
#endif
