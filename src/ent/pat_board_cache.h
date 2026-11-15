#ifndef PAT_BOARD_CACHE_H
#define PAT_BOARD_CACHE_H

#include "../def/equity_defs.h"
#include "../util/io_util.h"
#include "pat_eval.h"
#include <stdint.h>
#include <stdlib.h>

// What board-only PAT remembers between move generations. Board-only PAT is
// the term a sim's rollouts use by default (see the patrolloutfast option):
// it leaves out the mover's rack and the utility correction, so a position's
// context depends on the board, the opponent's rack size and the classes in
// use, and a move's term on those and the move. Rollouts reach the same
// boards again and again, and with a cache each is loaded once.
//
// A cache belongs to one thread. The sim worker that owns it gives it to the
// players of its game (see player_set_pat_board_cache), and a player with a
// cache is what makes a move generation use board-only PAT.

enum {
  // A context is about 32 KB, so the contexts take about 8 MB a thread.
  // Sims gained 1 to 2% from 16 times as many.
  PAT_BOARD_CACHE_CONTEXT_BITS = 8,
  PAT_BOARD_CACHE_NUM_CONTEXTS = 1 << PAT_BOARD_CACHE_CONTEXT_BITS,
  PAT_BOARD_CACHE_TERM_BITS = 14,
  PAT_BOARD_CACHE_NUM_TERMS = 1 << PAT_BOARD_CACHE_TERM_BITS,
};

// The multipliers the keys are mixed with.
#define PAT_BOARD_CACHE_KEY_MULTIPLIER UINT64_C(0x9E3779B97F4A7C15)
#define PAT_BOARD_CACHE_KEY_PRIME UINT64_C(0x100000001B3)

// A key is never 0, so a zeroed entry holds nothing.
typedef struct PATBoardCacheContext {
  uint64_t key;
  PATEvalContext ctx;
} PATBoardCacheContext;

typedef struct PATBoardCacheTerm {
  uint64_t key;
  Equity term;
} PATBoardCacheTerm;

typedef struct PATBoardCache {
  // Mixed into every key, and changed by a reset: entries from before it
  // then match nothing.
  uint64_t salt;
  PATBoardCacheContext *contexts;
  PATBoardCacheTerm *terms;
} PATBoardCache;

static inline PATBoardCache *pat_board_cache_create(void) {
  PATBoardCache *cache = malloc_or_die(sizeof(PATBoardCache));
  cache->salt = 1;
  cache->contexts = (PATBoardCacheContext *)calloc_or_die(
      PAT_BOARD_CACHE_NUM_CONTEXTS, sizeof(PATBoardCacheContext));
  cache->terms = (PATBoardCacheTerm *)calloc_or_die(PAT_BOARD_CACHE_NUM_TERMS,
                                                    sizeof(PATBoardCacheTerm));
  return cache;
}

static inline void pat_board_cache_destroy(PATBoardCache *cache) {
  if (!cache) {
    return;
  }
  free(cache->contexts);
  free(cache->terms);
  free(cache);
}

// Forgets everything, for a new sim: its lexicon, weights and layout may
// differ from the last one's, which the keys do not hold.
static inline void pat_board_cache_reset(PATBoardCache *cache) {
  cache->salt++;
}

#endif
