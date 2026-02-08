#include "endgame.h"

#include "../compat/cpthread.h"
#include "../compat/ctime.h"
#include "../def/cpthread_defs.h"
#include "../def/equity_defs.h"
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
#include "../ent/move_undo.h"
#include "../ent/player.h"
#include "../ent/rack.h"
#include "../ent/small_move_arena.h"
#include "../ent/thread_control.h"
#include "../ent/transposition_table.h"
#include "../ent/xoshiro.h"
#include "../ent/zobrist.h"
#include "../str/move_string.h"
#include "../str/rack_string.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "gameplay.h"
#include "kwg_maker.h"
#include "move_gen.h"
#include "word_prune.h"
#include <assert.h>
#include <math.h>
#ifndef __wasm__
#include <sched.h>
#endif
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
  DEFAULT_ENDGAME_MOVELIST_CAPACITY = 250000,
  // Maximum moves for stack allocation in ABDADA deferred tracking
  // Keep small to avoid stack overflow in deep recursive searches
  MAX_DEFERRED_STACK = 64,
  // Bit flags for move estimates. These large numbers will force these
  // estimated values to sort first.
  LARGE_VALUE = 1 << 30, // for alpha-beta pruning
  EARLY_PASS_BF = 1 << 29,
  HASH_MOVE_BF = 1 << 28,
  GOING_OUT_BF = 1 << 27,
  KILLER_MOVE_BF = 1 << 26,
  // ABDADA: sentinel value returned when node is being searched by another
  // processor
  ON_EVALUATION = -(1 << 29),
  // Aspiration window initial size
  ASPIRATION_WINDOW = 25,
  // Killer table size: 2^20 entries (1M) x 16 bytes = 16MB per table
  KILLER_TABLE_SIZE_POWER = 20,
};

// Solitaire pre-solve killer table for move ordering
typedef struct {
  uint64_t key;
  uint64_t tiny_move;
} KillerEntry;

typedef struct {
  KillerEntry *entries;
  uint32_t mask;
} KillerTable;

static KillerTable *killer_table_create(int size_power) {
  KillerTable *kt = malloc_or_die(sizeof(KillerTable));
  uint32_t size = (uint32_t)1 << size_power;
  kt->mask = size - 1;
  kt->entries = calloc_or_die(size, sizeof(KillerEntry));
  return kt;
}

static void killer_table_destroy(KillerTable *kt) {
  if (!kt) {
    return;
  }
  free(kt->entries);
  free(kt);
}

static inline void killer_table_store(KillerTable *kt, uint64_t key,
                                      uint64_t tiny_move) {
  uint32_t idx = (uint32_t)(key & kt->mask);
  kt->entries[idx].key = key;
  kt->entries[idx].tiny_move = tiny_move;
}

static inline uint64_t killer_table_lookup(const KillerTable *kt,
                                           uint64_t key) {
  uint32_t idx = (uint32_t)(key & kt->mask);
  if (kt->entries[idx].key == key) {
    return kt->entries[idx].tiny_move;
  }
  return INVALID_TINY_MOVE;
}

// Shadow zobrist: XOR only board tile positions from a move.
// No rack, turn, or scoreless turn hashing. This creates a hash that is
// consistent between solitaire search (one player) and the real two-sided
// search, because it only tracks one side's tile placements.
static inline uint64_t shadow_zobrist_add_move(const Zobrist *z,
                                                uint64_t shadow_key,
                                                const Move *move) {
  if (move->move_type == GAME_EVENT_PASS) {
    return shadow_key;
  }
  int row = move_get_row_start(move);
  int col = move_get_col_start(move);
  bool vertical = move_get_dir(move) == BOARD_VERTICAL_DIRECTION;

  for (int idx = 0; idx < move->tiles_length; idx++) {
    int tile = move->tiles[idx];
    int new_row = row + (vertical ? idx : 0);
    int new_col = col + (vertical ? 0 : idx);
    if (tile == PLAYED_THROUGH_MARKER) {
      continue;
    }
    int board_tile = tile;
    if (get_is_blanked(tile)) {
      board_tile = get_unblanked_machine_letter(tile) + ZOBRIST_MAX_LETTERS;
    }
    shadow_key ^= z->pos_table[new_row * BOARD_DIM + new_col][board_tile];
  }
  return shadow_key;
}

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
  atomic_int nodes_searched;

  int solve_multiple_variations;
  int32_t best_pv_value;
  int requested_plies;
  int threads;
  double tt_fraction_of_mem;
  TranspositionTable *transposition_table;
  KillerTable *killer_table[2];

  // Signal for threads to stop early (0=running, 1=done)
  atomic_int search_complete;

  // Per-ply callback for iterative deepening progress
  EndgamePerPlyCallback per_ply_callback;
  void *per_ply_callback_data;

  // Owned by the caller:
  ThreadControl *thread_control;
  const Game *game;
};

typedef struct EndgameSolverWorker {
  int thread_index;
  Game *game_copy;
  Arena *small_move_arena;
  MoveList *move_list;
  EndgameSolver *solver;
  int current_iterative_deepening_depth;
  // Array of MoveUndo structures for incremental play/unplay
  MoveUndo move_undos[MAX_SEARCH_DEPTH];
  XoshiroPRNG *prng;     // Per-thread PRNG for jitter
  // MoveList *playout_move_list;  // For greedy playout (thread 0 only)
  // MoveUndo playout_undos[RACK_SIZE * 2]; // Undo slots for playout moves
  PVLine best_pv;        // Thread-local best PV
  int32_t best_pv_value; // Thread-local best value
  int completed_depth;   // Depth this thread completed
} EndgameSolverWorker;

#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

static void pvline_clear(PVLine *pv_line) {
  pv_line->num_moves = 0;
  pv_line->score = 0;
}

