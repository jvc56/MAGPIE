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
#include <stdlib.h>

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
  // ABDADA: sentinel value returned when node is being searched by another
  // processor
  ON_EVALUATION = -(1 << 29),
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
  atomic_int nodes_searched;

  int solve_multiple_variations;
  int32_t best_pv_value;
  int requested_plies;
  int threads;
  double tt_fraction_of_mem;
  TranspositionTable *transposition_table;

  // Lazy SMP result synchronization
  cpthread_mutex_t result_mutex;
  atomic_int completed_depth;  // Highest depth completed by any thread
  atomic_int search_complete; // Signal for threads to stop early (0=running, 1=done)

  // Per-ply callback for iterative deepening progress
  EndgamePerPlyCallback per_ply_callback;
  void *per_ply_callback_data;

  // Diagnostic counters for ABDADA
  atomic_int deferred_count;       // How many times a move was deferred
  atomic_int abdada_loops;         // How many ABDADA loop iterations
  atomic_int deferred_mallocs;     // How many deferred arrays allocated

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
  XoshiroPRNG *prng;           // Per-thread PRNG for jitter
  PVLine best_pv;              // Thread-local best PV
  int32_t best_pv_value;       // Thread-local best value
  int completed_depth;         // Depth this thread completed
} EndgameSolverWorker;

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

// Reconstruct the PV by probing the transposition table
// This fills in moves that may have been truncated due to TT cutoffs
static void pvline_reconstruct_from_tt(PVLine *pv_line, Game *game_copy,
                                       TranspositionTable *tt,
                                       int solving_player, int max_depth) {
  if (!tt) {
    return;
  }

  // Start from the current position and probe TT for best moves
  int num_moves = 0;
  MoveList *move_list = move_list_create(1);

  while (num_moves < max_depth &&
         game_get_game_end_reason(game_copy) == GAME_END_REASON_NONE) {
    int on_turn = game_get_player_on_turn_index(game_copy);
    const Player *solving = game_get_player(game_copy, solving_player);
    const Player *other = game_get_player(game_copy, 1 - solving_player);

    // Calculate hash for current position
    uint64_t hash = zobrist_calculate_hash(
        tt->zobrist, game_get_board(game_copy), player_get_rack(solving),
        player_get_rack(other), on_turn != solving_player,
        game_get_consecutive_scoreless_turns(game_copy));

    // Probe TT
    TTEntry entry = transposition_table_lookup(tt, hash);
    if (!ttentry_valid(entry)) {
      break;  // No TT entry, can't continue
    }

    uint64_t tiny_move = ttentry_move(entry);
    if (tiny_move == INVALID_TINY_MOVE) {
      break;  // No move stored
    }

    // Convert tiny_move to SmallMove
    SmallMove sm;
    sm.tiny_move = tiny_move;
    sm.metadata = 0;

    // Get the player on turn's score before the move
    const Player *player_on_turn = game_get_player(game_copy, on_turn);
    int score_before = equity_to_int(player_get_score(player_on_turn));

    // Play the move to advance the position and calculate score
    small_move_to_move(move_list->spare_move, &sm, game_get_board(game_copy));
    play_move(move_list->spare_move, game_copy, NULL);

    // Get score after move (player indices may have swapped, but score stays with player)
    int score_after = equity_to_int(player_get_score(player_on_turn));
    int move_score = score_after - score_before;

    // Store move with calculated score in metadata (lower 16 bits)
    pv_line->moves[num_moves].tiny_move = tiny_move;
    pv_line->moves[num_moves].metadata = (uint32_t)(move_score & 0xFFFF);
    num_moves++;
  }

  pv_line->num_moves = num_moves;
  move_list_destroy(move_list);
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
  es->pruned_kwg = make_kwg_from_words(
      possible_word_list, KWG_MAKER_OUTPUT_GADDAG, KWG_MAKER_MERGE_EXACT);
  dictionary_word_list_destroy(possible_word_list);

  // Initialize Lazy SMP synchronization
  atomic_store(&es->completed_depth, 0);
  atomic_store(&es->search_complete, 0);

  // Initialize diagnostic counters
  atomic_store(&es->deferred_count, 0);
  atomic_store(&es->abdada_loops, 0);
  atomic_store(&es->deferred_mallocs, 0);

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
  cpthread_mutex_init(&es->result_mutex);
  return es;
}

