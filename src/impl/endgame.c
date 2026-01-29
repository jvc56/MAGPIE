#include "endgame.h"

#include "../compat/cpthread.h"
#include "../compat/ctime.h"
#include "../def/cpthread_defs.h"
#include "../def/game_defs.h"
#include "../def/kwg_defs.h"
#include "../def/move_defs.h"
#include "../ent/bag.h"
#include "../ent/board.h"
#include "../ent/dictionary_word.h"
#include "../ent/endgame_results.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/game_history.h"
#include "../ent/kwg.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/player.h"
#include "../ent/rack.h"
#include "../ent/small_move_arena.h"
#include "../ent/thread_control.h"
#include "../ent/transposition_table.h"
#include "../ent/xoshiro.h"
#include "../ent/zobrist.h"
#include "../str/move_string.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "gameplay.h"
#include "kwg_maker.h"
#include "move_gen.h"
#include "word_prune.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

enum {
  DEFAULT_ENDGAME_MOVELIST_CAPACITY = 250000,
  // Bit flags for move estimates. These large numbers will force these
  // estimated values to sort first.
  LARGE_VALUE = 1 << 30, // for alpha-beta pruning
  EARLY_PASS_BF = 1 << 29,
  HASH_MOVE_BF = 1 << 28,
  GOING_OUT_BF = 1 << 27,
  // YBWC constants
  MIN_SPLIT_DEPTH = 99,        // Don't split at shallow depths (TEMP: disabled for debugging)
  MAX_SPLIT_DEPTH = 8,        // Maximum nested split depth per thread
  MAX_MOVES_PER_SPLIT = 64,   // Maximum moves to store in a split-point
};

// Forward declarations for YBWC
typedef struct SplitPoint SplitPoint;
typedef struct YBWCThreadPool YBWCThreadPool;
typedef struct EndgameSolverWorker EndgameSolverWorker;

// Forward declaration of negamax for helper functions
static int32_t negamax(EndgameSolverWorker *worker, uint64_t node_key, int depth,
                       int32_t alpha, int32_t beta, PVLine *pv, bool pv_node);

// SplitPoint: represents a node where work can be distributed to helper threads
struct SplitPoint {
  // Tree position
  uint64_t node_key;               // Zobrist hash for this position
  int depth;                       // Remaining depth to search

  // Alpha-beta bounds (alpha is atomic for concurrent updates)
  _Atomic int32_t alpha;           // Current best bound (updated atomically)
  int32_t beta;                    // Upper bound (constant for this split)
  int32_t best_value;              // Best value found so far
  uint64_t best_tiny_move;         // Best move found
  int32_t alpha_orig;              // Original alpha for TT flag determination

  // Move distribution
  SmallMove moves[MAX_MOVES_PER_SPLIT]; // Moves at this node
  int num_moves;                   // Total moves
  _Atomic int next_move_index;     // Next move to be claimed (atomic)

  // Game state for helpers to sync to
  Game *game_state;                // Game state at this split-point

  // Thread coordination
  _Atomic int active_helpers;      // Number of threads working here
  cpthread_mutex_t mutex;          // Protects non-atomic result updates
  cpthread_cond_t complete_cond;   // Signal when all helpers done

  // Context
  EndgameSolverWorker *owner;      // Thread that created this split-point
  bool pv_node;                    // Whether this is a PV node
  int on_turn_spread;              // Spread at this position

  // Result
  PVLine pv;                       // Best PV found at this split-point
};

// YBWCThreadPool: manages worker threads and work distribution
struct YBWCThreadPool {
  int num_threads;
  EndgameSolverWorker **workers;
  cpthread_t *thread_ids;

  // Split-point pool
  SplitPoint **split_points;       // Array of pointers to active split-points
  _Atomic int num_split_points;    // Number of active split-points
  int max_split_points;            // Capacity

  // Thread synchronization
  cpthread_mutex_t pool_mutex;     // Protects split-point array
  cpthread_cond_t work_available;  // Signal when split-point created

  // Global state
  _Atomic bool stop_search;        // Early termination flag
  _Atomic int idle_threads;        // Number of threads waiting for work
};

struct EndgameSolver {
  int initial_spread;
  int solving_player;
  int n_initial_moves;

  int initial_small_move_arena_size;
  bool iterative_deepening_optim;
  bool first_win_optim;
  bool transposition_table_optim;
  bool negascout_optim;
  bool prevent_slowroll;
  PVLine principal_variation;
  PVLine *variations;

  KWG *pruned_kwg;
  atomic_int nodes_searched;       // Atomic for thread-safe counting

  int solve_multiple_variations;
  int32_t best_pv_value;
  int requested_plies;
  int threads;
  double tt_fraction_of_mem;
  TranspositionTable *transposition_table;

  // YBWC thread pool
  YBWCThreadPool *thread_pool;

  // Owned by the caller:
  ThreadControl *thread_control;
  const Game *game;
};

struct EndgameSolverWorker {
  int thread_index;
  Game *game_copy;
  Arena *small_move_arena;
  MoveList *move_list;
  EndgameSolver *solver;
  int current_iterative_deepening_depth;

  // YBWC additions
  YBWCThreadPool *thread_pool;           // Reference to thread pool
  SplitPoint *current_split_point;       // Split-point we're helping (NULL if owner)
  XoshiroPRNG *prng;                     // Per-thread PRNG for move ordering jitter

  // Per-thread results
  PVLine best_pv;                        // Thread-local best PV
  int32_t best_pv_value;                 // Thread-local best value
  int completed_depth;                   // Depth this thread completed

  // Split-point ownership tracking
  int num_owned_splits;                  // Number of splits this thread owns
};

#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

void pvline_clear(PVLine *pv_line) {
  pv_line->num_moves = 0;
  pv_line->score = 0;
}