static void pvline_update(PVLine *pv_line, const PVLine *new_pv_line,
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

// Extend the PV by probing the transposition table for moves beyond
// what the search PV already contains. Existing PV moves (which have
// correct scores) are preserved; only new moves are appended.
static void pvline_extend_from_tt(PVLine *pv_line, Game *game_copy,
                                  TranspositionTable *tt, int solving_player,
                                  int max_depth) {
  if (!tt) {
    return;
  }

  MoveList *move_list =
      move_list_create_small(DEFAULT_ENDGAME_MOVELIST_CAPACITY);

  // Play existing PV moves to advance game state to where extension begins.
  // These moves already have correct scores in their metadata.
  for (int i = 0; i < pv_line->num_moves; i++) {
    if (game_get_game_end_reason(game_copy) != GAME_END_REASON_NONE) {
      small_move_list_destroy(move_list);
      return;
    }
    small_move_to_move(move_list->spare_move, &pv_line->moves[i],
                       game_get_board(game_copy));
    play_move(move_list->spare_move, game_copy, NULL);
  }

  // Extend PV from TT
  int num_moves = pv_line->num_moves;

  while (num_moves < max_depth &&
         game_get_game_end_reason(game_copy) == GAME_END_REASON_NONE) {
    int on_turn = game_get_player_on_turn_index(game_copy);
    const Player *solving = game_get_player(game_copy, solving_player);
    const Player *other = game_get_player(game_copy, 1 - solving_player);

    uint64_t hash = zobrist_calculate_hash(
        tt->zobrist, game_get_board(game_copy), player_get_rack(solving),
        player_get_rack(other), on_turn != solving_player,
        game_get_consecutive_scoreless_turns(game_copy));

    TTEntry tt_entry = transposition_table_lookup(tt, hash);
    if (!ttentry_valid(tt_entry)) {
      break;
    }

    uint64_t tiny_move = ttentry_move(tt_entry);
    if (tiny_move == INVALID_TINY_MOVE) {
      break;
    }

    // Generate legal moves to find the correct score for this tiny_move.
    // This is only called for display, so the cost doesn't matter.
    const MoveGenArgs args = {
        .game = game_copy,
        .move_list = move_list,
        .move_record_type = MOVE_RECORD_ALL_SMALL,
        .move_sort_type = MOVE_SORT_SCORE,
        .override_kwg = NULL,
        .thread_index = 0,
        .eq_margin_movegen = 0,
        .target_equity = EQUITY_MAX_VALUE,
        .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
    };
    generate_moves(&args);

    uint16_t move_score = 0;
    for (int i = 0; i < move_list->count; i++) {
      if (move_list->small_moves[i]->tiny_move == tiny_move) {
        move_score = small_move_get_score(move_list->small_moves[i]);
        break;
      }
    }

    SmallMove sm;
    sm.tiny_move = tiny_move;
    sm.metadata = (uint64_t)move_score;
    small_move_to_move(move_list->spare_move, &sm, game_get_board(game_copy));
    play_move(move_list->spare_move, game_copy, NULL);

    pv_line->moves[num_moves].tiny_move = tiny_move;
    pv_line->moves[num_moves].metadata = (uint64_t)move_score;
    num_moves++;
  }

  pv_line->num_moves = num_moves;
  small_move_list_destroy(move_list);
}

void endgame_solver_reset(EndgameSolver *es, const EndgameArgs *endgame_args) {
  es->first_win_optim = false;
  es->transposition_table_optim = true;
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
  es->pruned_kwg = make_kwg_from_words_small(
      possible_word_list, KWG_MAKER_OUTPUT_GADDAG, KWG_MAKER_MERGE_EXACT);
  dictionary_word_list_destroy(possible_word_list);

  // Initialize Lazy SMP synchronization
  atomic_store(&es->search_complete, 0);

  es->thread_control = endgame_args->thread_control;
  es->game = endgame_args->game;
  es->per_ply_callback = endgame_args->per_ply_callback;
  es->per_ply_callback_data = endgame_args->per_ply_callback_data;
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
  EndgameSolver *es = calloc_or_die(1, sizeof(EndgameSolver));
  return es;
}

void endgame_solver_destroy(EndgameSolver *es) {
  if (!es) {
    return;
  }
  transposition_table_destroy(es->transposition_table);
  killer_table_destroy(es->killer_table[0]);
  killer_table_destroy(es->killer_table[1]);
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
  // Zero-initialize move_undos to prevent undefined behavior from stale values
  memset(solver_worker->move_undos, 0, sizeof(solver_worker->move_undos));

  // // Thread 0 gets a playout move list for greedy playout ordering
  // if (worker_index == 0) {
  //   solver_worker->playout_move_list = move_list_create(1024);
  // } else {
  //   solver_worker->playout_move_list = NULL;
  // }

  // Initialize per-thread PRNG with unique seed for jitter
  // Each thread gets a different seed based on base_seed + thread_index
  solver_worker->prng = prng_create(base_seed + (uint64_t)worker_index * 12345);

  // Initialize per-thread result tracking
  solver_worker->best_pv.game = solver_worker->game_copy;
  solver_worker->best_pv.num_moves = 0;
  solver_worker->best_pv_value = -LARGE_VALUE;
  solver_worker->completed_depth = 0;

  return solver_worker;
}

void solver_worker_destroy(EndgameSolverWorker *solver_worker) {
  if (!solver_worker) {
    return;
  }
  game_destroy(solver_worker->game_copy);
  small_move_list_destroy(solver_worker->move_list);
  // if (solver_worker->playout_move_list) {
  //   move_list_destroy(solver_worker->playout_move_list);
  // }
  arena_destroy(solver_worker->small_move_arena);
  prng_destroy(solver_worker->prng);
  free(solver_worker);
}

int generate_stm_plays(EndgameSolverWorker *worker, int depth) {
  // stm means side to move
  // Lazy cross-set generation: only compute if not already valid
  Board *board = game_get_board(worker->game_copy);
  if (!board_get_cross_sets_valid(board)) {
    // Get the parent's MoveUndo (the one that made cross-sets invalid)
    // The parent undo contains info about the move that was played to reach
    // this state
    int undo_index = worker->solver->requested_plies - depth - 1;
    if (undo_index >= 0) {
      const MoveUndo *parent_undo = &worker->move_undos[undo_index];
      if (parent_undo->move_tiles_length > 0) {
        // Update cross-sets using the undo-based function
        update_cross_set_for_move_from_undo(parent_undo, worker->game_copy);
      }
    }
    board_set_cross_sets_valid(board, true);
  }
  // This won't actually sort by score. We'll do this later.
  const MoveGenArgs args = {
      .game = worker->game_copy,
      .move_list = worker->move_list,
      .move_record_type = MOVE_RECORD_ALL_SMALL,
      .move_sort_type = MOVE_SORT_SCORE,
      .override_kwg = worker->solver->pruned_kwg,
      .thread_index = worker->thread_index,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
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

/*
// Greedy playout: play best-scoring moves to end-of-game, return final spread
// from the perspective of the player who was on turn at the start.
// The candidate move has already been played on game_copy before calling this.
// If verbose_sb is non-NULL, append playout move descriptions to it.
static int32_t greedy_playout(EndgameSolverWorker *worker, int stm_index,
                              StringBuilder *verbose_sb) {
  Game *game = worker->game_copy;
  MoveList *ml = worker->playout_move_list;
  int playout_depth = 0;

  while (game_get_game_end_reason(game) == GAME_END_REASON_NONE &&
         playout_depth < RACK_SIZE * 2) {
    // Update cross-sets if needed (previous move may have invalidated them)
    Board *board = game_get_board(game);
    if (!board_get_cross_sets_valid(board)) {
      if (playout_depth > 0) {
        const MoveUndo *prev_undo =
            &worker->playout_undos[playout_depth - 1];
        if (prev_undo->move_tiles_length > 0) {
          update_cross_set_for_move_from_undo(prev_undo, game);
        }
      }
      board_set_cross_sets_valid(board, true);
    }

    // Generate best move by equity
    move_list_reset(ml);
    const MoveGenArgs args = {
        .game = game,
        .move_list = ml,
        .move_record_type = MOVE_RECORD_BEST,
        .move_sort_type = MOVE_SORT_EQUITY,
        .override_kwg = worker->solver->pruned_kwg,
        .thread_index = 0,
        .eq_margin_movegen = 0,
        .target_equity = EQUITY_MAX_VALUE,
        .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
    };
    generate_moves(&args);

    if (ml->count == 0) {
      break;
    }

    Move *best = ml->moves[0];

    if (verbose_sb) {
      string_builder_add_string(verbose_sb, " ");
      string_builder_add_move(verbose_sb, board, best,
                              game_get_ld(game), true);
    }

    play_move_incremental(best, game, &worker->playout_undos[playout_depth]);
    playout_depth++;
  }

  // Compute final spread from stm_index's perspective
  int32_t final_spread =
      equity_to_int(player_get_score(game_get_player(game, stm_index))) -
      equity_to_int(player_get_score(game_get_player(game, 1 - stm_index)));

  // Unplay all playout moves in reverse
  for (int d = playout_depth - 1; d >= 0; d--) {
    unplay_move_incremental(game, &worker->playout_undos[d]);
  }

  return final_spread;
}
*/

void assign_estimates_and_sort(EndgameSolverWorker *worker, int depth,
                               int move_count, uint64_t tt_move,
                               uint64_t node_key, uint64_t shadow_key) {
  (void)node_key;
  (void)depth;
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

  // Lazy SMP jitter: different threads use different move ordering heuristics
  // to explore different parts of the search tree first
  const int thread_idx = worker->thread_index;
  const int num_threads = worker->solver->threads;

  for (size_t i = 0; i < (size_t)move_count; i++) {
    size_t element_offset = arena_offset + i * sizeof(SmallMove);
    SmallMove *current_move =
        (SmallMove *)(worker->small_move_arena->memory + element_offset);
    if (small_move_get_tiles_played(current_move) == ntiles_on_rack) {
      small_move_set_estimated_value(
          current_move,
          equity_to_int(int_to_equity(small_move_get_score(current_move)) +
                        (calculate_end_rack_points(
                            other_rack, game_get_ld(worker->game_copy)))) |
              GOING_OUT_BF);
    } else if (num_threads > 1) {
      // Lazy SMP jitter for move ordering at all depths
      // Different threads prioritize different move characteristics
      int base_estimate = small_move_get_score(current_move);
      int tiles_played = small_move_get_tiles_played(current_move);

      // Add thread-specific jitter to move estimates
      // Thread 0: pure score (no jitter - the "main" thread)
      // Odd threads: favor more tiles played
      // Even threads (except 0): favor fewer tiles played
      // All non-zero threads also get small random jitter
      if (thread_idx > 0) {
        if (thread_idx % 2 == 1) {
          base_estimate += 3 * tiles_played;
        } else {
          base_estimate -= 3 * tiles_played;
        }
        // Add small random jitter (0-7) to break ties differently
        base_estimate += (int)(prng_get_random_number(worker->prng, 8));
      }
      small_move_set_estimated_value(current_move, base_estimate);
    } else {
      small_move_set_estimated_value(current_move,
                                     small_move_get_score(current_move));
    }

    if (current_move->tiny_move == tt_move) {
      small_move_add_estimated_value(current_move, HASH_MOVE_BF);
    }

    // Killer move from solitaire pre-solve
    KillerTable *kt = worker->solver->killer_table[player_index];
    if (kt) {
      uint64_t killer = killer_table_lookup(kt, shadow_key);
      if (current_move->tiny_move == killer) {
        small_move_add_estimated_value(current_move, KILLER_MOVE_BF);
      }
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

// Minimax solitaire: our player searches top-N moves (maximizing), opponent
// searches top-N moves (minimizing). Returns spread from our player's
// perspective, including end-of-game adjustments.
static int32_t greedy_search(EndgameSolverWorker *worker, int our_player,
                             int depth, int max_depth, uint64_t shadow_key,
                             KillerTable *kt, int *node_count, PVLine *pv) {
  pvline_clear(pv);

  int on_turn = game_get_player_on_turn_index(worker->game_copy);
  const Player *our = game_get_player(worker->game_copy, our_player);
  const Player *opp = game_get_player(worker->game_copy, 1 - our_player);

  if (depth == 0 ||
      game_get_game_end_reason(worker->game_copy) != GAME_END_REASON_NONE) {
    // Spread from our player's perspective with end-of-game rack adjustment
    int32_t our_score = equity_to_int(player_get_score(our));
    int32_t opp_score = equity_to_int(player_get_score(opp));
    int32_t spread = our_score - opp_score;
    // If game hasn't ended, add 2x unplayed tile penalty for both sides
    if (game_get_game_end_reason(worker->game_copy) == GAME_END_REASON_NONE) {
      const LetterDistribution *ld = game_get_ld(worker->game_copy);
      spread -= equity_to_int(
          calculate_end_rack_points(player_get_rack(our), ld));
      spread += equity_to_int(
          calculate_end_rack_points(player_get_rack(opp), ld));
    }
    return spread;
  }

  (*node_count)++;

  // Handle cross-sets
  Board *board = game_get_board(worker->game_copy);
  if (!board_get_cross_sets_valid(board)) {
    int undo_index = max_depth - depth - 1;
    if (undo_index >= 0) {
      const MoveUndo *parent_undo = &worker->move_undos[undo_index];
      if (parent_undo->move_tiles_length > 0) {
        update_cross_set_for_move_from_undo(parent_undo, worker->game_copy);
      }
    }
    board_set_cross_sets_valid(board, true);
  }

  int undo_index = max_depth - depth;

  // Opponent's turn
  if (on_turn != our_player) {
    const MoveGenArgs args = {
        .game = worker->game_copy,
        .move_list = worker->move_list,
        .move_record_type = MOVE_RECORD_ALL_SMALL,
        .move_sort_type = MOVE_SORT_SCORE,
        .override_kwg = worker->solver->pruned_kwg,
        .thread_index = worker->thread_index,
        .eq_margin_movegen = 0,
        .target_equity = EQUITY_MAX_VALUE,
        .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
    };
    generate_moves(&args);
    int nplays = worker->move_list->count;

    if (nplays == 0) {
      return equity_to_int(player_get_score(our)) -
             equity_to_int(player_get_score(opp));
    }

    // First opponent turn (depth == max_depth - 1): exhaustive search
    // (try all moves, minimize our spread). Later opponent turns: greedy
    // (pick single highest-scoring move).
    if (depth == max_depth - 1) {
      // Exhaustive: copy all moves to arena, search each, minimize
      SmallMove *arena_opp_moves = (SmallMove *)arena_alloc(
          worker->small_move_arena, nplays * sizeof(SmallMove));
      for (int i = 0; i < nplays; i++) {
        arena_opp_moves[i] = *(worker->move_list->small_moves[i]);
      }
      size_t arena_offset =
          worker->small_move_arena->size - (sizeof(SmallMove) * nplays);

      int32_t best_value = LARGE_VALUE; // minimize
      PVLine best_child_pv;

      for (int idx = 0; idx < nplays; idx++) {
        size_t element_offset = arena_offset + idx * sizeof(SmallMove);
        SmallMove *sm =
            (SmallMove *)(worker->small_move_arena->memory + element_offset);

        small_move_to_move(worker->move_list->spare_move, sm,
                           game_get_board(worker->game_copy));

        play_move_incremental(worker->move_list->spare_move, worker->game_copy,
                              &worker->move_undos[undo_index]);

        PVLine child_pv;
        int32_t value =
            greedy_search(worker, our_player, depth - 1, max_depth, shadow_key,
                          kt, node_count, &child_pv);

        unplay_move_incremental(worker->game_copy,
                                &worker->move_undos[undo_index]);

        const MoveUndo *current_undo = &worker->move_undos[undo_index];
        if (current_undo->move_tiles_length > 0) {
          update_cross_sets_after_unplay_from_undo(current_undo,
                                                   worker->game_copy);
          board_set_cross_sets_valid(game_get_board(worker->game_copy), true);
        }

        // Re-assign sm pointer: arena may have grown during recursive call
        sm =
            (SmallMove *)(worker->small_move_arena->memory + element_offset);

        if (value < best_value) {
          best_value = value;
          pvline_update(&best_child_pv, &child_pv, sm, value);
        }
      }

      arena_dealloc(worker->small_move_arena, nplays * sizeof(SmallMove));
      *pv = best_child_pv;
      return best_value;
    }

    // Greedy: find and play best move by score
    int best_score = INT32_MIN;
    int best_idx = 0;
    for (int i = 0; i < nplays; i++) {
      int score = small_move_get_score(worker->move_list->small_moves[i]);
      if (score > best_score) {
        best_score = score;
        best_idx = i;
      }
    }

    SmallMove best_sm = *(worker->move_list->small_moves[best_idx]);
    small_move_to_move(worker->move_list->spare_move, &best_sm,
                       game_get_board(worker->game_copy));

    play_move_incremental(worker->move_list->spare_move, worker->game_copy,
                          &worker->move_undos[undo_index]);

    PVLine child_pv;
    int32_t value = greedy_search(worker, our_player, depth - 1, max_depth,
                                  shadow_key, kt, node_count, &child_pv);

    unplay_move_incremental(worker->game_copy,
                            &worker->move_undos[undo_index]);

    const MoveUndo *current_undo = &worker->move_undos[undo_index];
    if (current_undo->move_tiles_length > 0) {
      update_cross_sets_after_unplay_from_undo(current_undo,
                                               worker->game_copy);
      board_set_cross_sets_valid(game_get_board(worker->game_copy), true);
    }

    pvline_update(pv, &child_pv, &best_sm, value);
    return value;
  }

  // Our player's turn: search multiple moves
  const MoveGenArgs args = {
      .game = worker->game_copy,
      .move_list = worker->move_list,
      .move_record_type = MOVE_RECORD_ALL_SMALL,
      .move_sort_type = MOVE_SORT_SCORE,
      .override_kwg = worker->solver->pruned_kwg,
      .thread_index = worker->thread_index,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
  };
  generate_moves(&args);
  int nplays = worker->move_list->count;

  SmallMove *arena_small_moves = (SmallMove *)arena_alloc(
      worker->small_move_arena, nplays * sizeof(SmallMove));
  for (int i = 0; i < nplays; i++) {
    arena_small_moves[i] = *(worker->move_list->small_moves[i]);
  }

  // Sort by score (going-out moves first)
  size_t arena_offset =
      worker->small_move_arena->size - (sizeof(SmallMove) * nplays);
  const Rack *stm_rack = player_get_rack(our);
  const Rack *other_rack = player_get_rack(opp);
  int ntiles_on_rack = stm_rack->number_of_letters;
  for (int i = 0; i < nplays; i++) {
    size_t element_offset = arena_offset + i * sizeof(SmallMove);
    SmallMove *sm =
        (SmallMove *)(worker->small_move_arena->memory + element_offset);
    if (small_move_get_tiles_played(sm) == ntiles_on_rack) {
      small_move_set_estimated_value(
          sm,
          equity_to_int(int_to_equity(small_move_get_score(sm)) +
                        calculate_end_rack_points(
                            other_rack, game_get_ld(worker->game_copy))) |
              GOING_OUT_BF);
    } else {
      small_move_set_estimated_value(sm, small_move_get_score(sm));
    }
  }
  SmallMove *small_moves =
      (SmallMove *)(worker->small_move_arena->memory + arena_offset);
  qsort(small_moves, nplays, sizeof(SmallMove),
        compare_small_moves_by_estimated_value);

  // Prune non-root nodes to top-K
  const int GREEDY_NON_ROOT_WIDTH = 5;
  if (depth < max_depth && nplays > GREEDY_NON_ROOT_WIDTH) {
    arena_dealloc(worker->small_move_arena,
                  (nplays - GREEDY_NON_ROOT_WIDTH) * sizeof(SmallMove));
    nplays = GREEDY_NON_ROOT_WIDTH;
  }

  // If the only move is a pass, play it and continue (opponent gets to move)
  if (nplays == 1) {
    size_t element_offset = arena_offset;
    SmallMove *sm =
        (SmallMove *)(worker->small_move_arena->memory + element_offset);
    if (small_move_is_pass(sm)) {
      // Check for consecutive passes
      if (game_get_consecutive_scoreless_turns(worker->game_copy) >= 1) {
        arena_dealloc(worker->small_move_arena, nplays * sizeof(SmallMove));
        return equity_to_int(player_get_score(our)) -
               equity_to_int(player_get_score(opp));
      }
    }
  }

  int32_t best_value = -LARGE_VALUE;
  uint64_t best_tiny_move = INVALID_TINY_MOVE;
  PVLine child_pv;

  for (int idx = 0; idx < nplays; idx++) {
    size_t element_offset = arena_offset + idx * sizeof(SmallMove);
    SmallMove *sm =
        (SmallMove *)(worker->small_move_arena->memory + element_offset);

    small_move_to_move(worker->move_list->spare_move, sm,
                       game_get_board(worker->game_copy));

    play_move_incremental(worker->move_list->spare_move, worker->game_copy,
                          &worker->move_undos[undo_index]);

    uint64_t child_shadow = shadow_zobrist_add_move(
        worker->solver->transposition_table->zobrist, shadow_key,
        worker->move_list->spare_move);

    int32_t value = greedy_search(worker, our_player, depth - 1, max_depth,
                                  child_shadow, kt, node_count, &child_pv);

    unplay_move_incremental(worker->game_copy,
                            &worker->move_undos[undo_index]);

    const MoveUndo *current_undo = &worker->move_undos[undo_index];
    if (current_undo->move_tiles_length > 0) {
      update_cross_sets_after_unplay_from_undo(current_undo,
                                               worker->game_copy);
      board_set_cross_sets_valid(game_get_board(worker->game_copy), true);
    }

    // Re-assign sm pointer: arena may have grown during recursive call
    sm = (SmallMove *)(worker->small_move_arena->memory + element_offset);

    if (value > best_value) {
      best_value = value;
      best_tiny_move = sm->tiny_move;
      pvline_update(pv, &child_pv, sm, value);
    }
  }

  if (best_tiny_move != INVALID_TINY_MOVE) {
    killer_table_store(kt, shadow_key, best_tiny_move);
  }

  arena_dealloc(worker->small_move_arena, nplays * sizeof(SmallMove));
  return best_value;
}

// Thread argument for parallel greedy root search
typedef struct {
  EndgameSolverWorker *worker;
  int our_player;
  int max_depth;
  KillerTable *kt;
  const Zobrist *zobrist;
  SmallMove *root_moves; // shared array (read-only)
  int total_moves;
  int thread_id;
  int num_threads;
  // Output
  int32_t best_value;
  PVLine best_pv;
  int node_count;
} GreedyRootArg;

static void *greedy_root_worker(void *arg) {
  GreedyRootArg *gra = (GreedyRootArg *)arg;
  gra->best_value = -LARGE_VALUE;
  gra->node_count = 0;
  pvline_clear(&gra->best_pv);

  for (int idx = gra->thread_id; idx < gra->total_moves;
       idx += gra->num_threads) {
    SmallMove *sm = &gra->root_moves[idx];

    small_move_to_move(gra->worker->move_list->spare_move, sm,
                       game_get_board(gra->worker->game_copy));

    play_move_incremental(gra->worker->move_list->spare_move,
                          gra->worker->game_copy,
                          &gra->worker->move_undos[0]);

    uint64_t child_shadow = shadow_zobrist_add_move(
        gra->zobrist, 0, gra->worker->move_list->spare_move);

    PVLine child_pv;
    int32_t value =
        greedy_search(gra->worker, gra->our_player, gra->max_depth - 1,
                      gra->max_depth, child_shadow, gra->kt,
                      &gra->node_count, &child_pv);

    unplay_move_incremental(gra->worker->game_copy,
                            &gra->worker->move_undos[0]);

    const MoveUndo *undo = &gra->worker->move_undos[0];
    if (undo->move_tiles_length > 0) {
      update_cross_sets_after_unplay_from_undo(undo, gra->worker->game_copy);
      board_set_cross_sets_valid(game_get_board(gra->worker->game_copy), true);
    }

    if (value > gra->best_value) {
      gra->best_value = value;
      pvline_update(&gra->best_pv, &child_pv, sm, value);
    }
  }

  return NULL;
}

// Parallel greedy root search: generates root moves on worker 0, distributes
// across all workers via threads, returns best value and PV.
static int32_t parallel_greedy_root_search(
    EndgameSolver *solver, EndgameSolverWorker **workers, int num_workers,
    int our_player, int max_depth, KillerTable *kt, const Game *base_game,
    int *total_node_count, PVLine *best_pv) {

  // Reset worker 0 and generate root moves
  EndgameSolverWorker *w0 = workers[0];
  game_copy(w0->game_copy, base_game);
  game_set_endgame_solving_mode(w0->game_copy);
  game_set_backup_mode(w0->game_copy, BACKUP_MODE_SIMULATION);
  game_gen_all_cross_sets(w0->game_copy);

  const MoveGenArgs args = {
      .game = w0->game_copy,
      .move_list = w0->move_list,
      .move_record_type = MOVE_RECORD_ALL_SMALL,
      .move_sort_type = MOVE_SORT_SCORE,
      .override_kwg = solver->pruned_kwg,
      .thread_index = 0,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
  };
  generate_moves(&args);
  int nplays = w0->move_list->count;

  if (nplays == 0) {
    pvline_clear(best_pv);
    *total_node_count = 0;
    const Player *our = game_get_player(w0->game_copy, our_player);
    const Player *opp = game_get_player(w0->game_copy, 1 - our_player);
    return equity_to_int(player_get_score(our)) -
           equity_to_int(player_get_score(opp));
  }

  // Copy moves and sort (with going-out bonus)
  SmallMove *root_moves = malloc_or_die(nplays * sizeof(SmallMove));
  for (int i = 0; i < nplays; i++) {
    root_moves[i] = *(w0->move_list->small_moves[i]);
  }

  const Player *our = game_get_player(w0->game_copy, our_player);
  const Player *opp = game_get_player(w0->game_copy, 1 - our_player);
  const Rack *stm_rack = player_get_rack(our);
  const Rack *other_rack = player_get_rack(opp);
  int ntiles_on_rack = stm_rack->number_of_letters;
  for (int i = 0; i < nplays; i++) {
    if (small_move_get_tiles_played(&root_moves[i]) == ntiles_on_rack) {
      small_move_set_estimated_value(
          &root_moves[i],
          equity_to_int(int_to_equity(small_move_get_score(&root_moves[i])) +
                        calculate_end_rack_points(
                            other_rack, game_get_ld(w0->game_copy))) |
              GOING_OUT_BF);
    } else {
      small_move_set_estimated_value(&root_moves[i],
                                     small_move_get_score(&root_moves[i]));
    }
  }
  qsort(root_moves, nplays, sizeof(SmallMove),
        compare_small_moves_by_estimated_value);

  // Handle single pass with consecutive scoreless turns
  if (nplays == 1 && small_move_is_pass(&root_moves[0])) {
    if (game_get_consecutive_scoreless_turns(w0->game_copy) >= 1) {
      pvline_clear(best_pv);
      *total_node_count = 0;
      int32_t val = equity_to_int(player_get_score(our)) -
                    equity_to_int(player_get_score(opp));
      free(root_moves);
      return val;
    }
  }

  // Reset all workers' game copies to base_game
  for (int i = 0; i < num_workers; i++) {
    game_copy(workers[i]->game_copy, base_game);
    game_set_endgame_solving_mode(workers[i]->game_copy);
    game_set_backup_mode(workers[i]->game_copy, BACKUP_MODE_SIMULATION);
    game_gen_all_cross_sets(workers[i]->game_copy);
  }

  const Zobrist *zobrist = solver->transposition_table->zobrist;
  int actual_threads = num_workers < nplays ? num_workers : nplays;
  GreedyRootArg *thread_args =
      malloc_or_die(actual_threads * sizeof(GreedyRootArg));

  for (int t = 0; t < actual_threads; t++) {
    thread_args[t] = (GreedyRootArg){
        .worker = workers[t],
        .our_player = our_player,
        .max_depth = max_depth,
        .kt = kt,
        .zobrist = zobrist,
        .root_moves = root_moves,
        .total_moves = nplays,
        .thread_id = t,
        .num_threads = actual_threads,
    };
    thread_args[t].best_pv.game = workers[t]->game_copy;
  }

  // Spawn threads (or run single-threaded if only 1)
  if (actual_threads == 1) {
    greedy_root_worker(&thread_args[0]);
  } else {
    cpthread_t *tids = malloc_or_die(actual_threads * sizeof(cpthread_t));
    for (int t = 0; t < actual_threads; t++) {
      cpthread_create(&tids[t], greedy_root_worker, &thread_args[t]);
    }
    for (int t = 0; t < actual_threads; t++) {
      cpthread_join(tids[t]);
    }
    free(tids);
  }

  // Collect results
  int32_t overall_best = -LARGE_VALUE;
  int best_thread = 0;
  *total_node_count = 1; // root node
  for (int t = 0; t < actual_threads; t++) {
    *total_node_count += thread_args[t].node_count;
    if (thread_args[t].best_value > overall_best) {
      overall_best = thread_args[t].best_value;
      best_thread = t;
    }
  }
  *best_pv = thread_args[best_thread].best_pv;

  // Store killer for root position
  if (overall_best > -LARGE_VALUE && best_pv->num_moves > 0) {
    killer_table_store(kt, 0, best_pv->moves[0].tiny_move);
  }

  free(root_moves);
  free(thread_args);
  return overall_best;
}

static void print_greedy_pv(const char *label, int player_num, int depth,
                            int32_t display_value, double dt, int node_count,
                            const PVLine *pv, const Game *base_game,
                            const Move *prefix_move,
                            const LetterDistribution *ld,
                            int32_t p1_final_spread) {
  StringBuilder *sb = string_builder_create();
  Game *pv_game = game_duplicate(base_game);

  // Optional prefix move (e.g., p1's opening move for p2 display)
  if (prefix_move) {
    string_builder_add_move(sb, game_get_board(pv_game), prefix_move, ld,
                            true);
    play_move(prefix_move, pv_game, NULL);
  }

  for (int i = 0; i < pv->num_moves; i++) {
    Move move;
    small_move_to_move(&move, &pv->moves[i], game_get_board(pv_game));
    if (i > 0 || prefix_move) {
      string_builder_add_string(sb, " ");
    }
    string_builder_add_move(sb, game_get_board(pv_game), &move, ld, true);
    play_move(&move, pv_game, NULL);
  }
  game_destroy(pv_game);

  // [Px wins by Y] annotation
  char annot[64];
  if (p1_final_spread > 0) {
    snprintf(annot, sizeof(annot), " [P1 wins by %d]", p1_final_spread);
  } else if (p1_final_spread < 0) {
    snprintf(annot, sizeof(annot), " [P2 wins by %d]", -p1_final_spread);
  } else {
    snprintf(annot, sizeof(annot), " [Tie]");
  }

  printf("  %s p%d depth %d: value=%d, time=%.3fs, nodes=%d, pv=%s%s\n",
         label, player_num, depth, display_value, dt, node_count,
         string_builder_peek(sb), annot);
  string_builder_destroy(sb);
}

// Run solitaire pre-solve for both players to populate killer tables
static void run_solitaire_presolve(EndgameSolver *solver) {
  if (!solver->transposition_table) {
    return;
  }

  Timer presolve_timer;
  ctimer_start(&presolve_timer);

  // Create killer tables
  solver->killer_table[0] = killer_table_create(KILLER_TABLE_SIZE_POWER);
  solver->killer_table[1] = killer_table_create(KILLER_TABLE_SIZE_POWER);

  int plies = solver->requested_plies;
  int solving_player = solver->solving_player;
  int other_player = 1 - solving_player;
  int saved_plies = solver->requested_plies;
  const LetterDistribution *ld = game_get_ld(solver->game);
  int32_t initial_spread =
      equity_to_int(player_get_score(
          game_get_player(solver->game, solving_player))) -
      equity_to_int(
          player_get_score(game_get_player(solver->game, other_player)));

  // Create workers for parallel search
  int num_workers = solver->threads;
  EndgameSolverWorker **workers =
      malloc_or_die(num_workers * sizeof(EndgameSolverWorker *));
  for (int i = 0; i < num_workers; i++) {
    workers[i] = endgame_solver_create_worker(solver, i, 0);
  }

  // Greedy search for solving player (full depth)
  PVLine sol_pv;
  sol_pv.game = workers[0]->game_copy;
  for (int d = 1; d <= plies; d++) {
    solver->requested_plies = d;
    int node_count = 0;
    Timer depth_timer;
    ctimer_start(&depth_timer);
    int32_t value = parallel_greedy_root_search(
        solver, workers, num_workers, solving_player, d,
        solver->killer_table[solving_player], solver->game, &node_count,
        &sol_pv);
    double dt = ctimer_elapsed_seconds(&depth_timer);

    int32_t p1_spread = (solving_player == 0) ? value : -value;
    print_greedy_pv("greedy", solving_player + 1, d, value - initial_spread,
                    dt, node_count, &sol_pv, solver->game, NULL, ld,
                    p1_spread);
  }

  // Greedy search for other player, starting from p1's best move
  if (plies > 1 && sol_pv.num_moves > 0) {
    Game *other_game = game_duplicate(solver->game);
    Move p1_move;
    small_move_to_move(&p1_move, &sol_pv.moves[0],
                       game_get_board(other_game));
    play_move(&p1_move, other_game, NULL);

    for (int d = 1; d <= plies - 1; d++) {
      solver->requested_plies = d;
      int node_count = 0;
      PVLine opp_pv;
      opp_pv.game = workers[0]->game_copy;
      Timer depth_timer;
      ctimer_start(&depth_timer);
      int32_t value = parallel_greedy_root_search(
          solver, workers, num_workers, other_player, d,
          solver->killer_table[other_player], other_game, &node_count,
          &opp_pv);
      double dt = ctimer_elapsed_seconds(&depth_timer);

      int32_t p1_spread = (other_player == 0) ? value : -value;
      print_greedy_pv("greedy", other_player + 1, d, value + initial_spread,
                      dt, node_count, &opp_pv, solver->game, &p1_move, ld,
                      p1_spread);
    }
    game_destroy(other_game);
  }

  solver->requested_plies = saved_plies;

  for (int i = 0; i < num_workers; i++) {
    solver_worker_destroy(workers[i]);
  }
  free(workers);

  double elapsed = ctimer_elapsed_seconds(&presolve_timer);
  log_warn("Greedy pre-solve: %.3fs (plies=%d)", elapsed, plies);
}

int32_t abdada_negamax(EndgameSolverWorker *worker, uint64_t node_key,
                       uint64_t shadow_key_0, uint64_t shadow_key_1,
                       int depth, int32_t alpha, int32_t beta, PVLine *pv,
                       bool pv_node, bool exclusive_p) {

  assert(pv_node || alpha == beta - 1);

  // ABDADA: if exclusive search and another processor is on this node, defer.
  // Only use ABDADA exclusion at sufficient depth (>= 3) where the cost of
  // redundant search justifies the protocol overhead. At shallow depths the
  // deferral/retry loop causes more contention than it saves.
  const int num_threads = worker->solver->threads;
  const bool abdada_active = (num_threads > 1 && depth >= 3 &&
                              worker->solver->transposition_table_optim);
  if (exclusive_p && abdada_active) {
    if (transposition_table_is_busy(worker->solver->transposition_table,
                                    node_key)) {
      return ON_EVALUATION;
    }
  }

  // ABDADA: mark that we're searching this node
  if (abdada_active) {
    transposition_table_enter_node(worker->solver->transposition_table,
                                   node_key);
  }

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
          // ABDADA: leave node before returning
          if (abdada_active) {
            transposition_table_leave_node(worker->solver->transposition_table,
                                           node_key);
          }
          return score;
        }
      } else if (flag == TT_LOWER) {
        alpha = MAX(alpha, score);
      } else if (flag == TT_UPPER) {
        beta = MIN(beta, score);
      }
      if (alpha >= beta) {
        if (!pv_node) {
          // ABDADA: leave node before returning
          if (abdada_active) {
            transposition_table_leave_node(worker->solver->transposition_table,
                                           node_key);
          }
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
    // ABDADA: leave node before returning
    if (abdada_active) {
      transposition_table_leave_node(worker->solver->transposition_table,
                                     node_key);
    }
    // This assumes the player turn changed even though the game was already
    // over, which appears to be the case in the code.
    return on_turn_spread;
  }

  PVLine child_pv;
  child_pv.game = worker->game_copy;
  child_pv.num_moves = 0;

  int nplays;
  bool arena_alloced = false;
  if (worker->current_iterative_deepening_depth != depth) {
    nplays = generate_stm_plays(worker, depth);
    uint64_t stm_shadow =
        (on_turn_idx == 0) ? shadow_key_0 : shadow_key_1;
    assign_estimates_and_sort(worker, depth, nplays, tt_move, node_key,
                              stm_shadow);
    arena_alloced = true;
  } else {
    // Use initial moves. They already have been sorted by estimated value.
    nplays = worker->solver->n_initial_moves;
  }

  int32_t best_value = -LARGE_VALUE;
  uint64_t best_tiny_move = INVALID_TINY_MOVE;
  size_t arena_offset =
      worker->small_move_arena->size - (sizeof(SmallMove) * nplays);

  // ABDADA: track deferred moves for second phase
  // Use stack allocation when possible to avoid malloc overhead
  bool deferred_stack[MAX_DEFERRED_STACK];
  bool *deferred = NULL;
  bool deferred_heap_allocated = false;
  bool use_abdada = abdada_active;
  if (use_abdada) {
    if (nplays <= MAX_DEFERRED_STACK) {
      deferred = deferred_stack;
    } else {
      deferred = malloc_or_die(sizeof(bool) * nplays);
      deferred_heap_allocated = true;
    }
    for (int i = 0; i < nplays; i++) {
      deferred[i] = false;
    }
  }

  // ABDADA two-phase iteration
  bool all_done = false;
  while (!all_done) {
    all_done = true;

    for (int idx = 0; idx < nplays; idx++) {
      // ABDADA: determine if this move should be searched exclusively
      // First move (idx == 0) is never exclusive
      // In first phase, other moves are exclusive
      // In second phase (deferred[idx] == true), moves are not exclusive
      bool child_exclusive = use_abdada && (idx > 0) && !deferred[idx];

      // ABDADA: skip if deferred and we're in first phase (will process later)
      // This check is not needed in the current structure since we mark
      // deferred only after ON_EVALUATION

      size_t element_offset = arena_offset + idx * sizeof(SmallMove);
      SmallMove *small_move =
          (SmallMove *)(worker->small_move_arena->memory + element_offset);
      small_move_to_move(worker->move_list->spare_move, small_move,
                         game_get_board(worker->game_copy));

      const Rack *stm_rack = player_get_rack(player_on_turn);
      const int stm_rack_tiles = rack_get_total_letters(stm_rack);
      const bool is_outplay =
          small_move_get_tiles_played(small_move) == stm_rack_tiles;

      int last_consecutive_scoreless_turns =
          game_get_consecutive_scoreless_turns(worker->game_copy);

      // Calculate undo index for incremental backup
      int undo_index = worker->solver->requested_plies - depth;

      // Use optimized function for outplays - skips board/cross-set updates
      if (is_outplay) {
        play_move_endgame_outplay(worker->move_list->spare_move,
                                  worker->game_copy,
                                  &worker->move_undos[undo_index]);
      } else {
        play_move_incremental(worker->move_list->spare_move, worker->game_copy,
                              &worker->move_undos[undo_index]);
        // Cross-sets are left invalid - they will be computed lazily before
        // move generation if we reach that point. The cross-set squares will be
        // saved to MoveUndo before updating, so they're restored on unplay.
      }

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

      // Update shadow keys: only the moving player's shadow changes
      uint64_t child_shadow_0 = shadow_key_0;
      uint64_t child_shadow_1 = shadow_key_1;
      if (worker->solver->transposition_table_optim) {
        const Zobrist *z = worker->solver->transposition_table->zobrist;
        if (on_turn_idx == 0) {
          child_shadow_0 =
              shadow_zobrist_add_move(z, shadow_key_0,
                                     worker->move_list->spare_move);
        } else {
          child_shadow_1 =
              shadow_zobrist_add_move(z, shadow_key_1,
                                     worker->move_list->spare_move);
        }
      }

      int32_t value = 0;
      if (idx == 0 || !worker->solver->negascout_optim) {
        value = abdada_negamax(worker, child_key, child_shadow_0,
                               child_shadow_1, depth - 1, -beta, -alpha,
                               &child_pv, pv_node, child_exclusive);
      } else {
        value = abdada_negamax(worker, child_key, child_shadow_0,
                               child_shadow_1, depth - 1, -alpha - 1, -alpha,
                               &child_pv, false, child_exclusive);
        if (value != ON_EVALUATION && alpha < -value && -value < beta) {
          // re-search with wider window (not exclusive since we need the value)
          value = abdada_negamax(worker, child_key, child_shadow_0,
                                 child_shadow_1, depth - 1, -beta, -alpha,
                                 &child_pv, pv_node, false);
        }
      }
      unplay_move_incremental(worker->game_copy,
                              &worker->move_undos[undo_index]);
      // After unplay, if tiles were placed, cross-sets need to be recomputed
      // for the restored state. Use undo-based function for correct cross-set
      // update. If it was a pass, cross-sets are unchanged and still valid.
      const MoveUndo *current_undo = &worker->move_undos[undo_index];
      if (current_undo->move_tiles_length > 0) {
        update_cross_sets_after_unplay_from_undo(current_undo,
                                                 worker->game_copy);
        board_set_cross_sets_valid(game_get_board(worker->game_copy), true);
      }

      // ABDADA: check if move was deferred
      if (value == ON_EVALUATION) {
        if (deferred != NULL) {
          deferred[idx] = true;
        }
        all_done = false;
        continue; // Skip to next move
      }

      // Mark as not deferred (in case we're in second phase)
      if (deferred != NULL) {
        deferred[idx] = false;
      }

      // Re-assign small_move. Its pointer location may have changed after all
      // the calls to negamax and possible reallocations in the
      // small_move_arena.
      small_move =
          (SmallMove *)(worker->small_move_arena->memory + element_offset);
      if (-value > best_value) {
        best_value = -value;
        best_tiny_move = small_move->tiny_move;
        pvline_update(pv, &child_pv, small_move,
                      best_value - worker->solver->initial_spread);
      }
      if (worker->current_iterative_deepening_depth == depth) {
        // At the very top depth, set the estimated value of the small move,
        // for the next iterative deepening iteration.
        small_move_set_estimated_value(small_move, -value);
      }
      alpha = MAX(alpha, best_value);
      if (best_value >= beta) {
        // beta cut-off
        all_done = true;
        break;
      }
      // clear the child node's pv for the next child node
      pvline_clear(&child_pv);
    }

    // Single-threaded mode: only one pass needed
    if (!use_abdada) {
      all_done = true;
    }

    // ABDADA: yield when we still have deferred moves so other threads
    // can make progress. Without this, threads tight-spin on deferred
    // moves causing massive CPU waste at shallow depths.
    if (!all_done) {
      sched_yield();
    }
  }

  // Clean up deferred array (only if heap-allocated)
  if (deferred_heap_allocated) {
    free(deferred);
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
  }

  // ABDADA: leave node before returning
  if (abdada_active) {
    transposition_table_leave_node(worker->solver->transposition_table,
                                   node_key);
  }

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

  int initial_move_count =
      generate_stm_plays(worker, worker->solver->requested_plies);
  // Arena pointer better have started at 0, since it was empty.
  assign_estimates_and_sort(worker, 0, initial_move_count, INVALID_TINY_MOVE,
                            initial_hash_key, 0);
  worker->solver->n_initial_moves = initial_move_count;
  assert((size_t)worker->small_move_arena->size ==
         initial_move_count * sizeof(SmallMove));

  worker->current_iterative_deepening_depth = 1;
  int start = 1;
  if (!worker->solver->iterative_deepening_optim) {
    start = plies;
  }

  // Lazy SMP depth jitter: spread threads across different starting depths
  // so they don't all compete on the same shallow levels simultaneously.
  // Thread 0 always starts at depth 1 (main thread, provides per-ply callback).
  // Other threads start at depth 1 + (index % depth_spread), clamped to plies.
  const int num_threads = worker->solver->threads;
  if (num_threads > 1 && worker->thread_index > 0) {
    // Spread across up to 4 different starting depths to reduce contention.
    // With 16 threads: 1 at d1 (thread 0), ~4 at d2, ~4 at d3, ~4 at d4, ~3 at d5
    int depth_offset = 1 + (worker->thread_index % MIN(4, plies - 1));
    start = MIN(depth_offset, plies);
  }

  int32_t prev_value = 0;
  bool use_aspiration = worker->solver->iterative_deepening_optim;

  for (int p = start; p <= plies; p++) {
    // Check if another thread has completed the full search
    if (atomic_load(&worker->solver->search_complete) != 0) {
      break;
    }

    worker->current_iterative_deepening_depth = p;
    PVLine pv;
    pv.game = worker->game_copy;
    pv.num_moves = 0;

    int32_t val;

    // Track whether this depth's search completed validly
    bool search_valid = true;

    // Use aspiration windows after depth 1
    if (use_aspiration && p > 1 && !worker->solver->first_win_optim) {
      int32_t window = ASPIRATION_WINDOW;
      alpha = prev_value - window;
      beta = prev_value + window;

      // Search with narrow window, widen on fail-high/fail-low
      while (true) {
        // Check if another thread completed
        if (atomic_load(&worker->solver->search_complete) != 0) {
          search_valid = false;
          break;
        }

        val = abdada_negamax(worker, initial_hash_key, 0, 0, p, alpha, beta,
                             &pv, true, false);

        if (val <= alpha) {
          // Fail-low: widen alpha
          alpha = (window >= LARGE_VALUE / 2) ? -LARGE_VALUE
                                              : prev_value - window * 2;
          window *= 2;
        } else if (val >= beta) {
          // Fail-high: widen beta
          beta = (window >= LARGE_VALUE / 2) ? LARGE_VALUE
                                             : prev_value + window * 2;
          window *= 2;
        } else {
          // Value is within window, we're done
          break;
        }

        // Reset PV for re-search
        pv.num_moves = 0;
      }
    } else {
      // Full window search for depth 1 or when aspiration disabled
      val = abdada_negamax(worker, initial_hash_key, 0, 0, p, alpha, beta, &pv,
                           true, false);
    }

    // If search was interrupted, don't store results for this depth
    if (!search_valid) {
      break;
    }

    prev_value = val;

    // sort initial moves by valuation for next time.
    SmallMove *initial_moves = (SmallMove *)(worker->small_move_arena->memory);
    qsort(initial_moves, initial_move_count, sizeof(SmallMove),
          compare_small_moves_by_estimated_value);

    // Store result in worker's local tracking
    int32_t pv_value = val - worker->solver->initial_spread;
    worker->best_pv_value = pv_value;
    worker->best_pv = pv;
    worker->completed_depth = p;

    // Call per-ply callback (only thread 0 to avoid race conditions)
    if (worker->thread_index == 0 && worker->solver->per_ply_callback) {
      // If PV is shorter than depth, extend it from TT
      PVLine extended_pv = pv;
      if (pv.num_moves < p && worker->solver->transposition_table_optim) {
        Game *temp_game = game_duplicate(worker->game_copy);
        pvline_extend_from_tt(&extended_pv, temp_game,
                              worker->solver->transposition_table,
                              worker->solver->solving_player, p);
        game_destroy(temp_game);
      }
      worker->solver->per_ply_callback(p, pv_value, &extended_pv,
                                       worker->game_copy,
                                       worker->solver->per_ply_callback_data);
    }

    // Signal other threads to stop when we complete the full search
    if (p == plies) {
      atomic_store(&worker->solver->search_complete, 1);
    }
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

  Game *gc = game_duplicate(game);
  const Board *board = game_get_board(gc);
  const LetterDistribution *ld = game_get_ld(gc);

  if (add_line_breaks) {
    StringGrid *sg = string_grid_create(pv_line->num_moves, 3, 1);
    StringBuilder *tmp_sb = string_builder_create();
    for (int i = 0; i < pv_line->num_moves; i++) {
      int curr_col = 0;
      // Set the player name
      const int player_on_turn = game_get_player_on_turn_index(gc);
      const char *player_name;
      if (game_history) {
        player_name =
            game_history_player_get_name(game_history, player_on_turn);
      } else {
        player_name = player_on_turn == 0 ? "Player 1" : "Player 2";
      }
      string_grid_set_cell(sg, i, curr_col++,
                           get_formatted_string("(%s)", player_name));

      // Set the play sequence index and player name
      string_grid_set_cell(sg, i, curr_col++,
                           get_formatted_string("%d:", i + 1));

      // Set the move string
      small_move_to_move(&move, &(pv_line->moves[i]), board);
      string_builder_add_move(tmp_sb, board, &move, ld, true);
      // Play the move on the board to make the next small_move_to_move make
      // sense.
      play_move(&move, gc, NULL);
      if (game_get_game_end_reason(gc) == GAME_END_REASON_STANDARD) {
        int opp_idx = game_get_player_on_turn_index(gc);
        const Rack *opp_rack =
            player_get_rack(game_get_player(gc, opp_idx));
        int adj = equity_to_int(calculate_end_rack_points(opp_rack, ld));
        string_builder_add_string(tmp_sb, " (");
        string_builder_add_rack(tmp_sb, opp_rack, ld, false);
        string_builder_add_formatted_string(tmp_sb, " +%d)", adj);
      } else if (game_get_game_end_reason(gc) ==
                 GAME_END_REASON_CONSECUTIVE_ZEROS) {
        string_builder_add_string(tmp_sb, " (6 zeros)");
      }
      string_grid_set_cell(sg, i, curr_col++,
                           string_builder_dump(tmp_sb, NULL));
      string_builder_clear(tmp_sb);
    }
    string_builder_destroy(tmp_sb);
    string_builder_add_string_grid(pv_description, sg, false);
    string_grid_destroy(sg);
  } else {
    for (int i = 0; i < pv_line->num_moves; i++) {
      string_builder_add_formatted_string(pv_description, "%d: ", i + 1);
      small_move_to_move(&move, &(pv_line->moves[i]), board);
      string_builder_add_move(pv_description, board, &move, ld, true);
      // Play the move on the board to make the next small_move_to_move make
      // sense.
      play_move(&move, gc, NULL);
      if (game_get_game_end_reason(gc) == GAME_END_REASON_STANDARD) {
        int opp_idx = game_get_player_on_turn_index(gc);
        const Rack *opp_rack =
            player_get_rack(game_get_player(gc, opp_idx));
        int adj = equity_to_int(calculate_end_rack_points(opp_rack, ld));
        string_builder_add_string(pv_description, " (");
        string_builder_add_rack(pv_description, opp_rack, ld, false);
        string_builder_add_formatted_string(pv_description, " +%d)", adj);
      } else if (game_get_game_end_reason(gc) ==
                 GAME_END_REASON_CONSECUTIVE_ZEROS) {
        string_builder_add_string(pv_description, " (6 zeros)");
      }
      if (i != pv_line->num_moves - 1) {
        string_builder_add_string(pv_description, " -> ");
      }
    }
  }
  string_builder_add_string(pv_description, "\n");
  game_destroy(gc);
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
  // Track solve start time
  Timer solve_timer;
  ctimer_start(&solve_timer);

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

  // Run solitaire pre-solve to populate killer tables for move ordering
  run_solitaire_presolve(solver);

  // Generate base seed for Lazy SMP jitter using current time
  uint64_t base_seed = (uint64_t)ctime_get_current_time();

  // Kick-off iterative deepening threads (Lazy SMP)
  EndgameSolverWorker **solver_workers =
      malloc_or_die((sizeof(EndgameSolverWorker *)) * solver->threads);
  cpthread_t *worker_ids =
      malloc_or_die((sizeof(cpthread_t)) * (solver->threads));

  for (int thread_index = 0; thread_index < solver->threads; thread_index++) {
    solver_workers[thread_index] =
        endgame_solver_create_worker(solver, thread_index, base_seed);
    cpthread_create(&worker_ids[thread_index], solver_worker_start,
                    solver_workers[thread_index]);
  }

  // Wait for all threads to complete
  for (int thread_index = 0; thread_index < solver->threads; thread_index++) {
    cpthread_join(worker_ids[thread_index]);
  }

  // Lazy SMP result aggregation: find the best result among all threads
  // The thread that completed the deepest search with a valid result wins
  int best_thread = 0;
  int best_depth = solver_workers[0]->completed_depth;

  for (int thread_index = 1; thread_index < solver->threads; thread_index++) {
    int thread_depth = solver_workers[thread_index]->completed_depth;
    // Prefer deeper completed depth, or same depth from lower-indexed thread
    if (thread_depth > best_depth) {
      best_depth = thread_depth;
      best_thread = thread_index;
    }
  }

  // Copy the best result to the solver's principal variation
  solver->principal_variation = solver_workers[best_thread]->best_pv;
  solver->best_pv_value = solver_workers[best_thread]->best_pv_value;

  // Clean up workers
  for (int thread_index = 0; thread_index < solver->threads; thread_index++) {
    solver_worker_destroy(solver_workers[thread_index]);
  }

  free(solver_workers);
  free(worker_ids);

  // Clean up killer tables from pre-solve
  killer_table_destroy(solver->killer_table[0]);
  killer_table_destroy(solver->killer_table[1]);
  solver->killer_table[0] = NULL;
  solver->killer_table[1] = NULL;

  // Calculate elapsed time
  double elapsed = ctimer_elapsed_seconds(&solve_timer);

  // Print final result with PV
  StringBuilder *final_sb = string_builder_create();
  const LetterDistribution *ld = game_get_ld(endgame_args->game);
  Game *final_game_copy = game_duplicate(endgame_args->game);
  Move move;

  string_builder_add_formatted_string(
      final_sb,
      "FINAL: depth=%d, value=%d, time=%.3fs, pv=", solver->requested_plies,
      solver->best_pv_value, elapsed);
  for (int i = 0; i < solver->principal_variation.num_moves; i++) {
    small_move_to_move(&move, &solver->principal_variation.moves[i],
                       game_get_board(final_game_copy));
    string_builder_add_move(final_sb, game_get_board(final_game_copy), &move,
                            ld, true);
    play_move(&move, final_game_copy, NULL);
    if (game_get_game_end_reason(final_game_copy) ==
        GAME_END_REASON_STANDARD) {
      int opp_idx = game_get_player_on_turn_index(final_game_copy);
      const Rack *opp_rack =
          player_get_rack(game_get_player(final_game_copy, opp_idx));
      int adj = equity_to_int(calculate_end_rack_points(opp_rack, ld));
      string_builder_add_string(final_sb, " (");
      string_builder_add_rack(final_sb, opp_rack, ld, false);
      string_builder_add_formatted_string(final_sb, " +%d)", adj);
    } else if (game_get_game_end_reason(final_game_copy) ==
               GAME_END_REASON_CONSECUTIVE_ZEROS) {
      string_builder_add_string(final_sb, " (6 zeros)");
    }
    if (i < solver->principal_variation.num_moves - 1) {
      string_builder_add_string(final_sb, " ");
    }
  }
  // Append [PX wins by Y] to final line
  {
    int on_turn = game_get_player_on_turn_index(endgame_args->game);
    int p1 = equity_to_int(
        player_get_score(game_get_player(endgame_args->game, 0)));
    int p2 = equity_to_int(
        player_get_score(game_get_player(endgame_args->game, 1)));
    int final_spread =
        (p1 - p2) + (on_turn == 0 ? solver->best_pv_value
                                   : -solver->best_pv_value);
    if (final_spread > 0) {
      string_builder_add_formatted_string(final_sb, " [P1 wins by %d]",
                                          final_spread);
    } else if (final_spread < 0) {
      string_builder_add_formatted_string(final_sb, " [P2 wins by %d]",
                                          -final_spread);
    } else {
      string_builder_add_string(final_sb, " [Tie]");
    }
  }
  log_warn("%s", string_builder_peek(final_sb));
  string_builder_destroy(final_sb);
  game_destroy(final_game_copy);

  endgame_results_set_pvline(results, &solver->principal_variation);
}