void endgame_solver_reset_tt(EndgameSolver *es) {
  if (!es || !es->transposition_table) {
    return;
  }
  double fraction = es->tt_fraction_of_mem;
  transposition_table_destroy(es->transposition_table);
  es->transposition_table = transposition_table_create(fraction);
}

void endgame_solver_clear_tt(EndgameSolver *solver) {
  if (solver && solver->transposition_table) {
    transposition_table_reset(solver->transposition_table);
  }
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

  // Initialize per-thread PRNG with unique seed for jitter
  // Each thread gets a different seed based on base_seed + thread_index
  solver_worker->prng = prng_create(base_seed + (uint64_t)worker_index * 12345);

  // Initialize per-thread result tracking
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
  arena_destroy(solver_worker->small_move_arena);
  prng_destroy(solver_worker->prng);
  free(solver_worker);
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

  // Lazy SMP jitter: different threads use different move ordering heuristics
  // to explore different parts of the search tree first
  const int thread_idx = worker->thread_index;
  const int num_threads = worker->solver->threads;

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
    } else if (depth > 2 && num_threads > 1) {
      // Lazy SMP jitter for move ordering
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

int32_t abdada_negamax(EndgameSolverWorker *worker, uint64_t node_key,
                       int depth, int32_t alpha, int32_t beta, PVLine *pv,
                       bool pv_node, bool exclusive_p) {

  assert(pv_node || alpha == beta - 1);

  // ABDADA: if exclusive search and another processor is on this node, defer
  const int num_threads = worker->solver->threads;
  if (exclusive_p && num_threads > 1 &&
      worker->solver->transposition_table_optim) {
    if (transposition_table_is_busy(worker->solver->transposition_table,
                                    node_key)) {
      return ON_EVALUATION;
    }
  }

  // ABDADA: mark that we're searching this node
  if (num_threads > 1 && worker->solver->transposition_table_optim) {
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
          if (num_threads > 1) {
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
          if (num_threads > 1) {
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
    if (num_threads > 1 && worker->solver->transposition_table_optim) {
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

  // ABDADA: track deferred moves for second phase
  // Use stack allocation when possible to avoid malloc overhead
  bool deferred_stack[MAX_DEFERRED_STACK];
  bool *deferred = NULL;
  bool deferred_heap_allocated = false;
  bool use_abdada = (num_threads > 1);
  if (use_abdada) {
    if (nplays <= MAX_DEFERRED_STACK) {
      deferred = deferred_stack;
    } else {
      deferred = malloc_or_die(sizeof(bool) * nplays);
      deferred_heap_allocated = true;
      atomic_fetch_add(&worker->solver->deferred_mallocs, 1);
    }
    for (int i = 0; i < nplays; i++) {
      deferred[i] = false;
    }
  }

  // ABDADA two-phase iteration
  bool all_done = false;
  int loop_count = 0;
  while (!all_done) {
    all_done = true;
    loop_count++;

    for (int idx = 0; idx < nplays; idx++) {
      // ABDADA: determine if this move should be searched exclusively
      // First move (idx == 0) is never exclusive
      // In first phase, other moves are exclusive
      // In second phase (deferred[idx] == true), moves are not exclusive
      bool child_exclusive = use_abdada && (idx > 0) &&
                             (deferred == NULL || !deferred[idx]);

      // ABDADA: skip if deferred and we're in first phase (will process later)
      // This check is not needed in the current structure since we mark
      // deferred only after ON_EVALUATION

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
      if (idx == 0 || !worker->solver->negascout_optim) {
        value = abdada_negamax(worker, child_key, depth - 1, -beta, -alpha,
                               &child_pv, pv_node, child_exclusive);
      } else {
        value = abdada_negamax(worker, child_key, depth - 1, -alpha - 1, -alpha,
                               &child_pv, false, child_exclusive);
        if (value != ON_EVALUATION && alpha < -value && -value < beta) {
          // re-search with wider window (not exclusive since we need the value)
          value = abdada_negamax(worker, child_key, depth - 1, -beta, -alpha,
                                 &child_pv, pv_node, false);
        }
      }
      game_unplay_last_move(worker->game_copy);

      // ABDADA: check if move was deferred
      if (value == ON_EVALUATION) {
        if (deferred != NULL) {
          deferred[idx] = true;
          atomic_fetch_add(&worker->solver->deferred_count, 1);
        }
        all_done = false;
        continue; // Skip to next move
      }

      // Mark as not deferred (in case we're in second phase)
      if (deferred != NULL) {
        deferred[idx] = false;
      }

      // Re-assign small_move. Its pointer location may have changed after all
      // the calls to negamax and possible reallocations in the small_move_arena.
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
  }

  // Track ABDADA loop iterations (only if more than 1 loop was needed)
  if (loop_count > 1) {
    atomic_fetch_add(&worker->solver->abdada_loops, loop_count - 1);
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
    // log_warn("arena_dealloced; new size: %ld",
    // worker->small_move_arena->size); printf("%sReset back to %ld\n", spaces,
    // worker->small_move_arena->size);
  }

  // ABDADA: leave node before returning
  if (num_threads > 1 && worker->solver->transposition_table_optim) {
    transposition_table_leave_node(worker->solver->transposition_table,
                                   node_key);
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

  // Lazy SMP depth jitter: different threads can start at different depths
  // This helps threads explore different parts of the search space
  // Thread 0 always starts at depth 1 (the main thread)
  // Other threads can start at depth 1 or 2 based on their index
  const int num_threads = worker->solver->threads;
  if (num_threads > 1 && worker->thread_index > 0) {
    // Even-indexed helper threads (2, 4, 6...) start at depth 2
    // Odd-indexed helper threads (1, 3, 5...) start at depth 1
    // This ensures some threads get to deeper depths faster
    if (worker->thread_index % 2 == 0 && plies >= 2) {
      start = 2;
    }
  }

  // Aspiration window parameters
  const int32_t ASPIRATION_WINDOW = 25;  // Initial window size
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

    // Use aspiration windows after depth 1
    if (use_aspiration && p > 1 && !worker->solver->first_win_optim) {
      int32_t window = ASPIRATION_WINDOW;
      alpha = prev_value - window;
      beta = prev_value + window;

      // Search with narrow window, widen on fail-high/fail-low
      while (true) {
        // Check if another thread completed
        if (atomic_load(&worker->solver->search_complete) != 0) {
          break;
        }

        val = abdada_negamax(worker, initial_hash_key, p, alpha, beta, &pv, true, false);

        if (val <= alpha) {
          // Fail-low: widen alpha
          alpha = (window >= LARGE_VALUE / 2) ? -LARGE_VALUE : prev_value - window * 2;
          window *= 2;
        } else if (val >= beta) {
          // Fail-high: widen beta
          beta = (window >= LARGE_VALUE / 2) ? LARGE_VALUE : prev_value + window * 2;
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
      val = abdada_negamax(worker, initial_hash_key, p, alpha, beta, &pv, true, false);
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
      // If PV seems truncated, try to reconstruct it from TT
      PVLine reconstructed_pv = pv;
      if (pv.num_moves < p && worker->solver->transposition_table_optim) {
        Game *temp_game = game_duplicate(worker->game_copy);
        pvline_reconstruct_from_tt(&reconstructed_pv, temp_game,
                                   worker->solver->transposition_table,
                                   worker->solver->solving_player, p);
        game_destroy(temp_game);
      }
      worker->solver->per_ply_callback(p, pv_value, &reconstructed_pv,
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

  // Print ABDADA diagnostics
  int nodes = atomic_load(&solver->nodes_searched);
  int deferred = atomic_load(&solver->deferred_count);
  int loops = atomic_load(&solver->abdada_loops);
  int mallocs = atomic_load(&solver->deferred_mallocs);
  log_warn("ABDADA stats: nodes=%d, deferred=%d (%.2f%%), extra_loops=%d, deferred_mallocs=%d",
           nodes, deferred, 100.0 * deferred / (nodes > 0 ? nodes : 1),
           loops, mallocs);

  if (solver->transposition_table) {
    int tt_lookups = atomic_load(&solver->transposition_table->lookups);
    int tt_hits = atomic_load(&solver->transposition_table->hits);
    int tt_created = atomic_load(&solver->transposition_table->created);
    int tt_collisions = atomic_load(&solver->transposition_table->t2_collisions);
    log_warn("TT stats: lookups=%d, hits=%d (%.2f%%), created=%d, t2_collisions=%d (%.2f%%)",
             tt_lookups, tt_hits, 100.0 * tt_hits / (tt_lookups > 0 ? tt_lookups : 1),
             tt_created, tt_collisions, 100.0 * tt_collisions / (tt_lookups > 0 ? tt_lookups : 1));
  }

  // Clean up workers
  for (int thread_index = 0; thread_index < solver->threads; thread_index++) {
    solver_worker_destroy(solver_workers[thread_index]);
  }

  free(solver_workers);
  free(worker_ids);

  endgame_results_set_pvline(results, &solver->principal_variation);
}