void pvline_update(PVLine *pv_line, const PVLine *new_pv_line,
                   const SmallMove *move, int32_t score) {
  pvline_clear(pv_line);
  pv_line->moves[0].metadata = move->metadata;
  pv_line->moves[0].tiny_move = move->tiny_move;
  for (int i = 0; i < new_pv_line->num_moves; i++) {
    assert(i != MAX_VARIANT_LENGTH - 1);
    pv_line->moves[i + 1].metadata = new_pv_line->moves[i].metadata;
    pv_line->moves[i + 1].tiny_move = new_pv_line->moves[i].tiny_move;
  }
  pv_line->num_moves = new_pv_line->num_moves + 1;
  pv_line->score = score;
}

void endgame_solver_reset(EndgameSolver *es, const EndgameArgs *endgame_args) {
  es->first_win_optim = false;
  // Disable TT with multiple threads until we make it thread-safe
  es->transposition_table_optim = (endgame_args->num_threads <= 1);
  es->iterative_deepening_optim = true;
  es->negascout_optim = true;
  es->solve_multiple_variations = 0;
  atomic_store(&es->nodes_searched, 0);
  es->threads = endgame_args->num_threads;
  if (es->threads < 1) {
    es->threads = 1;
  }
  es->requested_plies = endgame_args->plies;
  es->solving_player = game_get_player_on_turn_index(endgame_args->game);
  es->initial_small_move_arena_size =
      endgame_args->initial_small_move_arena_size;
  const Player *player =
      game_get_player(endgame_args->game, es->solving_player);
  const Player *opponent =
      game_get_player(endgame_args->game, 1 - es->solving_player);

  es->initial_spread =
      equity_to_int(player_get_score(player) - player_get_score(opponent));

  kwg_destroy(es->pruned_kwg);
  DictionaryWordList *possible_word_list = dictionary_word_list_create();
  generate_possible_words(endgame_args->game, NULL, possible_word_list);
  es->pruned_kwg = make_kwg_from_words(
      possible_word_list, KWG_MAKER_OUTPUT_GADDAG, KWG_MAKER_MERGE_EXACT);
  dictionary_word_list_destroy(possible_word_list);

  es->thread_control = endgame_args->thread_control;
  es->game = endgame_args->game;
  if (endgame_args->tt_fraction_of_mem == 0) {
    transposition_table_destroy(es->transposition_table);
    es->transposition_table = NULL;
  } else if (es->tt_fraction_of_mem != endgame_args->tt_fraction_of_mem) {
    transposition_table_destroy(es->transposition_table);
    es->transposition_table =
        transposition_table_create(endgame_args->tt_fraction_of_mem);
  }
  es->tt_fraction_of_mem = endgame_args->tt_fraction_of_mem;
}

EndgameSolver *endgame_solver_create(void) {
  return calloc_or_die(1, sizeof(EndgameSolver));
}

void endgame_solver_destroy(EndgameSolver *es) {
  if (!es) {
    return;
  }
  transposition_table_destroy(es->transposition_table);
  kwg_destroy(es->pruned_kwg);
  free(es);
}

EndgameSolverWorker *endgame_solver_create_worker(EndgameSolver *solver,
                                                  int worker_index,
                                                  uint64_t base_seed) {

  EndgameSolverWorker *solver_worker =
      malloc_or_die(sizeof(EndgameSolverWorker));

  solver_worker->thread_index = worker_index;
  solver_worker->game_copy = game_duplicate(solver->game);
  game_set_endgame_solving_mode(solver_worker->game_copy);
  game_set_backup_mode(solver_worker->game_copy, BACKUP_MODE_SIMULATION);
  solver_worker->move_list =
      move_list_create_small(DEFAULT_ENDGAME_MOVELIST_CAPACITY);

  solver_worker->small_move_arena =
      create_arena(solver->initial_small_move_arena_size, 16);

  solver_worker->solver = solver;

  // YBWC additions
  solver_worker->thread_pool = solver->thread_pool;
  solver_worker->current_split_point = NULL;

  // Initialize per-thread PRNG with unique seed for jitter
  solver_worker->prng = prng_create(base_seed + (uint64_t)worker_index * 12345);

  // Initialize per-thread result tracking
  solver_worker->best_pv.num_moves = 0;
  solver_worker->best_pv_value = -LARGE_VALUE;
  solver_worker->completed_depth = 0;
  solver_worker->num_owned_splits = 0;

  return solver_worker;
}

void solver_worker_destroy(EndgameSolverWorker *solver_worker) {
  if (!solver_worker) {
    return;
  }
  game_destroy(solver_worker->game_copy);
  small_move_list_destroy(solver_worker->move_list);
  arena_destroy(solver_worker->small_move_arena);
  prng_destroy(solver_worker->prng);
  free(solver_worker);
}

// ============================================================================
// YBWC Thread Pool and Split-Point Management
// ============================================================================

YBWCThreadPool *ybwc_pool_create(int num_threads) {
  YBWCThreadPool *pool = malloc_or_die(sizeof(YBWCThreadPool));
  pool->num_threads = num_threads;
  pool->workers = malloc_or_die(sizeof(EndgameSolverWorker *) * num_threads);
  pool->thread_ids = malloc_or_die(sizeof(cpthread_t) * num_threads);

  // Split-point pool: capacity = num_threads * 4
  pool->max_split_points = num_threads * 4;
  pool->split_points =
      malloc_or_die(sizeof(SplitPoint *) * pool->max_split_points);
  atomic_store(&pool->num_split_points, 0);

  // Synchronization
  cpthread_mutex_init(&pool->pool_mutex);
  cpthread_cond_init(&pool->work_available);

  atomic_store(&pool->stop_search, false);
  atomic_store(&pool->idle_threads, 0);

  return pool;
}

