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
  // ABORTING_MALLOC=0 lets a refused growth return NULL. Free the probe before
  // the solver starts so its real allocations can reuse the space. This is a
  // conservative estimate, not a guarantee against later memory pressure.
  void *volatile reservation = malloc(required);
  if (!reservation) {
    return 0;
  }
  free(reservation);
  return 1;
}

#endif