void ybwc_pool_destroy(YBWCThreadPool *pool) {
  if (!pool) {
    return;
  }
  free(pool->split_points);
  free(pool->workers);
  free(pool->thread_ids);
  free(pool);
}

SplitPoint *split_point_create(EndgameSolverWorker *owner, uint64_t node_key,
                               int depth, int32_t alpha, int32_t beta,
                               int32_t best_value, uint64_t best_tiny_move,
                               bool pv_node, int on_turn_spread,
                               int32_t alpha_orig) {
  SplitPoint *sp = malloc_or_die(sizeof(SplitPoint));

  sp->node_key = node_key;
  sp->depth = depth;
  // Alpha should reflect improvement from first move (if any)
  atomic_store(&sp->alpha, best_value > alpha ? best_value : alpha);
  sp->beta = beta;
  sp->best_value = best_value;
  sp->best_tiny_move = best_tiny_move;
  sp->alpha_orig = alpha_orig;

  sp->num_moves = 0;
  atomic_store(&sp->next_move_index, 0);

  // Duplicate game state for helpers to sync to
  sp->game_state = game_duplicate(owner->game_copy);

  atomic_store(&sp->active_helpers, 1); // Owner is always active
  cpthread_mutex_init(&sp->mutex);
  cpthread_cond_init(&sp->complete_cond);

  sp->owner = owner;
  sp->pv_node = pv_node;
  sp->on_turn_spread = on_turn_spread;

  sp->pv.num_moves = 0;
  sp->pv.score = 0;

  return sp;
}

void split_point_destroy(SplitPoint *sp) {
  if (!sp) {
    return;
  }
  game_destroy(sp->game_state);
  free(sp);
}

void split_point_add_move(SplitPoint *sp, const SmallMove *move) {
  if (sp->num_moves < MAX_MOVES_PER_SPLIT) {
    sp->moves[sp->num_moves++] = *move;
  }
}

// Add split-point to pool and wake up waiting threads
void add_split_point_to_pool(YBWCThreadPool *pool, SplitPoint *sp) {
  cpthread_mutex_lock(&pool->pool_mutex);
  int idx = atomic_load(&pool->num_split_points);
  if (idx < pool->max_split_points) {
    pool->split_points[idx] = sp;
    atomic_store(&pool->num_split_points, idx + 1);
    // Wake up all idle threads
    cpthread_cond_broadcast(&pool->work_available);
  }
  cpthread_mutex_unlock(&pool->pool_mutex);
}

// Remove split-point from pool
void remove_split_point_from_pool(YBWCThreadPool *pool, SplitPoint *sp) {
  cpthread_mutex_lock(&pool->pool_mutex);
  int n = atomic_load(&pool->num_split_points);
  for (int i = 0; i < n; i++) {
    if (pool->split_points[i] == sp) {
      // Shift remaining elements
      for (int j = i; j < n - 1; j++) {
        pool->split_points[j] = pool->split_points[j + 1];
      }
      atomic_store(&pool->num_split_points, n - 1);
      break;
    }
  }
  cpthread_mutex_unlock(&pool->pool_mutex);
}

// Find a split-point to help with (prefer deeper splits for more cutoffs)
SplitPoint *find_split_point_to_help(YBWCThreadPool *pool,
                                     EndgameSolverWorker *worker) {
  cpthread_mutex_lock(&pool->pool_mutex);

  SplitPoint *best_sp = NULL;
  int best_depth = -1;

  int n = atomic_load(&pool->num_split_points);
  for (int i = 0; i < n; i++) {
    SplitPoint *sp = pool->split_points[i];
    // Don't help your own split-point (you're already the owner)
    if (sp->owner == worker) {
      continue;
    }
    // Check if there's work remaining
    int next_idx = atomic_load(&sp->next_move_index);
    if (next_idx >= sp->num_moves) {
      continue;
    }
    // Check for cutoff
    if (atomic_load(&sp->alpha) >= sp->beta) {
      continue;
    }
    // Prefer deeper splits
    if (sp->depth > best_depth) {
      best_depth = sp->depth;
      best_sp = sp;
    }
  }

  // Must increment active_helpers while holding lock to prevent race with owner
  if (best_sp) {
    atomic_fetch_add(&best_sp->active_helpers, 1);
  }

  cpthread_mutex_unlock(&pool->pool_mutex);
  return best_sp;
}

// Update split-point results atomically
void update_split_point_results(SplitPoint *sp, const SmallMove *move,
                                int32_t value, const PVLine *child_pv) {
  cpthread_mutex_lock(&sp->mutex);

  if (value > sp->best_value) {
    sp->best_value = value;
    sp->best_tiny_move = move->tiny_move;
    pvline_update(&sp->pv, child_pv, move, value);

    // Update alpha if improved (using compare-exchange for atomicity)
    int32_t old_alpha = atomic_load(&sp->alpha);
    while (value > old_alpha) {
      if (atomic_compare_exchange_weak(&sp->alpha, &old_alpha, value)) {
        break;
      }
    }
  }

  cpthread_mutex_unlock(&sp->mutex);
}

// Check if we should split at this node
bool should_split(EndgameSolverWorker *worker, int depth, int num_moves) {
  YBWCThreadPool *pool = worker->thread_pool;
  if (!pool || pool->num_threads <= 1) {
    return false;
  }

  // Don't split too close to leaves (overhead > benefit)
  if (depth < MIN_SPLIT_DEPTH) {
    return false;
  }

  // Need at least 2 moves remaining (first is searched sequentially)
  if (num_moves < 2) {
    return false;
  }

  // Only split if there are idle threads waiting
  if (atomic_load(&pool->idle_threads) == 0) {
    return false;
  }

  // Limit nesting depth
  if (worker->num_owned_splits >= MAX_SPLIT_DEPTH) {
    return false;
  }

  return true;
}

// Help process moves at a split-point
void help_at_split_point(EndgameSolverWorker *worker, SplitPoint *sp) {
  // Create a temporary game copy from the split-point's game state
  // We need a fresh copy since we'll be playing/unplaying moves
  Game *saved_game = worker->game_copy;
  worker->game_copy = game_duplicate(sp->game_state);
  game_set_endgame_solving_mode(worker->game_copy);
  game_set_backup_mode(worker->game_copy, BACKUP_MODE_SIMULATION);

  while (true) {
    // Check for cutoff
    int32_t current_alpha = atomic_load(&sp->alpha);
    if (current_alpha >= sp->beta) {
      break;
    }

    // Claim next move atomically
    int idx = atomic_fetch_add(&sp->next_move_index, 1);
    if (idx >= sp->num_moves) {
      break;
    }

    SmallMove *move = &sp->moves[idx];

    // Convert small move to full move for play
    small_move_to_move(worker->move_list->spare_move, move,
                       game_get_board(worker->game_copy));

    const int on_turn_idx = game_get_player_on_turn_index(worker->game_copy);
    const Player *player_on_turn =
        game_get_player(worker->game_copy, on_turn_idx);
    const Rack *stm_rack = player_get_rack(player_on_turn);
    int last_consecutive_scoreless_turns =
        game_get_consecutive_scoreless_turns(worker->game_copy);

    play_move(worker->move_list->spare_move, worker->game_copy, NULL);

    // Atomic increment for thread-safe node counting
    atomic_fetch_add(&worker->solver->nodes_searched, 1);

    // Calculate child key for TT
    uint64_t child_key = 0;
    if (worker->solver->transposition_table_optim) {
      child_key = zobrist_add_move(
          worker->solver->transposition_table->zobrist, sp->node_key,
          worker->move_list->spare_move, stm_rack,
          on_turn_idx == worker->solver->solving_player,
          game_get_consecutive_scoreless_turns(worker->game_copy),
          last_consecutive_scoreless_turns);
    }

    PVLine child_pv;
    child_pv.game = worker->game_copy;
    child_pv.num_moves = 0;

    // Re-read alpha for the search
    current_alpha = atomic_load(&sp->alpha);

    // Full-window search (temporary - for debugging)
    int32_t value =
        negamax(worker, child_key, sp->depth - 1, -sp->beta,
                -current_alpha, &child_pv, sp->pv_node);

    game_unplay_last_move(worker->game_copy);

    // Update split-point results
    update_split_point_results(sp, move, -value, &child_pv);
  }

  // Restore the worker's original game copy
  game_destroy(worker->game_copy);
  worker->game_copy = saved_game;
}

// Wait for all helpers at a split-point to complete
void wait_for_split_complete(SplitPoint *sp) {
  cpthread_mutex_lock(&sp->mutex);
  while (atomic_load(&sp->active_helpers) > 1) {
    // Only wait if other helpers are still working
    cpthread_cond_wait(&sp->complete_cond, &sp->mutex);
  }
  cpthread_mutex_unlock(&sp->mutex);
}

// Helper thread main loop - waits for work and processes split-points
void *helper_thread_main(void *arg) {
  EndgameSolverWorker *worker = (EndgameSolverWorker *)arg;
  YBWCThreadPool *pool = worker->thread_pool;

  while (!atomic_load(&pool->stop_search)) {
    // Try to find a split-point to help with
    SplitPoint *sp = find_split_point_to_help(pool, worker);

    if (sp == NULL) {
      // No work available, wait for notification
      cpthread_mutex_lock(&pool->pool_mutex);
      atomic_fetch_add(&pool->idle_threads, 1);

      // Wait until work is available or search stops
      while (!atomic_load(&pool->stop_search) &&
             atomic_load(&pool->num_split_points) == 0) {
        cpthread_cond_wait(&pool->work_available, &pool->pool_mutex);
      }

      atomic_fetch_sub(&pool->idle_threads, 1);
      cpthread_mutex_unlock(&pool->pool_mutex);
      continue;
    }

    // Join the split-point (active_helpers already incremented in find_split_point_to_help)
    worker->current_split_point = sp;

    // Help process moves
    help_at_split_point(worker, sp);

    // Leave the split-point
    worker->current_split_point = NULL;

    // Must hold mutex while decrementing to prevent race with owner's wait
    cpthread_mutex_lock(&sp->mutex);
    int remaining = atomic_fetch_sub(&sp->active_helpers, 1) - 1;
    if (remaining == 1) {
      // Only owner remains - signal completion
      cpthread_cond_signal(&sp->complete_cond);
    }
    cpthread_mutex_unlock(&sp->mutex);
  }

  return NULL;
}

int generate_stm_plays(EndgameSolverWorker *worker) {
  // stm means side to move
  // This won't actually sort by score. We'll do this later.
  const MoveGenArgs args = {
      .game = worker->game_copy,
      .move_list = worker->move_list,
      .move_record_type = MOVE_RECORD_ALL_SMALL,
      .move_sort_type = MOVE_SORT_SCORE,
      .override_kwg = worker->solver->pruned_kwg,
      .thread_index = worker->thread_index,
      .eq_margin_movegen = 0,
  };
  generate_moves(&args);
  SmallMove *arena_small_moves = (SmallMove *)arena_alloc(
      worker->small_move_arena, worker->move_list->count * sizeof(SmallMove));
  for (int i = 0; i < worker->move_list->count; i++) {
    // Copy by value
    arena_small_moves[i] = *(worker->move_list->small_moves[i]);
  }
  // Now that the arena has these, we can deal with the move list directly
  // in the arena.
  return worker->move_list->count;
}

void assign_estimates_and_sort(EndgameSolverWorker *worker, int depth,
                               int move_count, uint64_t tt_move) {
  // assign estimates to arena plays.
  // log_warn("assigning estimates; depth %d, arena_begin %d, move_count %d",
  //          depth, worker->small_move_arena->size, move_count);
  const int player_index = game_get_player_on_turn_index(worker->game_copy);
  const Player *player = game_get_player(worker->game_copy, player_index);
  const Player *opponent = game_get_player(worker->game_copy, 1 - player_index);
  const Rack *stm_rack = player_get_rack(player);
  const Rack *other_rack = player_get_rack(opponent);
  int ntiles_on_rack = stm_rack->number_of_letters;
  bool last_move_was_pass = game_get_consecutive_scoreless_turns(
                                worker->game_copy) == 1; // check if this is ok.

  size_t arena_offset =
      worker->small_move_arena->size - (sizeof(SmallMove) * move_count);

  for (size_t i = 0; i < (size_t)move_count; i++) {
    size_t element_offset = arena_offset + i * sizeof(SmallMove);
    SmallMove *current_move =
        (SmallMove *)(worker->small_move_arena->memory + element_offset);
    // log_warn("assign estimate to move idx %d, tm %x, meta %x", i,
    //          current_move->tiny_move, current_move->metadata);
    if (small_move_get_tiles_played(current_move) == ntiles_on_rack) {
      small_move_set_estimated_value(
          current_move,
          equity_to_int(int_to_equity(small_move_get_score(current_move)) +
                        (calculate_end_rack_points(
                            other_rack, game_get_ld(worker->game_copy)))) |
              GOING_OUT_BF);
    } else if (depth > 2) {
      // some more jitter for lazysmp (to be implemented)
      if (worker->thread_index >= 6) {
        small_move_set_estimated_value(
            current_move, small_move_get_score(current_move) +
                              3 * small_move_get_tiles_played(current_move));
      } else {
        small_move_set_estimated_value(
            current_move, small_move_get_score(current_move) -
                              5 * small_move_get_tiles_played(current_move));
      }
    } else {
      small_move_set_estimated_value(current_move,
                                     small_move_get_score(current_move));
    }

    if (current_move->tiny_move == tt_move) {
      small_move_add_estimated_value(current_move, HASH_MOVE_BF);
    }

    // Consider pass first if we've just passed. This will allow us to see that
    // branch of the tree faster.
    if (last_move_was_pass && small_move_is_pass(current_move)) {
      small_move_add_estimated_value(current_move, EARLY_PASS_BF);
    }
  }
  // sort moves by estimated value, from biggest to smallest value. A good move
  // sorting is instrumental to the performance of ab pruning.
  SmallMove *small_moves =
      (SmallMove *)(worker->small_move_arena->memory + arena_offset);
  qsort(small_moves, move_count, sizeof(SmallMove),
        compare_small_moves_by_estimated_value);
}

// XXX: Move this debug helper to a utility function or something.
char *create_spaces(int depth) {
  // Allocate memory for the string of spaces (+1 for the null terminator)
  char *spaces = (char *)malloc_or_die(depth + 1);

  // Fill the string with spaces
  for (int i = 0; i < depth; i++) {
    spaces[i] = ' ';
  }

  // Null-terminate the string
  spaces[depth] = '\0';

  return spaces;
}

// void print_small_plays(EndgameSolverWorker *worker, int nplays,
//                        int cur_move_loc) {}

static int32_t negamax(EndgameSolverWorker *worker, uint64_t node_key, int depth,
                       int32_t alpha, int32_t beta, PVLine *pv, bool pv_node) {

  assert(pv_node || alpha == beta - 1);
  int32_t alpha_orig = alpha;

  int on_turn_idx = game_get_player_on_turn_index(worker->game_copy);
  const Player *player_on_turn =
      game_get_player(worker->game_copy, on_turn_idx);
  const Player *other_player =
      game_get_player(worker->game_copy, 1 - on_turn_idx);
  int on_turn_spread = equity_to_int(player_get_score(player_on_turn) -
                                     player_get_score(other_player));
  uint64_t tt_move = INVALID_TINY_MOVE;

  if (worker->solver->transposition_table_optim) {
    TTEntry tt_entry = transposition_table_lookup(
        worker->solver->transposition_table, node_key);
    if (ttentry_valid(tt_entry) && ttentry_depth(tt_entry) >= (uint8_t)depth) {
      int16_t score = ttentry_score(tt_entry);
      uint8_t flag = ttentry_flag(tt_entry);
      // add spread back in; we subtract it when storing.
      score = (int16_t)((int16_t)on_turn_spread + score);
      if (flag == TT_EXACT) {
        if (!pv_node) {
          return score;
        }
      } else if (flag == TT_LOWER) {
        alpha = MAX(alpha, score);
      } else if (flag == TT_UPPER) {
        beta = MIN(beta, score);
      }
      if (alpha >= beta) {
        if (!pv_node) {
          // don't cut-off PV node
          return score;
        }
      }
      // search hash move first
      tt_move = ttentry_move(tt_entry);
    }
  }

  if (depth == 0 ||
      game_get_game_end_reason(worker->game_copy) != GAME_END_REASON_NONE) {
    // This assumes the player turn changed even though the game was already
    // over, which appears to be the case in the code.
    return on_turn_spread;
  }

  PVLine child_pv;
  child_pv.game = worker->game_copy;
  child_pv.num_moves = 0;

  int nplays;
  // char *spaces = create_spaces(worker->solver->requested_plies - depth);
  bool arena_alloced = false;
  if (worker->current_iterative_deepening_depth != depth) {
    nplays = generate_stm_plays(worker);
    assign_estimates_and_sort(worker, depth, nplays, tt_move);
    // log_warn("generated and allocated; nplays %d, cur_size %ld", nplays,
    //          worker->small_move_arena->size);
    arena_alloced = true;
  } else {
    // Use initial moves. They already have been sorted by estimated value.
    nplays = worker->solver->n_initial_moves;
  }
  // print_small_plays(worker, nplays, cur_move_loc);

  int32_t best_value = -LARGE_VALUE;
  uint64_t best_tiny_move = INVALID_TINY_MOVE;
  size_t arena_offset =
      worker->small_move_arena->size - (sizeof(SmallMove) * nplays);

  // YBWC: Search first move sequentially (eldest brother)
  if (nplays > 0) {
    size_t element_offset = arena_offset;
    SmallMove *small_move =
        (SmallMove *)(worker->small_move_arena->memory + element_offset);
    small_move_to_move(worker->move_list->spare_move, small_move,
                       game_get_board(worker->game_copy));

    const Rack *stm_rack = player_get_rack(player_on_turn);

    int last_consecutive_scoreless_turns =
        game_get_consecutive_scoreless_turns(worker->game_copy);
    play_move(worker->move_list->spare_move, worker->game_copy, NULL);

    // Atomic increment for thread-safe node counting
    atomic_fetch_add(&worker->solver->nodes_searched, 1);

    uint64_t child_key = 0;
    if (worker->solver->transposition_table_optim) {
      child_key = zobrist_add_move(
          worker->solver->transposition_table->zobrist, node_key,
          worker->move_list->spare_move, stm_rack,
          on_turn_idx == worker->solver->solving_player,
          game_get_consecutive_scoreless_turns(worker->game_copy),
          last_consecutive_scoreless_turns);
    }

    int32_t value = negamax(worker, child_key, depth - 1, -beta, -alpha,
                            &child_pv, pv_node);
    game_unplay_last_move(worker->game_copy);

    // Re-assign small_move after potential reallocation
    small_move =
        (SmallMove *)(worker->small_move_arena->memory + element_offset);
    if (-value > best_value) {
      best_value = -value;
      best_tiny_move = small_move->tiny_move;
      pvline_update(pv, &child_pv, small_move,
                    best_value - worker->solver->initial_spread);
    }
    if (worker->current_iterative_deepening_depth == depth) {
      small_move_set_estimated_value(small_move, -value);
    }
    alpha = MAX(alpha, best_value);
    pvline_clear(&child_pv);
  }

  // YBWC: After first move, check if we should split for remaining moves
  if (nplays > 1 && best_value < beta &&
      should_split(worker, depth, nplays - 1)) {
    // Create split-point for remaining moves
    SplitPoint *sp =
        split_point_create(worker, node_key, depth, alpha, beta, best_value,
                           best_tiny_move, pv_node, on_turn_spread, alpha_orig);

    // Copy remaining moves to split-point
    for (int idx = 1; idx < nplays && idx <= MAX_MOVES_PER_SPLIT; idx++) {
      size_t element_offset = arena_offset + idx * sizeof(SmallMove);
      SmallMove *small_move =
          (SmallMove *)(worker->small_move_arena->memory + element_offset);
      split_point_add_move(sp, small_move);
    }
    sp->pv = *pv; // Copy current best PV

    // Start claiming from first remaining move
    atomic_store(&sp->next_move_index, 0);

    // Add to pool and wake helpers
    worker->num_owned_splits++;
    add_split_point_to_pool(worker->thread_pool, sp);

    // Owner also helps process the split-point
    help_at_split_point(worker, sp);

    // Wait for all helpers to complete
    wait_for_split_complete(sp);

    // Remove from pool
    remove_split_point_from_pool(worker->thread_pool, sp);
    worker->num_owned_splits--;

    // Collect results
    best_value = sp->best_value;
    best_tiny_move = sp->best_tiny_move;
    *pv = sp->pv;

    // Update estimated values for iterative deepening (can't easily do this
    // for parallel search results, so skip)

    split_point_destroy(sp);
  } else if (nplays > 1 && best_value < beta) {
    // Sequential search of remaining moves (no split possible)
    for (int idx = 1; idx < nplays; idx++) {
      size_t element_offset = arena_offset + idx * sizeof(SmallMove);
      SmallMove *small_move =
          (SmallMove *)(worker->small_move_arena->memory + element_offset);
      small_move_to_move(worker->move_list->spare_move, small_move,
                         game_get_board(worker->game_copy));

      const Rack *stm_rack = player_get_rack(player_on_turn);

      int last_consecutive_scoreless_turns =
          game_get_consecutive_scoreless_turns(worker->game_copy);
      play_move(worker->move_list->spare_move, worker->game_copy, NULL);

      // Atomic increment for thread-safe node counting
      atomic_fetch_add(&worker->solver->nodes_searched, 1);

      uint64_t child_key = 0;
      if (worker->solver->transposition_table_optim) {
        child_key = zobrist_add_move(
            worker->solver->transposition_table->zobrist, node_key,
            worker->move_list->spare_move, stm_rack,
            on_turn_idx == worker->solver->solving_player,
            game_get_consecutive_scoreless_turns(worker->game_copy),
            last_consecutive_scoreless_turns);
      }

      int32_t value = 0;
      if (!worker->solver->negascout_optim) {
        value = negamax(worker, child_key, depth - 1, -beta, -alpha, &child_pv,
                        pv_node);
      } else {
        value = negamax(worker, child_key, depth - 1, -alpha - 1, -alpha,
                        &child_pv, false);
        if (alpha < -value && -value < beta) {
          // re-search with wider window
          value = negamax(worker, child_key, depth - 1, -beta, -alpha,
                          &child_pv, pv_node);
        }
      }
      game_unplay_last_move(worker->game_copy);

      // Re-assign small_move after potential reallocation
      small_move =
          (SmallMove *)(worker->small_move_arena->memory + element_offset);
      if (-value > best_value) {
        best_value = -value;
        best_tiny_move = small_move->tiny_move;
        pvline_update(pv, &child_pv, small_move,
                      best_value - worker->solver->initial_spread);
      }
      if (worker->current_iterative_deepening_depth == depth) {
        small_move_set_estimated_value(small_move, -value);
      }
      alpha = MAX(alpha, best_value);
      if (best_value >= beta) {
        break;
      }
      pvline_clear(&child_pv);
    }
  }

  if (worker->solver->transposition_table_optim) {
    int16_t score = (int16_t)(best_value - on_turn_spread);
    uint8_t flag;
    TTEntry entry_to_store = {.score = score};
    if (best_value <= alpha_orig) {
      flag = TT_UPPER;
    } else if (best_value >= beta) {
      flag = TT_LOWER;
    } else {
      flag = TT_EXACT;
    }
    entry_to_store.flag_and_depth = (flag << 6) + (uint8_t)depth;
    entry_to_store.tiny_move = best_tiny_move;
    transposition_table_store(worker->solver->transposition_table, node_key,
                              entry_to_store);
  }

  if (arena_alloced) {
    arena_dealloc(worker->small_move_arena, nplays * sizeof(SmallMove));
    // log_warn("arena_dealloced; new size: %ld",
    // worker->small_move_arena->size); printf("%sReset back to %ld\n", spaces,
    // worker->small_move_arena->size);
  }
  // free(spaces);
  return best_value;
}

void iterative_deepening(EndgameSolverWorker *worker, int plies) {

  int32_t alpha = -LARGE_VALUE;
  int32_t beta = LARGE_VALUE;

  if (worker->solver->first_win_optim) {
    // search a very small window centered around 0; we're just trying to find
    // something that surpasses it. This is useful to find "any win", and for
    // a pre-endgame solver.
    alpha = -1;
    beta = 1;
  }
  assert(worker->small_move_arena->size == 0); // make sure arena is empty.

  uint64_t initial_hash_key = 0;
  if (worker->solver->transposition_table_optim) {
    const Player *solving_player =
        game_get_player(worker->game_copy, worker->solver->solving_player);
    const Player *other_player =
        game_get_player(worker->game_copy, 1 - worker->solver->solving_player);
    initial_hash_key = zobrist_calculate_hash(
        worker->solver->transposition_table->zobrist,
        game_get_board(worker->game_copy), player_get_rack(solving_player),
        player_get_rack(other_player), false,
        game_get_consecutive_scoreless_turns(worker->game_copy));
  }

  int initial_move_count = generate_stm_plays(worker);
  // Arena pointer better have started at 0, since it was empty.
  assign_estimates_and_sort(worker, 0, initial_move_count, INVALID_TINY_MOVE);
  worker->solver->n_initial_moves = initial_move_count;
  assert((size_t)worker->small_move_arena->size ==
         initial_move_count * sizeof(SmallMove));
  // log_warn("Generated %d initial moves; pointer is at %d",
  // initial_move_count,
  //          worker->small_move_arena->size);
  // SmallMove last_winner;

  worker->current_iterative_deepening_depth = 1;
  int start = 1;
  if (!worker->solver->iterative_deepening_optim) {
    start = plies;
  }

  for (int p = start; p <= plies; p++) {
    worker->current_iterative_deepening_depth = p;
    PVLine pv;
    pv.game = worker->game_copy;
    pv.num_moves = 0;
    int32_t val = negamax(worker, initial_hash_key, p, alpha, beta, &pv, true);
    printf("  depth %d: value=%d, pv=", p, val - worker->solver->initial_spread);
    // Print the full PV with scores
    Game *pv_game = game_duplicate(worker->game_copy);
    const LetterDistribution *ld = game_get_ld(pv_game);
    StringBuilder *pv_sb = string_builder_create();
    Move pv_move;
    for (int i = 0; i < pv.num_moves; i++) {
      if (i > 0) {
        string_builder_add_string(pv_sb, " ");
      }
      small_move_to_move(&pv_move, &pv.moves[i], game_get_board(pv_game));
      string_builder_add_move(pv_sb, game_get_board(pv_game), &pv_move, ld,
                              true);
      play_move(&pv_move, pv_game, NULL);
    }
    printf("%s\n", string_builder_peek(pv_sb));
    string_builder_destroy(pv_sb);
    game_destroy(pv_game);
    // sort initial moves by valuation for next time.
    SmallMove *initial_moves = (SmallMove *)(worker->small_move_arena->memory);
    qsort(initial_moves, initial_move_count, sizeof(SmallMove),
          compare_small_moves_by_estimated_value);

    // log_warn("val returned was %d, initial spread %d", val,
    //          worker->solver->initial_spread);
    worker->solver->best_pv_value = val - worker->solver->initial_spread;
    worker->solver->principal_variation = pv;
  }
}

void *solver_worker_start(void *uncasted_solver_worker) {
  EndgameSolverWorker *solver_worker =
      (EndgameSolverWorker *)uncasted_solver_worker;
  const EndgameSolver *solver = solver_worker->solver;
  // ThreadControl *thread_control = solver->thread_control;
  // later allow thread control to quit early.
  iterative_deepening(solver_worker, solver->requested_plies);
  return NULL;
}

void string_builder_endgame_results(StringBuilder *pv_description,
                                    const EndgameResults *results,
                                    const Game *game,
                                    const GameHistory *game_history,
                                    bool add_line_breaks) {
  const PVLine *pv_line = endgame_results_get_pvline(results);
  Move move;
  string_builder_add_formatted_string(
      pv_description, "Principal Variation (value: %d, length: %d)%c",
      pv_line->score, pv_line->num_moves, add_line_breaks ? '\n' : ' ');

  Game *game_copy = game_duplicate(game);
  const Board *board = game_get_board(game_copy);
  const LetterDistribution *ld = game_get_ld(game_copy);

  if (add_line_breaks) {
    StringGrid *sg = string_grid_create(pv_line->num_moves, 3, 1);
    StringBuilder *tmp_sb = string_builder_create();
    for (int i = 0; i < pv_line->num_moves; i++) {
      int curr_col = 0;
      // Set the player name
      string_grid_set_cell(
          sg, i, curr_col++,
          get_formatted_string(
              "(%s)",
              game_history_player_get_name(
                  game_history, game_get_player_on_turn_index(game_copy))));

      // Set the play sequence index and player name
      string_grid_set_cell(sg, i, curr_col++,
                           get_formatted_string("%d:", i + 1));

      // Set the move string
      small_move_to_move(&move, &(pv_line->moves[i]), board);
      string_builder_add_move(tmp_sb, board, &move, ld, true);
      string_grid_set_cell(sg, i, curr_col++,
                           string_builder_dump(tmp_sb, NULL));
      string_builder_clear(tmp_sb);
      // Play the move on the board to make the next small_move_to_move make
      // sense.
      play_move(&move, game_copy, NULL);
    }
    string_builder_add_string_grid(pv_description, sg, false);
    string_grid_destroy(sg);
  } else {
    for (int i = 0; i < pv_line->num_moves; i++) {
      string_builder_add_formatted_string(pv_description, "%d: ", i + 1);
      small_move_to_move(&move, &(pv_line->moves[i]), board);
      string_builder_add_move(pv_description, board, &move, ld, true);
      if (i != pv_line->num_moves - 1) {
        string_builder_add_string(pv_description, " -> ");
      }
      // Play the move on the board to make the next small_move_to_move make
      // sense.
      play_move(&move, game_copy, NULL);
    }
  }
  string_builder_add_string(pv_description, "\n");
  game_destroy(game_copy);
}

char *endgame_results_get_string(const EndgameResults *results,
                                 const Game *game,
                                 const GameHistory *game_history,
                                 bool add_line_breaks) {
  StringBuilder *pv_description = string_builder_create();
  string_builder_endgame_results(pv_description, results, game, game_history,
                                 add_line_breaks);
  char *pvline_string = string_builder_dump(pv_description, NULL);
  string_builder_destroy(pv_description);
  return pvline_string;
}

void endgame_solve(EndgameSolver *solver, const EndgameArgs *endgame_args,
                   EndgameResults *results, ErrorStack *error_stack) {
  const int bag_size = bag_get_letters(game_get_bag(endgame_args->game));
  if (bag_size != 0) {
    error_stack_push(error_stack, ERROR_STATUS_ENDGAME_BAG_NOT_EMPTY,
                     get_formatted_string(
                         "bag must be empty to solve an endgame, but have %d "
                         "letters in the bag",
                         bag_size));
    return;
  }

  endgame_solver_reset(solver, endgame_args);

  // Generate base seed for YBWC using current time
  uint64_t base_seed = (uint64_t)ctime_get_current_time();

  // Create YBWC thread pool if multi-threaded
  YBWCThreadPool *thread_pool = NULL;
  if (solver->threads > 1) {
    thread_pool = ybwc_pool_create(solver->threads);
    solver->thread_pool = thread_pool;
  } else {
    solver->thread_pool = NULL;
  }

  // Create workers
  EndgameSolverWorker **solver_workers =
      malloc_or_die((sizeof(EndgameSolverWorker *)) * solver->threads);

  for (int thread_index = 0; thread_index < solver->threads; thread_index++) {
    solver_workers[thread_index] =
        endgame_solver_create_worker(solver, thread_index, base_seed);
    if (thread_pool) {
      thread_pool->workers[thread_index] = solver_workers[thread_index];
    }
  }

  if (solver->threads > 1) {
    // YBWC mode: master thread runs iterative deepening, helpers wait for work
    // Start helper threads first
    for (int thread_index = 1; thread_index < solver->threads; thread_index++) {
      cpthread_create(&thread_pool->thread_ids[thread_index],
                      helper_thread_main, solver_workers[thread_index]);
    }

    // Master thread (index 0) runs iterative deepening
    iterative_deepening(solver_workers[0], solver->requested_plies);

    // Store master's result
    solver_workers[0]->best_pv = solver->principal_variation;
    solver_workers[0]->best_pv_value = solver->best_pv_value;
    solver_workers[0]->completed_depth = solver->requested_plies;

    // Signal helpers to stop and wait for them
    atomic_store(&thread_pool->stop_search, true);
    cpthread_mutex_lock(&thread_pool->pool_mutex);
    cpthread_cond_broadcast(&thread_pool->work_available);
    cpthread_mutex_unlock(&thread_pool->pool_mutex);

    for (int thread_index = 1; thread_index < solver->threads; thread_index++) {
      cpthread_join(thread_pool->thread_ids[thread_index]);
    }
  } else {
    // Single-threaded mode: just run iterative deepening
    iterative_deepening(solver_workers[0], solver->requested_plies);
  }

  // Clean up workers
  for (int thread_index = 0; thread_index < solver->threads; thread_index++) {
    solver_worker_destroy(solver_workers[thread_index]);
  }
  free(solver_workers);

  // Clean up thread pool
  if (thread_pool) {
    ybwc_pool_destroy(thread_pool);
    solver->thread_pool = NULL;
  }

  endgame_results_set_pvline(results, &solver->principal_variation);
}