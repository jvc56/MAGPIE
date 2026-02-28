#include "endgame.h"

#include "../compat/cpthread.h"
#include "../compat/csched.h"
#include "../compat/ctime.h"
#include "../def/cpthread_defs.h"
#include "../def/equity_defs.h"
#include "../def/game_defs.h"
#include "../def/kwg_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/move_defs.h"
#include "../def/players_data_defs.h"
#include "../def/thread_control_defs.h"
#include "../ent/bag.h"
#include "../ent/board.h"
#include "../ent/dictionary_word.h"
#include "../ent/endgame_results.h"
#include "../ent/equity.h"
#include "../ent/game.h"
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
#include "../str/letter_distribution_string.h"
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
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
  DEFAULT_ENDGAME_MOVELIST_CAPACITY = 250000,
  // Maximum moves for stack allocation in ABDADA deferred tracking
  // Keep small to avoid stack overflow in deep recursive searches
  MAX_DEFERRED_STACK = 64,
  // How many abdada_negamax calls between per-depth deadline checks.
  // Checked per-worker (no cross-thread contention). At ~100K nodes/s/thread,
  // 4096 nodes ≈ 40ms granularity — cheap but responsive enough to bail early.
  DEPTH_DEADLINE_CHECK_INTERVAL = 4096,
  // Bit flags for move estimates. These large numbers will force these
  // estimated values to sort first.
  LARGE_VALUE = 1 << 30, // for alpha-beta pruning
  EARLY_PASS_BF = 1 << 29,
  HASH_MOVE_BF = 1 << 28,
  GOING_OUT_BF = 1 << 27,
  // ABDADA: sentinel value returned when node is being searched by another
  // processor
  ON_EVALUATION = -(1 << 29),
  ABDADA_INTERRUPTED = -(1 << 28),
  // Aspiration window initial size
  ASPIRATION_WINDOW = 25,
  // Conservation bonus weights: penalize playing tiles when opponent is stuck
  CONSERVATION_TILE_WEIGHT = 7,
  CONSERVATION_VALUE_WEIGHT = 2,
  // Random noise range for thread jitter, centered around zero
  THREAD_JITTER_NOISE = 8,
};

// Returns fraction of opponent's rack score that is stuck (0.0 = none, 1.0 =
// all). A tile is "stuck" if no legal move plays that tile type.
// tiles_played_bv: bitvector where bit i is set if machine letter i appears in
//   any valid move (from MOVE_RECORD_TILES_PLAYED).
static float stuck_tile_fraction_from_bv(const LetterDistribution *ld,
                                         const Rack *rack,
                                         uint64_t tiles_played_bv) {
  int total_score = 0;
  int stuck_score = 0;
  int ld_size = ld_get_size(ld);
  for (int ml = 0; ml < ld_size; ml++) {
    int count = rack_get_letter(rack, ml);
    if (count > 0) {
      int score = count * ld_get_score(ld, ml);
      total_score += score;
      if (!(tiles_played_bv & ((uint64_t)1 << ml))) {
        stuck_score += score;
      }
    }
  }
  if (total_score == 0) {
    return 0.0F;
  }
  return (float)stuck_score / (float)total_score;
}

struct EndgameSolver {
  int initial_spread;
  int solving_player;

  int initial_small_move_arena_size;
  bool iterative_deepening_optim;
  bool first_win_optim;
  bool transposition_table_optim;
  bool negascout_optim;
  bool use_heuristics;
  bool forced_pass_bypass;
  PVLine principal_variation;

  KWG *pruned_kwgs[2];
  dual_lexicon_mode_t dual_lexicon_mode;
  bool skip_pruned_cross_sets;
  bool cross_set_precheck;
  double soft_time_limit;
  double hard_time_limit;

  int solve_multiple_variations;
  int requested_plies;
  int threads;
  double tt_fraction_of_mem;
  TranspositionTable *transposition_table;

  // Signal for threads to stop early (0=running, 1=done)
  atomic_int search_complete;
  // Per-depth deadline: absolute CLOCK_MONOTONIC nanoseconds; 0 = disabled.
  // Thread 0 sets this after each completed depth (from EBF estimate).
  // All worker threads check it periodically and bail if exceeded.
  _Atomic int64_t depth_deadline_ns;
  // Flag: stuck-tile mode has been logged (0=not yet, 1=logged)
  atomic_int stuck_tile_logged;
  // Fraction of opponent's tiles that are stuck at the root (0.0 = none)
  float initial_opp_stuck_frac;

  // Root move progress tracking (thread 0 only, for external polling)
  atomic_int root_moves_completed; // How many root moves finished this depth
  atomic_int root_moves_total;     // Total root moves at this depth
  atomic_int current_depth;        // Depth currently being searched

  // Ply-2 progress tracking (children of root move #1, thread 0 only)
  atomic_int ply2_moves_completed;
  atomic_int ply2_moves_total;

  // Per-ply callback for iterative deepening progress
  EndgamePerPlyCallback per_ply_callback;
  void *per_ply_callback_data;

  // Owned by the caller:
  EndgameResults *results;
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
  XoshiroPRNG *prng;       // Per-thread PRNG for jitter
  PVLine best_pv;          // Thread-local best PV
  int32_t best_pv_value;   // Thread-local best value
  int completed_depth;     // Depth this thread completed
  int n_initial_moves;     // Number of root moves (thread-local to avoid races)
  bool in_first_root_move; // True when thread 0 is inside root move idx==0
  // Counter for throttling per-depth deadline checks in abdada_negamax
  uint64_t nodes_since_deadline_check;
} EndgameSolverWorker;

#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

// Insert a value into a sorted (descending) top-K array.
// Returns the Kth-best value (or -LARGE_VALUE if fewer than K values stored).
static inline int32_t topk_insert(int32_t *topk, int *n, int k, int32_t val) {
  // Find insertion point (descending order)
  int pos = *n;
  for (int i = 0; i < *n; i++) {
    if (val > topk[i]) {
      pos = i;
      break;
    }
  }
  // Shift elements down if room
  if (*n < k) {
    for (int i = *n; i > pos; i--) {
      topk[i] = topk[i - 1];
    }
    topk[pos] = val;
    (*n)++;
  } else if (pos < k) {
    for (int i = k - 1; i > pos; i--) {
      topk[i] = topk[i - 1];
    }
    topk[pos] = val;
  }
  // Return the Kth-best (last element) or -LARGE_VALUE if not enough values
  if (*n >= k) {
    return topk[k - 1];
  }
  return -(1 << 30); // -LARGE_VALUE
}

static void pvline_clear(PVLine *pv_line) {
  pv_line->num_moves = 0;
  pv_line->score = 0;
  pv_line->negamax_depth = 0;
}

static void pvline_update(PVLine *pv_line, const PVLine *new_pv_line,
                          const SmallMove *move, int32_t score) {
  pvline_clear(pv_line);
  pv_line->moves[0].metadata = move->metadata;
  pv_line->moves[0].tiny_move = move->tiny_move;
  int child_moves = new_pv_line->num_moves;
  if (child_moves > MAX_VARIANT_LENGTH - 1) {
    child_moves = MAX_VARIANT_LENGTH - 1;
  }
  for (int i = 0; i < child_moves; i++) {
    pv_line->moves[i + 1].metadata = new_pv_line->moves[i].metadata;
    pv_line->moves[i + 1].tiny_move = new_pv_line->moves[i].tiny_move;
  }
  pv_line->num_moves = child_moves + 1;
  pv_line->score = score;
  // negamax_depth is not updated here; callers set it explicitly.
}

// Greedy playout for display: extend PV with highest-scoring moves after
// TT extension. Returns number of moves appended.
static int greedy_playout_for_display(PVLine *pv_line, int start_idx,
                                      Game *game_copy, MoveList *move_list) {
  int num_moves = start_idx;
  while (num_moves < MAX_VARIANT_LENGTH &&
         game_get_game_end_reason(game_copy) == GAME_END_REASON_NONE) {
    const MoveGenArgs greedy_args = {
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
    generate_moves(&greedy_args);
    int nplays = move_list->count;
    if (nplays == 0) {
      break;
    }

    // Check for consecutive passes ending the game
    if (nplays == 1 && small_move_is_pass(move_list->small_moves[0]) &&
        game_get_consecutive_scoreless_turns(game_copy) >= 1) {
      break;
    }

    // Pick highest-scoring move
    int best_idx = 0;
    int best_score = INT32_MIN;
    for (int j = 0; j < nplays; j++) {
      int sc = small_move_get_score(move_list->small_moves[j]);
      if (sc > best_score) {
        best_score = sc;
        best_idx = j;
      }
    }

    SmallMove best_sm = *move_list->small_moves[best_idx];
    small_move_to_move(move_list->spare_move, &best_sm,
                       game_get_board(game_copy));
    play_move(move_list->spare_move, game_copy, NULL);

    pv_line->moves[num_moves] = best_sm;
    num_moves++;
  }
  return num_moves - start_idx;
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

    SmallMove sm = {0};
    sm.tiny_move = tiny_move;
    sm.metadata.score = move_score;
    small_move_to_move(move_list->spare_move, &sm, game_get_board(game_copy));
    play_move(move_list->spare_move, game_copy, NULL);

    pv_line->moves[num_moves].tiny_move = tiny_move;
    memset(&pv_line->moves[num_moves].metadata, 0,
           sizeof(pv_line->moves[num_moves].metadata));
    pv_line->moves[num_moves].metadata.score = move_score;
    num_moves++;
  }

  // Preserve original negamax_depth from the search. TT-extended moves
  // are from cached entries, not the current depth's negamax search.
  // The | separator in PV display marks the search depth boundary.

  // Greedy playout: if game isn't over, extend PV with highest-scoring moves.
  // This handles cases where the search PV was truncated (e.g., parallel search
  // effects) and TT entries were overwritten.
  num_moves +=
      greedy_playout_for_display(pv_line, num_moves, game_copy, move_list);

  pv_line->num_moves = num_moves;
  small_move_list_destroy(move_list);
}

void endgame_solver_reset(EndgameSolver *es, const EndgameArgs *endgame_args) {
  es->first_win_optim = false;
  es->transposition_table_optim = true;
  es->iterative_deepening_optim = true;
  es->negascout_optim = true;
  es->use_heuristics = endgame_args->use_heuristics;
  es->forced_pass_bypass = endgame_args->forced_pass_bypass;
  int num_top_moves = endgame_args->num_top_moves;
  if (num_top_moves <= 0) {
    num_top_moves = 1;
  }
  es->solve_multiple_variations = num_top_moves > 1 ? num_top_moves : 0;
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

  kwg_destroy(es->pruned_kwgs[0]);
  kwg_destroy(es->pruned_kwgs[1]);
  es->pruned_kwgs[0] = NULL;
  es->pruned_kwgs[1] = NULL;

  es->dual_lexicon_mode = endgame_args->dual_lexicon_mode;
  // INFORMED mode with shared KWGs is meaningless (both players have the same
  // lexicon). Coerce to IGNORANT to avoid building only pruned_kwgs[0] while
  // get_kwg_for_cross_set would try to dereference pruned_kwgs[1] as NULL.
  bool shared_kwg =
      game_get_data_is_shared(endgame_args->game, PLAYERS_DATA_TYPE_KWG);
  if (es->dual_lexicon_mode == DUAL_LEXICON_MODE_INFORMED && shared_kwg) {
    es->dual_lexicon_mode = DUAL_LEXICON_MODE_IGNORANT;
  }
  es->skip_pruned_cross_sets = endgame_args->skip_pruned_cross_sets;
  es->cross_set_precheck = !endgame_args->skip_pruned_cross_sets;
  es->soft_time_limit = endgame_args->soft_time_limit;
  es->hard_time_limit = endgame_args->hard_time_limit;
  bool create_separate_kwgs =
      (es->dual_lexicon_mode == DUAL_LEXICON_MODE_INFORMED) && !shared_kwg;

  // Generate pruned KWG(s) from the set of possible words on this board.
  // In IGNORANT mode (or shared-KWG), one pruned KWG is used for everything.
  // In INFORMED mode with different lexicons, each player index gets its own
  // pruned KWG so that cross-set index i uses the pruned KWG derived from
  // player i's lexicon.
  for (int player_idx = 0; player_idx < (create_separate_kwgs ? 2 : 1);
       player_idx++) {
    const KWG *full_kwg =
        player_get_kwg(game_get_player(endgame_args->game, player_idx));
    DictionaryWordList *word_list = dictionary_word_list_create();
    generate_possible_words(endgame_args->game, full_kwg, word_list);
    es->pruned_kwgs[player_idx] = make_kwg_from_words_small(
        word_list, KWG_MAKER_OUTPUT_GADDAG, KWG_MAKER_MERGE_EXACT);
    dictionary_word_list_destroy(word_list);
  }

  // Initialize ABDADA synchronization
  atomic_store(&es->search_complete, 0);
  atomic_store(&es->depth_deadline_ns, 0);
  atomic_store(&es->stuck_tile_logged, 0);
  atomic_store(&es->root_moves_completed, 0);
  atomic_store(&es->root_moves_total, 0);
  atomic_store(&es->current_depth, 0);
  atomic_store(&es->ply2_moves_completed, 0);
  atomic_store(&es->ply2_moves_total, 0);

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
  if (es->results) {
    endgame_results_reset(es->results);
    endgame_results_set_valid_for_current_game_state(es->results, true);
  }
}

EndgameSolver *endgame_solver_create(void) {
  EndgameSolver *es = calloc_or_die(1, sizeof(EndgameSolver));
  return es;
}

const TranspositionTable *
endgame_solver_get_transposition_table(const EndgameSolver *es) {
  return es->transposition_table;
}

void endgame_solver_get_progress(const EndgameSolver *es, int *current_depth,
                                 int *root_moves_completed,
                                 int *root_moves_total,
                                 int *ply2_moves_completed,
                                 int *ply2_moves_total) {
  *current_depth = atomic_load(&es->current_depth);
  *root_moves_completed = atomic_load(&es->root_moves_completed);
  *root_moves_total = atomic_load(&es->root_moves_total);
  *ply2_moves_completed = atomic_load(&es->ply2_moves_completed);
  *ply2_moves_total = atomic_load(&es->ply2_moves_total);
}

void endgame_solver_destroy(EndgameSolver *es) {
  if (!es) {
    return;
  }
  transposition_table_destroy(es->transposition_table);
  kwg_destroy(es->pruned_kwgs[0]);
  kwg_destroy(es->pruned_kwgs[1]);
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

  // Set override KWGs so cross-set computation uses the pruned lexicon
  if (!solver->skip_pruned_cross_sets) {
    game_set_override_kwgs(solver_worker->game_copy, solver->pruned_kwgs[0],
                           solver->pruned_kwgs[1], solver->dual_lexicon_mode);
    // Regenerate initial cross-sets using the pruned KWGs
    game_gen_all_cross_sets(solver_worker->game_copy);
  }
  solver_worker->move_list =
      move_list_create_small(DEFAULT_ENDGAME_MOVELIST_CAPACITY);

  solver_worker->small_move_arena =
      create_arena(solver->initial_small_move_arena_size, 16);

  solver_worker->solver = solver;
  // Zero-initialize move_undos to prevent undefined behavior from stale values
  memset(solver_worker->move_undos, 0, sizeof(solver_worker->move_undos));

  // Initialize per-thread PRNG with unique seed for jitter
  // Each thread gets a different seed based on base_seed + thread_index
  solver_worker->prng = prng_create(base_seed + (uint64_t)worker_index * 12345);

  // Initialize per-thread result tracking
  solver_worker->best_pv.game = solver_worker->game_copy;
  solver_worker->best_pv.num_moves = 0;
  solver_worker->best_pv_value = -LARGE_VALUE;
  solver_worker->completed_depth = 0;
  solver_worker->nodes_since_deadline_check = 0;

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

// Returns the pruned KWG for the given player index.
// In shared-KWG mode, only pruned_kwgs[0] exists, so it is always returned.
// In non-shared mode, each player index maps to its own pruned KWG.
static inline const KWG *solver_get_pruned_kwg(const EndgameSolver *solver,
                                               int player_index) {
  if (solver->pruned_kwgs[1] == NULL) {
    return solver->pruned_kwgs[0];
  }
  return solver->pruned_kwgs[player_index];
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
  const MoveGenArgs args = {
      .game = worker->game_copy,
      .move_list = worker->move_list,
      .move_record_type = MOVE_RECORD_ALL_SMALL,
      .move_sort_type = MOVE_SORT_SCORE,
      .override_kwg = solver_get_pruned_kwg(
          worker->solver, game_get_player_on_turn_index(worker->game_copy)),
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

// Sum face values of placed tiles in a move, skipping played-through markers.
static int compute_played_tiles_face_value(const Move *move,
                                           const LetterDistribution *ld) {
  int face_value = 0;
  int tlen = move_get_tiles_length(move);
  for (int pos = 0; pos < tlen; pos++) {
    uint8_t tile = move_get_tile(move, pos);
    if (tile == PLAYED_THROUGH_MARKER) {
      continue;
    }
    uint8_t ml = get_is_blanked(tile) ? BLANK_MACHINE_LETTER : tile;
    face_value += equity_to_int(ld_get_score(ld, ml));
  }
  return face_value;
}

// Conservation bonus: penalize playing tiles when opponent has stuck tiles.
// Returns (CONSERVATION_TILE_WEIGHT * tile_count +
//          CONSERVATION_VALUE_WEIGHT * face_value) * opp_stuck_frac.
// The move must already be expanded into spare_move.
static int compute_conservation_bonus(const Move *move,
                                      const LetterDistribution *ld,
                                      float opp_stuck_frac) {
  int tlen = move_get_tiles_length(move);
  int face_value = 0;
  int tile_count = 0;
  for (int pos = 0; pos < tlen; pos++) {
    uint8_t tile = move_get_tile(move, pos);
    if (tile == PLAYED_THROUGH_MARKER) {
      continue;
    }
    uint8_t ml = get_is_blanked(tile) ? BLANK_MACHINE_LETTER : tile;
    face_value += equity_to_int(ld_get_score(ld, ml));
    tile_count++;
  }
  return (int)((float)(CONSERVATION_TILE_WEIGHT * tile_count +
                       CONSERVATION_VALUE_WEIGHT * face_value) *
               opp_stuck_frac);
}

// Thread jitter for ABDADA search diversity: each thread gets a unique bias
// based on tiles played. Odd threads favor aggressive play, even threads favor
// conservative play. Returns 0 for single-threaded or thread 0.
static int compute_thread_jitter(EndgameSolverWorker *worker,
                                 int tiles_played) {
  int thread_idx = worker->thread_index;
  if (worker->solver->threads <= 1 || thread_idx == 0) {
    return 0;
  }
  int bias = thread_idx * tiles_played;
  int jitter = (thread_idx % 2 == 1) ? bias : -bias;
  jitter += (int)(prng_get_random_number(worker->prng, THREAD_JITTER_NOISE)) -
            THREAD_JITTER_NOISE / 2;
  return jitter;
}

// Compute build chain values for recursive build detection. When opponent is
// stuck, cumulative build chain values reward moves that build toward longer
// scoring sequences (e.g., ED->RED->IRED->AIRED->WAIRED).
// Returns malloc'd int[] (caller frees), or NULL when move_count <= 1 or
// opp_stuck_frac == 0.
static int *compute_build_chain_values(EndgameSolverWorker *worker,
                                       int move_count, size_t arena_offset,
                                       float opp_stuck_frac) {
  if (opp_stuck_frac <= 0.0F || move_count <= 1) {
    return NULL;
  }

  int *build_values = malloc_or_die(move_count * sizeof(int));

  // Sort indices by tiles_played descending for bottom-up computation
  int *order = malloc_or_die(move_count * sizeof(int));
  for (int i = 0; i < move_count; i++) {
    order[i] = i;
  }
  // Simple insertion sort by tiles_played descending
  for (int i = 1; i < move_count; i++) {
    int key = order[i];
    const SmallMove *sm_key =
        (const SmallMove *)(worker->small_move_arena->memory + arena_offset +
                            key * sizeof(SmallMove));
    int tp_key = small_move_get_tiles_played(sm_key);
    int j = i - 1;
    while (j >= 0) {
      const SmallMove *sm_j =
          (const SmallMove *)(worker->small_move_arena->memory + arena_offset +
                              order[j] * sizeof(SmallMove));
      int tp_j = small_move_get_tiles_played(sm_j);
      if (tp_j >= tp_key) {
        break;
      }
      order[j + 1] = order[j];
      j--;
    }
    order[j + 1] = key;
  }

  // Bottom-up: process moves from most tiles to fewest
  for (int oi = 0; oi < move_count; oi++) {
    int i = order[oi];
    const SmallMove *sm_a =
        (const SmallMove *)(worker->small_move_arena->memory + arena_offset +
                            i * sizeof(SmallMove));
    build_values[i] = small_move_get_score(sm_a);
    if (small_move_is_pass(sm_a)) {
      continue;
    }

    int dir_a = (int)(sm_a->tiny_move & 1);
    int row_a = (int)((sm_a->tiny_move & SMALL_MOVE_ROW_BITMASK) >> 6);
    int col_a = (int)((sm_a->tiny_move & SMALL_MOVE_COL_BITMASK) >> 1);
    int len_a = small_move_get_play_length(sm_a);
    int tp_a = small_move_get_tiles_played(sm_a);

    // Find the best extension (containing move with more tiles, already
    // processed so build_values[j] is final)
    int best_extension = 0;
    for (int oj = 0; oj < oi; oj++) {
      int j = order[oj];
      const SmallMove *sm_b =
          (const SmallMove *)(worker->small_move_arena->memory + arena_offset +
                              j * sizeof(SmallMove));
      if (small_move_is_pass(sm_b)) {
        continue;
      }
      int tp_b = small_move_get_tiles_played(sm_b);
      if (tp_b <= tp_a) {
        continue;
      }
      if ((int)(sm_b->tiny_move & 1) != dir_a) {
        continue;
      }

      int row_b = (int)((sm_b->tiny_move & SMALL_MOVE_ROW_BITMASK) >> 6);
      int col_b = (int)((sm_b->tiny_move & SMALL_MOVE_COL_BITMASK) >> 1);
      int len_b = small_move_get_play_length(sm_b);

      bool contained;
      if (dir_a == 0) {
        contained = (row_a == row_b) && (col_a >= col_b) &&
                    (col_a + len_a <= col_b + len_b);
      } else {
        contained = (col_a == col_b) && (row_a >= row_b) &&
                    (row_a + len_a <= row_b + len_b);
      }
      if (!contained) {
        continue;
      }

      // Verify actual tiles match at each position.
      // Convert both to full Moves and compare tile-by-tile.
      Move mv_a;
      Move mv_b;
      const Board *brd = game_get_board(worker->game_copy);
      small_move_to_move(&mv_a, sm_a, brd);
      small_move_to_move(&mv_b, sm_b, brd);

      // Offset of A's start within B's tile span
      int offset_in_b;
      if (dir_a == 0) {
        offset_in_b = col_a - col_b;
      } else {
        offset_in_b = row_a - row_b;
      }

      bool tiles_match = true;
      for (int ti = 0; ti < len_a; ti++) {
        uint8_t tile_a = move_get_tile(&mv_a, ti);
        if (tile_a == PLAYED_THROUGH_MARKER) {
          continue; // board tile, not placed by A
        }
        uint8_t tile_b = move_get_tile(&mv_b, ti + offset_in_b);
        if (tile_a != tile_b) {
          tiles_match = false;
          break;
        }
      }
      if (!tiles_match) {
        continue;
      }

      if (build_values[j] > best_extension) {
        best_extension = build_values[j];
      }
    }
    if (best_extension > 0) {
      build_values[i] += best_extension;
    }
  }
  free(order);
  return build_values;
}

void assign_estimates_and_sort(EndgameSolverWorker *worker, int move_count,
                               uint64_t tt_move, float opp_stuck_frac) {
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

  int *build_values = compute_build_chain_values(worker, move_count,
                                                 arena_offset, opp_stuck_frac);

  const Board *est_board = game_get_board(worker->game_copy);
  const LetterDistribution *est_ld = game_get_ld(worker->game_copy);

  for (size_t i = 0; i < (size_t)move_count; i++) {
    size_t element_offset = arena_offset + i * sizeof(SmallMove);
    SmallMove *current_move =
        (SmallMove *)(worker->small_move_arena->memory + element_offset);

    bool is_non_pass_partial =
        !small_move_is_pass(current_move) &&
        small_move_get_tiles_played(current_move) < ntiles_on_rack;

    // Conservation bonus: penalize playing tiles when opponent has stuck tiles.
    int conservation_bonus = 0;
    if (opp_stuck_frac > 0.0F && is_non_pass_partial) {
      small_move_to_move(worker->move_list->spare_move, current_move,
                         est_board);
      conservation_bonus = compute_conservation_bonus(
          worker->move_list->spare_move, est_ld, opp_stuck_frac);
    }

    if (small_move_get_tiles_played(current_move) == ntiles_on_rack) {
      small_move_set_estimated_value(
          current_move,
          equity_to_int(int_to_equity(small_move_get_score(current_move)) +
                        (calculate_end_rack_points(
                            other_rack, game_get_ld(worker->game_copy)))) |
              GOING_OUT_BF);
    } else if (build_values) {
      // Use build-chain estimate as center: when opponent has stuck tiles,
      // prorate the chain boost by stuck fraction. build_values[i] includes
      // the move's own score plus any extension chain value.
      int score = small_move_get_score(current_move);
      int estimate;
      if (build_values[i] > score) {
        estimate =
            score + (int)(opp_stuck_frac * (float)(build_values[i] - score));
      } else {
        estimate = score;
      }
      estimate -= conservation_bonus;
      estimate += compute_thread_jitter(
          worker, small_move_get_tiles_played(current_move));
      small_move_set_estimated_value(current_move, estimate);
    } else {
      int score = small_move_get_score(current_move);
      int estimate = score - conservation_bonus;
      // Static endgame equity: score minus face value of tiles played.
      if (is_non_pass_partial) {
        // spare_move may already be populated from conservation bonus above;
        // if not (opp_stuck_frac == 0), expand it now.
        if (opp_stuck_frac <= 0.0F) {
          small_move_to_move(worker->move_list->spare_move, current_move,
                             est_board);
        }
        estimate -= compute_played_tiles_face_value(
            worker->move_list->spare_move, est_ld);
      }
      estimate += compute_thread_jitter(
          worker, small_move_get_tiles_played(current_move));
      small_move_set_estimated_value(current_move, estimate);
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
  free(build_values);
  // sort moves by estimated value, from biggest to smallest value. A good move
  // sorting is instrumental to the performance of ab pruning.
  SmallMove *small_moves =
      (SmallMove *)(worker->small_move_arena->memory + arena_offset);
  qsort(small_moves, move_count, sizeof(SmallMove),
        compare_small_moves_by_estimated_value);
}

static bool iterative_deepening_should_stop(EndgameSolver *solver);

// Generate opponent's moves in TILES_PLAYED mode and return stuck-tile
// fraction. Saves and restores player-on-turn if it differs from opp_idx.
// If tiles_played_bv_out is non-NULL, writes the bitvector of tile types
// that appear in at least one valid move.
// solver is nullable; when non-NULL an interrupt check is performed before
// the expensive generate_moves call so a fired interrupt cuts the work short.
// Callers detect the interrupt themselves after this returns.
static float compute_opp_stuck_fraction(Game *game, MoveList *move_list,
                                        const KWG *pruned_kwg, int opp_idx,
                                        int thread_index,
                                        bool cross_set_precheck,
                                        uint64_t *tiles_played_bv_out,
                                        EndgameSolver *solver) {
  int saved_on_turn = game_get_player_on_turn_index(game);
  if (saved_on_turn != opp_idx) {
    game_set_player_on_turn_index(game, opp_idx);
  }
  const Rack *opp_rack = player_get_rack(game_get_player(game, opp_idx));
  // Cross-set pre-check: scan the board once to find all tiles with valid
  // single-tile plays. If every rack tile is playable, stuck fraction is 0 —
  // skip movegen. For 1-tile racks, the check is authoritative (no multi-tile
  // words possible), so skip movegen regardless of result. For multi-tile
  // racks that fall through, seed movegen's tiles_played bitvector with the
  // known-playable tiles so movegen doesn't have to rediscover them.
  //
  // Only valid when the board's cross-sets are up to date and were generated
  // from the same KWG used by movegen (the pruned KWG). When cross-sets are
  // stale (lazy update pending) or generated from a different KWG
  // (skip_pruned_cross_sets mode), skip the pre-check and fall through to
  // full movegen.
  uint64_t opp_tiles_bv = 0;
  const Board *board = game_get_board(game);
  if (cross_set_precheck && board_get_cross_sets_valid(board)) {
    bool kwgs_shared = game_get_data_is_shared(game, PLAYERS_DATA_TYPE_KWG);
    int ci = board_get_cross_set_index(kwgs_shared, opp_idx);
    const LetterDistribution *ld = game_get_ld(game);
    int ld_size = ld_get_size(ld);
    uint64_t rack_tiles_bv = 0;
    for (int ml = 0; ml < ld_size; ml++) {
      if (rack_get_letter(opp_rack, ml) > 0) {
        rack_tiles_bv |= ((uint64_t)1 << ml);
      }
    }
    uint64_t rack_non_blank = rack_tiles_bv & ~(uint64_t)1;
    uint64_t playable_bv =
        board_get_playable_tiles_bv(board, ci, rack_non_blank);
    opp_tiles_bv = playable_bv & rack_tiles_bv;
    if ((rack_tiles_bv & 1) && (playable_bv >> 1)) {
      opp_tiles_bv |= 1;
    }
    bool all_playable = (opp_tiles_bv == rack_tiles_bv);
    if (all_playable || rack_get_total_letters(opp_rack) == 1) {
      if (saved_on_turn != opp_idx) {
        game_set_player_on_turn_index(game, saved_on_turn);
      }
      if (tiles_played_bv_out) {
        *tiles_played_bv_out = opp_tiles_bv;
      }
      return all_playable
                 ? 0.0F
                 : stuck_tile_fraction_from_bv(ld, opp_rack, opp_tiles_bv);
    }
  }
  // Check for interrupt before the expensive movegen call.  If already fired,
  // restore game state and return early — the caller will re-detect the
  // interrupt immediately and discard this 0.0F result.
  if (solver && iterative_deepening_should_stop(solver)) {
    if (saved_on_turn != opp_idx) {
      game_set_player_on_turn_index(game, saved_on_turn);
    }
    if (tiles_played_bv_out) {
      *tiles_played_bv_out = opp_tiles_bv;
    }
    return 0.0F;
  }
  const MoveGenArgs tp_args = {
      .game = game,
      .move_list = move_list,
      .move_record_type = MOVE_RECORD_TILES_PLAYED,
      .move_sort_type = MOVE_SORT_SCORE,
      .override_kwg = pruned_kwg,
      .thread_index = thread_index,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
      .tiles_played_bv = &opp_tiles_bv,
      .initial_tiles_bv = opp_tiles_bv,
  };
  generate_moves(&tp_args);
  if (saved_on_turn != opp_idx) {
    game_set_player_on_turn_index(game, saved_on_turn);
  }
  if (tiles_played_bv_out) {
    *tiles_played_bv_out = opp_tiles_bv;
  }
  return stuck_tile_fraction_from_bv(game_get_ld(game), opp_rack, opp_tiles_bv);
}

// Greedy playout at depth==0 leaf nodes: generate moves iteratively,
// pick best (with conservation bonus), compute final spread with rack
// adjustments, unplay moves, store in TT. Returns evaluation from
// on_turn's perspective.
static int32_t negamax_greedy_leaf_playout(EndgameSolverWorker *worker,
                                           uint64_t node_key, int on_turn_idx,
                                           int32_t on_turn_spread, PVLine *pv,
                                           float opp_stuck_frac) {
  int solving_player = worker->solver->solving_player;
  int plies = worker->solver->requested_plies;
  int playout_depth = 0;
  int max_playout = MAX_SEARCH_DEPTH - plies;

  // Recompute opp_stuck_frac from the current position rather than using the
  // parent's value. This ensures position-dependent (not path-dependent)
  // evaluation, which is required for TT consistency across threads and
  // transpositions.
  if (worker->solver->use_heuristics) {
    int opp_idx = 1 - solving_player;
    opp_stuck_frac = compute_opp_stuck_fraction(
        worker->game_copy, worker->move_list,
        solver_get_pruned_kwg(worker->solver, opp_idx), opp_idx,
        worker->thread_index, worker->solver->cross_set_precheck, NULL,
        worker->solver);
  }

  bool playout_interrupted = false;
  while (game_get_game_end_reason(worker->game_copy) == GAME_END_REASON_NONE &&
         playout_depth < max_playout) {
    if (thread_control_get_status(worker->solver->thread_control) ==
        THREAD_CONTROL_STATUS_USER_INTERRUPT) {
      playout_interrupted = true;
      break;
    }
    // Lazy cross-set update from the previous move's undo.
    // When playout_depth==0, the previous move is the last negamax move
    // at undo slot plies-1. For subsequent playout moves, the previous
    // undo is at plies + playout_depth - 1.
    Board *pb = game_get_board(worker->game_copy);
    if (!board_get_cross_sets_valid(pb)) {
      int undo_idx = plies + playout_depth - 1;
      const MoveUndo *prev = &worker->move_undos[undo_idx];
      if (prev->move_tiles_length > 0) {
        update_cross_set_for_move_from_undo(prev, worker->game_copy);
      }
      board_set_cross_sets_valid(pb, true);
    }

    const MoveGenArgs pargs = {
        .game = worker->game_copy,
        .move_list = worker->move_list,
        .move_record_type = MOVE_RECORD_ALL_SMALL,
        .move_sort_type = MOVE_SORT_SCORE,
        .override_kwg = solver_get_pruned_kwg(
            worker->solver, game_get_player_on_turn_index(worker->game_copy)),
        .thread_index = worker->thread_index,
        .eq_margin_movegen = 0,
        .target_equity = EQUITY_MAX_VALUE,
        .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
    };
    generate_moves(&pargs);
    int nplays = worker->move_list->count;

    if (nplays == 0) {
      break;
    }

    // Check for consecutive passes ending the game
    if (nplays == 1 && small_move_is_pass(worker->move_list->small_moves[0]) &&
        game_get_consecutive_scoreless_turns(worker->game_copy) >= 1) {
      break; // would be 6 consecutive zeros
    }

    // Pick best move by adjusted score.
    // When opponent has stuck tiles and solving player is on turn,
    // prefer conservation: penalize playing many tiles / high-value tiles.
    int playout_on_turn = game_get_player_on_turn_index(worker->game_copy);
    bool conserve = opp_stuck_frac > 0.0F && playout_on_turn == solving_player;

    // Pre-compute voluntary pass penalty for conservation mode.
    // By passing instead of going out, the game heads toward a 6-zero
    // ending where both players lose 1x their rack value, instead of
    // gaining 2x opponent's rack. Cost: (own_rack + opp_rack) * stuck_frac.
    int pass_penalty = 0;
    if (conserve && worker->solver->forced_pass_bypass) {
      const LetterDistribution *ld = game_get_ld(worker->game_copy);
      const Rack *own_rack =
          player_get_rack(game_get_player(worker->game_copy, playout_on_turn));
      const Rack *opp_rack = player_get_rack(
          game_get_player(worker->game_copy, 1 - playout_on_turn));
      pass_penalty =
          (int)((float)(equity_to_int(rack_get_score(ld, own_rack)) +
                        equity_to_int(rack_get_score(ld, opp_rack))) *
                opp_stuck_frac);
    }

    int best_idx = 0;
    int best_adj = INT32_MIN;
    for (int j = 0; j < nplays; j++) {
      const SmallMove *sm = worker->move_list->small_moves[j];
      int score = small_move_get_score(sm);
      int adj;
      if (conserve) {
        if (small_move_is_pass(sm)) {
          adj = score - pass_penalty;
        } else {
          small_move_to_move(worker->move_list->spare_move, sm,
                             game_get_board(worker->game_copy));
          int conservation_bonus = compute_conservation_bonus(
              worker->move_list->spare_move, game_get_ld(worker->game_copy),
              opp_stuck_frac);
          adj = score - conservation_bonus;
        }
      } else {
        adj = score;
      }
      if (adj > best_adj) {
        best_adj = adj;
        best_idx = j;
      }
    }

    SmallMove best_sm = *(worker->move_list->small_moves[best_idx]);
    small_move_to_move(worker->move_list->spare_move, &best_sm,
                       game_get_board(worker->game_copy));

    int undo_slot = plies + playout_depth;
    play_move_incremental(worker->move_list->spare_move, worker->game_copy,
                          &worker->move_undos[undo_slot]);

    // Store move in PV
    if (playout_depth < MAX_VARIANT_LENGTH) {
      pv->moves[playout_depth] = best_sm;
    }
    playout_depth++;
  }

  // Compute final spread from solving_player's perspective
  const Player *sp = game_get_player(worker->game_copy, solving_player);
  const Player *op = game_get_player(worker->game_copy, 1 - solving_player);
  int32_t greedy_spread =
      equity_to_int(player_get_score(sp) - player_get_score(op));

  // End-of-game rack adjustments if game didn't end naturally
  if (game_get_game_end_reason(worker->game_copy) == GAME_END_REASON_NONE) {
    const LetterDistribution *ld = game_get_ld(worker->game_copy);
    int32_t sp_rack =
        equity_to_int(calculate_end_rack_points(player_get_rack(sp), ld));
    int32_t op_rack =
        equity_to_int(calculate_end_rack_points(player_get_rack(op), ld));
    greedy_spread -= sp_rack;
    greedy_spread += op_rack;
  }

  pv->num_moves =
      playout_depth < MAX_VARIANT_LENGTH ? playout_depth : MAX_VARIANT_LENGTH;
  pv->negamax_depth = 0;

  // Unplay all playout moves in reverse
  for (int d = playout_depth - 1; d >= 0; d--) {
    int undo_slot = plies + d;
    unplay_move_incremental(worker->game_copy, &worker->move_undos[undo_slot]);
    const MoveUndo *undo = &worker->move_undos[undo_slot];
    if (undo->move_tiles_length > 0) {
      update_cross_sets_after_unplay_from_undo(undo, worker->game_copy);
      board_set_cross_sets_valid(game_get_board(worker->game_copy), true);
    }
  }

  if (playout_interrupted) {
    return ABDADA_INTERRUPTED;
  }

  // Store greedy playout result in TT at depth 0.
  // Only store if no deeper entry exists (prefer deeper negamax results).
  if (worker->solver->transposition_table_optim) {
    TTEntry existing = transposition_table_lookup(
        worker->solver->transposition_table, node_key);
    if (!ttentry_valid(existing) || ttentry_depth(existing) == 0) {
      int32_t greedy_result =
          (on_turn_idx == solving_player) ? greedy_spread : -greedy_spread;
      int16_t tt_score = (int16_t)(greedy_result - on_turn_spread);
      TTEntry entry_to_store = {.score = tt_score,
                                .flag_and_depth = (TT_EXACT << 6),
                                .tiny_move = INVALID_TINY_MOVE};
      transposition_table_store(worker->solver->transposition_table, node_key,
                                entry_to_store);
    }
  }

  // Convert from solving_player's perspective to on_turn perspective
  if (on_turn_idx == solving_player) {
    return greedy_spread;
  }
  return -greedy_spread;
}

// Compute TT flag (UPPER/LOWER/EXACT) and store entry at end of search.
static void negamax_tt_store(const EndgameSolverWorker *worker,
                             uint64_t node_key, int depth, int32_t best_value,
                             int32_t alpha_orig, int32_t beta,
                             int32_t on_turn_spread, uint64_t best_tiny_move) {
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

// Stuck-tile detection, move generation, logging, and sorting for non-root
// nodes. Updates *opp_stuck_frac. Returns move count, or -1 if interrupted.
static int negamax_generate_and_sort_moves(EndgameSolverWorker *worker,
                                           int depth, uint64_t tt_move,
                                           float *opp_stuck_frac) {
  int opp_idx = 1 - worker->solver->solving_player;
  uint64_t opp_tiles_bv = 0;
  int nplays;
  if (worker->solver->use_heuristics) {
    *opp_stuck_frac = compute_opp_stuck_fraction(
        worker->game_copy, worker->move_list,
        solver_get_pruned_kwg(worker->solver, opp_idx), opp_idx,
        worker->thread_index, worker->solver->cross_set_precheck, &opp_tiles_bv,
        worker->solver);
    // Check for interrupt between the two expensive operations so threads
    // don't run a second full movegen after the timer has already fired.
    if (iterative_deepening_should_stop(worker->solver)) {
      return -1;
    }
    nplays = generate_stm_plays(worker, depth);
  } else {
    nplays = generate_stm_plays(worker, depth);
    *opp_stuck_frac = 0.0F;
  }

  // Log stuck-tile activation once per solve
  if (*opp_stuck_frac > 0.0F) {
    int expected = 0;
    if (atomic_compare_exchange_strong(&worker->solver->stuck_tile_logged,
                                       &expected, 1)) {
      const Rack *opp_rack =
          player_get_rack(game_get_player(worker->game_copy, opp_idx));
      const LetterDistribution *log_ld = game_get_ld(worker->game_copy);
      int ld_size = ld_get_size(log_ld);
      StringBuilder *stuck_sb = string_builder_create();
      string_builder_add_formatted_string(stuck_sb, "Player %d (", opp_idx + 1);
      string_builder_add_rack(stuck_sb, opp_rack, log_ld, false);
      string_builder_add_string(stuck_sb, ") stuck tiles: ");
      for (int ml = 0; ml < ld_size; ml++) {
        int count = rack_get_letter(opp_rack, ml);
        if (count > 0 && !(opp_tiles_bv & ((uint64_t)1 << ml))) {
          for (int k = 0; k < count; k++) {
            string_builder_add_user_visible_letter(stuck_sb, log_ld, ml);
          }
        }
      }
      log_warn("Stuck-tile mode (%.0f%%): %s",
               (double)(*opp_stuck_frac * 100.0F),
               string_builder_peek(stuck_sb));
      string_builder_destroy(stuck_sb);
    }
  }

  assign_estimates_and_sort(worker, nplays, tt_move, *opp_stuck_frac);
  return nplays;
}

// Checks whether the per-depth deadline has been exceeded and signals all
// workers to stop if so. Marked noinline to keep struct timespec off the hot
// abdada_negamax stack frame — deep searches (25-ply) would otherwise
// overflow the stack under ASAN's enlarged frames.
__attribute__((noinline)) static bool
check_depth_deadline(EndgameSolverWorker *worker) {
  int64_t deadline_ns = atomic_load_explicit(&worker->solver->depth_deadline_ns,
                                             memory_order_relaxed);
  if (deadline_ns == 0) {
    return false;
  }
  int64_t now_ns = ctimer_monotonic_ns();
  if (now_ns > deadline_ns) {
    atomic_store(&worker->solver->search_complete, 1);
    return true;
  }
  return false;
}

int32_t abdada_negamax(EndgameSolverWorker *worker, uint64_t node_key,
                       int depth, int32_t alpha, int32_t beta, PVLine *pv,
                       bool pv_node, bool exclusive_p, float opp_stuck_frac) {

  assert(pv_node || alpha == beta - 1);

  if (iterative_deepening_should_stop(worker->solver)) {
    return ABDADA_INTERRUPTED;
  }

  // Per-depth deadline check: bail mid-depth if we're running over the EBF
  // estimate. Throttled to every DEPTH_DEADLINE_CHECK_INTERVAL nodes per worker
  // to amortize the clock_gettime call. The actual check is in a noinline
  // helper so struct timespec does not land on every abdada_negamax frame —
  // deep searches (e.g. 25-ply) would otherwise overflow the stack.
  if (++worker->nodes_since_deadline_check >= DEPTH_DEADLINE_CHECK_INTERVAL) {
    worker->nodes_since_deadline_check = 0;
    if (check_depth_deadline(worker)) {
      return ABDADA_INTERRUPTED;
    }
  }

  // ABDADA: if exclusive search and another processor is on this node, defer.
  // Active at depth >= 3 where the cost of redundant search justifies the
  // protocol overhead. At shallower depths, nproc table collisions cause
  // excessive false deferrals.
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
    if (worker->solver->use_heuristics &&
        game_get_game_end_reason(worker->game_copy) == GAME_END_REASON_NONE) {
      return negamax_greedy_leaf_playout(worker, node_key, on_turn_idx,
                                         on_turn_spread, pv, opp_stuck_frac);
    }
    return on_turn_spread;
  }

  PVLine child_pv;
  child_pv.game = worker->game_copy;
  child_pv.num_moves = 0;

  int nplays;
  bool arena_alloced = false;
  if (worker->current_iterative_deepening_depth != depth) {
    // Check interrupt before entering expensive movegen.  A thread that just
    // passed the check at the top of this function may have had the interrupt
    // fire during the TT-lookup / spread-calculation window above; catching it
    // here avoids running two full movegens unnecessarily.
    if (iterative_deepening_should_stop(worker->solver)) {
      if (abdada_active) {
        transposition_table_leave_node(worker->solver->transposition_table,
                                       node_key);
      }
      return ABDADA_INTERRUPTED;
    }
    nplays = negamax_generate_and_sort_moves(worker, depth, tt_move,
                                             &opp_stuck_frac);
    if (nplays == -1) {
      // Interrupted during compute_opp_stuck_fraction
      if (abdada_active) {
        transposition_table_leave_node(worker->solver->transposition_table,
                                       node_key);
      }
      return ABDADA_INTERRUPTED;
    }
    arena_alloced = true;
  } else {
    // Use initial moves. They already have been sorted by estimated value.
    nplays = worker->n_initial_moves;
  }

  // Forced-pass fast path: when the only legal move is pass,
  // play it without consuming a depth ply. Since a forced pass is
  // deterministic (no branching), it shouldn't cost search depth.
  // Guard: only when previous move scored (consecutive_scoreless_turns == 0)
  // to avoid non-termination in mutual-pass endgames.
  if (worker->solver->forced_pass_bypass && arena_alloced && nplays == 1 &&
      game_get_consecutive_scoreless_turns(worker->game_copy) == 0) {
    size_t fp_offset = worker->small_move_arena->size - sizeof(SmallMove);
    const SmallMove *only_move =
        (const SmallMove *)(worker->small_move_arena->memory + fp_offset);
    if (small_move_is_pass(only_move)) {
      // Save move data before arena may be reallocated during recursion
      SmallMove pass_move = *only_move;
      const Board *board = game_get_board(worker->game_copy);
      small_move_to_move(worker->move_list->spare_move, &pass_move, board);

      int last_consecutive_scoreless_turns =
          game_get_consecutive_scoreless_turns(worker->game_copy);

      MoveUndo pass_undo;
      play_move_incremental(worker->move_list->spare_move, worker->game_copy,
                            &pass_undo);

      uint64_t child_key = 0;
      if (worker->solver->transposition_table_optim) {
        const Rack *stm_rack = player_get_rack(player_on_turn);
        child_key = zobrist_add_move(
            worker->solver->transposition_table->zobrist, node_key,
            worker->move_list->spare_move, stm_rack,
            on_turn_idx == worker->solver->solving_player,
            game_get_consecutive_scoreless_turns(worker->game_copy),
            last_consecutive_scoreless_turns);
      }

      // Recurse at SAME depth (forced pass doesn't consume depth)
      child_pv.num_moves = 0;
      int32_t value = abdada_negamax(worker, child_key, depth, -beta, -alpha,
                                     &child_pv, pv_node, false, opp_stuck_frac);

      unplay_move_incremental(worker->game_copy, &pass_undo);

      arena_dealloc(worker->small_move_arena, sizeof(SmallMove));

      if (abdada_active) {
        transposition_table_leave_node(worker->solver->transposition_table,
                                       node_key);
      }

      if (value == ABDADA_INTERRUPTED) {
        return ABDADA_INTERRUPTED;
      }

      int32_t pass_best = -value;
      pvline_update(pv, &child_pv, &pass_move,
                    pass_best - worker->solver->initial_spread);
      pv->negamax_depth = child_pv.negamax_depth + 1;

      if (worker->solver->transposition_table_optim) {
        negamax_tt_store(worker, node_key, depth, pass_best, alpha_orig, beta,
                         on_turn_spread, pass_move.tiny_move);
      }

      return pass_best;
    }
  }

  int32_t best_value = -LARGE_VALUE;
  uint64_t best_tiny_move = INVALID_TINY_MOVE;
  size_t arena_offset =
      worker->small_move_arena->size - (sizeof(SmallMove) * nplays);

  // Multi-PV: track top-K values at root to widen alpha
  const bool is_root = (worker->current_iterative_deepening_depth == depth);
  const bool is_ply2 =
      (worker->current_iterative_deepening_depth - 1 == depth) &&
      worker->thread_index == 0 && worker->in_first_root_move;
  if (is_ply2) {
    atomic_store(&worker->solver->ply2_moves_total, nplays);
    atomic_store(&worker->solver->ply2_moves_completed, 0);
  }
  const int multi_pv_k = worker->solver->solve_multiple_variations;
  const bool multi_pv = is_root && multi_pv_k > 1;
  int32_t topk_values[MAX_VARIANT_LENGTH];
  int topk_n = 0;

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

  // ABDADA two-phase iteration:
  // Pass 0: search all moves with exclusion (defer busy nodes).
  // Pass 1+: retry only deferred moves without exclusion.
  int pass = 0;
  bool all_done = false;
  while (!all_done) {
    all_done = true;

    for (int idx = 0; idx < nplays; idx++) {
      // After the first pass, skip moves that aren't deferred
      // (they were already successfully searched)
      if (pass > 0 && deferred != NULL && !deferred[idx]) {
        continue;
      }

      // ABDADA: determine if this move should be searched exclusively
      // First move (idx == 0) is never exclusive
      // In first phase, other moves are exclusive
      // In retry phase (deferred[idx] == true), moves are not exclusive
      bool child_exclusive = use_abdada && (idx > 0) && !deferred[idx];

      size_t element_offset = arena_offset + idx * sizeof(SmallMove);
      SmallMove *small_move =
          (SmallMove *)(worker->small_move_arena->memory + element_offset);
      small_move_to_move(worker->move_list->spare_move, small_move,
                         game_get_board(worker->game_copy));

      const Rack *stm_rack = player_get_rack(player_on_turn);
      const int stm_rack_tiles = rack_get_total_letters(stm_rack);
      const bool is_outplay =
          small_move_get_tiles_played(small_move) == stm_rack_tiles;

      // Track whether thread 0 is inside root move #1's subtree
      if (is_root && worker->thread_index == 0 && pass == 0) {
        worker->in_first_root_move = (idx == 0);
      }

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

      uint64_t child_key = 0;
      if (worker->solver->transposition_table_optim) {
        child_key = zobrist_add_move(
            worker->solver->transposition_table->zobrist, node_key,
            worker->move_list->spare_move, stm_rack,
            on_turn_idx == worker->solver->solving_player,
            game_get_consecutive_scoreless_turns(worker->game_copy),
            last_consecutive_scoreless_turns);
      }

      // Per-root-move aspiration: at root after depth 1, each move gets its
      // own aspiration window centered on its estimated_value from the previous
      // ID iteration. This gives accurate values for all root moves (needed for
      // multi-PV) while still benefiting from narrow windows.
      const bool use_root_aspiration =
          is_root && depth >= 2 && worker->solver->iterative_deepening_optim &&
          !worker->solver->first_win_optim;

      int32_t value = 0;
      if (use_root_aspiration) {
        int32_t est = small_move_get_estimated_value(small_move);
        int32_t window = ASPIRATION_WINDOW;
        int32_t move_alpha = MAX(alpha, est - window);
        int32_t move_beta = MIN(beta, est + window);
        // Ensure valid window
        if (move_alpha >= move_beta) {
          move_alpha = alpha;
          move_beta = beta;
        }

        while (true) {
          value = abdada_negamax(worker, child_key, depth - 1, -move_beta,
                                 -move_alpha, &child_pv, pv_node,
                                 child_exclusive, opp_stuck_frac);
          if (value == ON_EVALUATION || value == ABDADA_INTERRUPTED) {
            break;
          }
          if (-value <= move_alpha && move_alpha > alpha) {
            // Aspiration fail-low: widen alpha toward global alpha
            window *= 2;
            move_alpha =
                (window >= LARGE_VALUE / 2) ? alpha : MAX(alpha, est - window);
            pvline_clear(&child_pv);
          } else if (-value >= move_beta && move_beta < beta) {
            // Aspiration fail-high: widen beta toward global beta
            window *= 2;
            move_beta =
                (window >= LARGE_VALUE / 2) ? beta : MIN(beta, est + window);
            pvline_clear(&child_pv);
          } else {
            break;
          }
        }
      } else if (idx == 0 || !worker->solver->negascout_optim || is_root) {
        value =
            abdada_negamax(worker, child_key, depth - 1, -beta, -alpha,
                           &child_pv, pv_node, child_exclusive, opp_stuck_frac);
      } else {
        value =
            abdada_negamax(worker, child_key, depth - 1, -alpha - 1, -alpha,
                           &child_pv, false, child_exclusive, opp_stuck_frac);
        if (value != ABDADA_INTERRUPTED && value != ON_EVALUATION &&
            alpha < -value && -value < beta) {
          // re-search with wider window (not exclusive since we need the value)
          value = abdada_negamax(worker, child_key, depth - 1, -beta, -alpha,
                                 &child_pv, pv_node, false, opp_stuck_frac);
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

      if (value == ABDADA_INTERRUPTED) {
        all_done = true;
        best_value = ABDADA_INTERRUPTED;
        break;
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
        pv->negamax_depth = child_pv.negamax_depth + 1;
      }
      if (is_root) {
        // At the very top depth, set the estimated value of the small move,
        // for the next iterative deepening iteration.
        small_move_set_estimated_value(small_move, -value);
        // Track root move progress (thread 0 only, for external polling).
        // Count on any pass so ABDADA deferred moves that complete on pass 1+
        // are not missed.
        if (worker->thread_index == 0) {
          atomic_fetch_add(&worker->solver->root_moves_completed, 1);
        }
      }
      if (is_ply2) {
        atomic_fetch_add(&worker->solver->ply2_moves_completed, 1);
      }
      if (multi_pv) {
        // Multi-PV: insert value into sorted top-K array and set alpha to
        // the Kth-best value so top K moves all get full-width searches.
        int32_t kth = topk_insert(topk_values, &topk_n, multi_pv_k, -value);
        alpha = MAX(alpha_orig, kth);
      } else {
        alpha = MAX(alpha, best_value);
      }
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

    pass++;

    // ABDADA: yield when we still have deferred moves so other threads
    // can make progress.
    if (!all_done) {
      compat_sched_yield();
    }
  }

  // Clean up deferred array (only if heap-allocated)
  if (deferred_heap_allocated) {
    free(deferred);
  }

  if (worker->solver->transposition_table_optim &&
      best_value != ABDADA_INTERRUPTED) {
    negamax_tt_store(worker, node_key, depth, best_value, alpha_orig, beta,
                     on_turn_spread, best_tiny_move);
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

static bool iterative_deepening_should_stop(EndgameSolver *solver) {
  return atomic_load(&solver->search_complete) != 0 ||
         thread_control_get_status(solver->thread_control) ==
             THREAD_CONTROL_STATUS_USER_INTERRUPT;
}

// Build top-K ranked PVLines from root SmallMoves (with TT extension) and
// invoke per_ply_callback. Display/callback plumbing only.
static void build_ranked_pvs_and_notify(EndgameSolverWorker *worker, int depth,
                                        int32_t pv_value,
                                        const PVLine *extended_pv,
                                        const SmallMove *initial_moves,
                                        int initial_move_count) {
  enum { MAX_RANKED_CALLBACK_PVS = 10 };
  int n_ranked = initial_move_count < MAX_RANKED_CALLBACK_PVS
                     ? initial_move_count
                     : MAX_RANKED_CALLBACK_PVS;
  PVLine ranked_pvs[MAX_RANKED_CALLBACK_PVS];
  for (int r = 0; r < n_ranked; r++) {
    PVLine *rpv = &ranked_pvs[r];
    rpv->moves[0] = initial_moves[r];
    rpv->num_moves = 1;
    rpv->score = small_move_get_estimated_value(&initial_moves[r]) -
                 worker->solver->initial_spread;
    rpv->negamax_depth = 1;
    rpv->game = NULL;
    // Extend from TT
    if (worker->solver->transposition_table_optim) {
      Game *ext_game = game_duplicate(worker->game_copy);
      pvline_extend_from_tt(rpv, ext_game, worker->solver->transposition_table,
                            worker->solver->solving_player,
                            worker->solver->requested_plies);
      game_destroy(ext_game);
    }
  }

  worker->solver->per_ply_callback(depth, pv_value, extended_pv,
                                   worker->game_copy, ranked_pvs, n_ranked,
                                   worker->solver->per_ply_callback_data);
}

void iterative_deepening(EndgameSolverWorker *worker, int plies) {

  int32_t alpha = -LARGE_VALUE;
  int32_t beta = LARGE_VALUE;
  int32_t prev_value = 0;
  Timer ids_timer;
  ctimer_start(&ids_timer);
  double prev_depth_time = 0.0;
  double prev_prev_depth_time = 0.0;
  double ema_ebf_per_ply = -1.0; // -1 = not yet seeded
  bool use_aspiration = (worker->solver->threads > 1);

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

  // Half the threads use stuck-tile-aware root ordering (build chains +
  // conservation), the other half use score-based ordering for diversity.
  float initial_opp_stuck_frac = (worker->thread_index % 2 == 0)
                                     ? worker->solver->initial_opp_stuck_frac
                                     : 0.0F;

  int initial_move_count =
      generate_stm_plays(worker, worker->solver->requested_plies);
  // Arena pointer better have started at 0, since it was empty.
  assign_estimates_and_sort(worker, initial_move_count, INVALID_TINY_MOVE,
                            initial_opp_stuck_frac);
  worker->n_initial_moves = initial_move_count;
  assert((size_t)worker->small_move_arena->size ==
         initial_move_count * sizeof(SmallMove));

  worker->current_iterative_deepening_depth = 1;
  int start = 1;
  if (!worker->solver->iterative_deepening_optim) {
    start = plies;
  }

  // ABDADA depth jitter: spread threads across different starting depths
  // so they don't all compete on the same shallow levels simultaneously.
  // Thread 0 always starts at depth 1 (main thread, provides per-ply callback).
  // Other threads start at depth 1 + (index % depth_spread), clamped to plies.
  const int num_threads = worker->solver->threads;
  if (num_threads > 1 && worker->thread_index > 0) {
    // Spread across up to 4 different starting depths to reduce contention.
    // With 16 threads: 1 at d1 (thread 0), ~4 at d2, ~4 at d3, ~4 at d4, ~3 at
    // d5
    int depth_offset = 1 + (worker->thread_index % MIN(4, plies - 1));
    start = MIN(depth_offset, plies);
  }

  for (int ply = start; ply <= plies; ply++) {
    // Check if another thread has completed the full search
    if (iterative_deepening_should_stop(worker->solver)) {
      break;
    }

    worker->current_iterative_deepening_depth = ply;
    // Update root move progress counters (thread 0 only)
    if (worker->thread_index == 0) {
      atomic_store(&worker->solver->current_depth, ply);
      atomic_store(&worker->solver->root_moves_completed, 0);
      atomic_store(&worker->solver->root_moves_total, worker->n_initial_moves);
      atomic_store(&worker->solver->ply2_moves_completed, 0);
      atomic_store(&worker->solver->ply2_moves_total, 0);
      worker->in_first_root_move = false;
    }
    double depth_start_time = ctimer_elapsed_seconds(&ids_timer);
    PVLine pv;
    pv.game = worker->game_copy;
    pv.num_moves = 0;

    int32_t val;

    // Track whether this depth's search completed validly
    bool search_valid = true;

    // Use aspiration windows after depth 1
    if (use_aspiration && ply > 1 && !worker->solver->first_win_optim) {
      int32_t window = ASPIRATION_WINDOW;
      alpha = prev_value - window;
      beta = prev_value + window;

      // Search with narrow window, widen on fail-high/fail-low
      while (true) {
        // Check if another thread completed
        if (iterative_deepening_should_stop(worker->solver)) {
          search_valid = false;
          break;
        }

        val = abdada_negamax(worker, initial_hash_key, ply, alpha, beta, &pv,
                             true, false, initial_opp_stuck_frac);

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
      val = abdada_negamax(worker, initial_hash_key, ply, alpha, beta, &pv,
                           true, false, initial_opp_stuck_frac);
    }

    // If search was interrupted, discard this depth and use last complete depth
    bool interrupted = !search_valid || val == ABDADA_INTERRUPTED;
    if (interrupted) {
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
    pv.score = pv_value;
    worker->best_pv = pv;
    worker->completed_depth = ply;

    endgame_results_set_best_pvline(worker->solver->results, &pv, pv_value,
                                    ply);

    // Call per-ply callback (only thread 0 to avoid race conditions)
    if (worker->thread_index == 0 && worker->solver->per_ply_callback) {
      // Extend PV from TT + greedy playout for display
      PVLine extended_pv = pv;
      if (worker->solver->transposition_table_optim) {
        Game *temp_game = game_duplicate(worker->game_copy);
        pvline_extend_from_tt(
            &extended_pv, temp_game, worker->solver->transposition_table,
            worker->solver->solving_player, worker->solver->requested_plies);
        game_destroy(temp_game);
      }

      build_ranked_pvs_and_notify(worker, ply, pv_value, &extended_pv,
                                  initial_moves, initial_move_count);
    }

    // EBF-based time management: decide whether to start the next depth.
    // Only thread 0 checks (other threads follow via search_complete signal).
    // Require a minimum depth before applying any time management so the solver
    // always produces a reasonably deep answer before EBF/soft limit can stop
    // it — prevents shallow-depth artifacts (e.g. spurious PASS at d3).
    // min_depth=4: the d1/d3 EBF ratio is unreliable because the cross-set
    // precheck prunes very heavily at shallow depths, making d1 anomalously
    // fast. Starting at d4 (which uses the d2/d4 ratio) gives a stable base.
    const int min_depth_for_time_mgmt = 4;
    if (worker->thread_index == 0 && worker->solver->soft_time_limit > 0) {
      double elapsed = ctimer_elapsed_seconds(&ids_timer);
      double this_depth_time = elapsed - depth_start_time;
      if (ply >= min_depth_for_time_mgmt) {
        if (elapsed >= worker->solver->soft_time_limit) {
          // Past soft limit — stop immediately, bank remaining time
          atomic_store(&worker->solver->search_complete, 1);
          break;
        }
        // Estimate time for next depth using 2-ply EBF: sqrt(t[d]/t[d-2]).
        // More stable than 1-ply EBF across odd/even ply alternation.
        // Apply EMA smoothing: the cross-set precheck produces irregular search
        // trees at shallow depths (heavy pruning early, less so later), making
        // instantaneous EBF estimates volatile. EMA blends the current sample
        // with the historical estimate, smoothing out transient spikes.
        if (prev_prev_depth_time > 0.01 && this_depth_time > 0.01) {
          double ebf2 = this_depth_time / prev_prev_depth_time;
          double ebf_per_ply = sqrt(ebf2);
          const double ema_alpha = 0.4;
          if (ema_ebf_per_ply < 0.0) {
            ema_ebf_per_ply = ebf_per_ply; // seed on first valid sample
          } else {
            ema_ebf_per_ply =
                ema_alpha * ebf_per_ply + (1.0 - ema_alpha) * ema_ebf_per_ply;
          }
          double estimated_next = this_depth_time * ema_ebf_per_ply;
          if (elapsed + estimated_next > worker->solver->hard_time_limit) {
            // Next depth would likely exceed hard limit — stop now
            atomic_store(&worker->solver->search_complete, 1);
            break;
          }
          // Mid-depth bail: set a per-depth deadline so workers abort if a
          // depth runs far beyond its EBF estimate.  Only activate in the
          // final stretch (elapsed >= 75% of hard limit).  Below that
          // threshold the solver still has many depths ahead of it and the
          // EBF estimate is likely noisy at shallow depths; bailing early
          // would drop back to a shallower result unnecessarily.  Above 75%,
          // we are near the last depth we will attempt, so a runaway depth
          // directly starves subsequent turns.  Clear any stale deadline when
          // we are below the threshold so workers don't fire spuriously.
          if (elapsed >= 0.75 * worker->solver->hard_time_limit) {
            int64_t now_ns = ctimer_monotonic_ns();
            int64_t budget_ns = (int64_t)(estimated_next * 1.5e9);
            atomic_store(&worker->solver->depth_deadline_ns,
                         now_ns + budget_ns);
          } else {
            atomic_store(&worker->solver->depth_deadline_ns, 0);
          }
        }
      }
      prev_prev_depth_time = prev_depth_time;
      prev_depth_time = this_depth_time;
    }

    // Signal other threads to stop when we complete the full search
    if (ply == plies) {
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

// Compute the initial stuck-tile fraction for the opponent at the root.
// Duplicates the game to avoid modifying the original.
static float compute_initial_stuck_fraction(const EndgameSolver *solver,
                                            const Game *game) {
  int opp_idx = 1 - solver->solving_player;
  Game *root_game = game_duplicate(game);
  MoveList *tmp_ml = move_list_create_small(DEFAULT_ENDGAME_MOVELIST_CAPACITY);
  float frac = compute_opp_stuck_fraction(
      root_game, tmp_ml, solver_get_pruned_kwg(solver, opp_idx), opp_idx, 0,
      solver->cross_set_precheck, NULL, NULL);
  small_move_list_destroy(tmp_ml);
  game_destroy(root_game);
  return frac;
}

// Format and log all final PV lines: move-by-move replay, game-end
// annotations (rack points, 6 zeros), win/loss/tie summary.
static void log_final_pvs(const PVLine *multi_pvs, int num_pvs,
                          const EndgameSolver *solver, const Game *game,
                          double elapsed) {
  const LetterDistribution *ld = game_get_ld(game);
  const int on_turn = game_get_player_on_turn_index(game);
  const int p1_score =
      equity_to_int(player_get_score(game_get_player(game, 0)));
  const int p2_score =
      equity_to_int(player_get_score(game_get_player(game, 1)));

  for (int pv_idx = 0; pv_idx < num_pvs; pv_idx++) {
    const PVLine *pv = &multi_pvs[pv_idx];
    StringBuilder *final_sb = string_builder_create();
    Move move;
    Game *final_game_copy = game_duplicate(game);

    if (num_pvs > 1) {
      string_builder_add_formatted_string(
          final_sb,
          "FINAL %d/%d: depth=%d, value=%d, time=%.3fs, pv=", pv_idx + 1,
          num_pvs, solver->requested_plies, pv->score, elapsed);
    } else {
      string_builder_add_formatted_string(
          final_sb,
          "FINAL: depth=%d, value=%d, time=%.3fs, pv=", solver->requested_plies,
          pv->score, elapsed);
    }

    for (int i = 0; i < pv->num_moves; i++) {
      small_move_to_move(&move, &pv->moves[i], game_get_board(final_game_copy));
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
      if (i < pv->num_moves - 1) {
        // Insert | between exact (negamax) and greedy moves
        if (i + 1 == pv->negamax_depth && pv->negamax_depth > 0) {
          string_builder_add_string(final_sb, " |");
        }
        string_builder_add_string(final_sb, " ");
      }
    }
    // Append [PX wins by Y]
    {
      int final_spread =
          (p1_score - p2_score) + (on_turn == 0 ? pv->score : -pv->score);
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
  }
}

// Read root SmallMoves from best thread's arena, swap PV move to front,
// build PVLines with TT extension for non-best root moves. Returns number
// of PVs filled. multi_pvs[0] must already be set by caller.
static int extract_multi_pvs(const EndgameSolver *solver,
                             EndgameSolverWorker *best_worker, const Game *game,
                             PVLine *multi_pvs, int num_top) {
  int n_root = best_worker->n_initial_moves;
  int k = (num_top < n_root) ? num_top : n_root;
  SmallMove *root_moves = (SmallMove *)best_worker->small_move_arena->memory;

  // Root moves are already sorted by estimated_value descending from the
  // final qsort in iterative_deepening. The estimated_value for each root
  // move is the actual negamax return value set during the search
  // (line small_move_set_estimated_value(small_move, -value) at root).

  // Ensure the PV's first move is at root_moves[0] to avoid duplicates.
  // qsort is not stable, so tied values may place the PV move elsewhere.
  uint64_t pv_tiny = solver->principal_variation.moves[0].tiny_move;
  if (root_moves[0].tiny_move != pv_tiny) {
    for (int r = 1; r < n_root; r++) {
      if (root_moves[r].tiny_move == pv_tiny) {
        SmallMove tmp = root_moves[0];
        root_moves[0] = root_moves[r];
        root_moves[r] = tmp;
        break;
      }
    }
  }

  // Build PVLines for non-best root moves (r=1..k-1).
  // PV[0] is already set from the search-tracked principal variation above.
  for (int r = 1; r < k; r++) {
    PVLine *pv = &multi_pvs[r];
    pv->moves[0] = root_moves[r];
    pv->num_moves = 1;
    pv->score =
        small_move_get_estimated_value(&root_moves[r]) - solver->initial_spread;
    pv->negamax_depth = 1;
    pv->game = NULL;

    if (solver->transposition_table) {
      Game *ext_game = game_duplicate(game);
      pvline_extend_from_tt(pv, ext_game, solver->transposition_table,
                            solver->solving_player, solver->requested_plies);
      game_destroy(ext_game);
    }
  }
  return k;
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

  solver->results = results;
  endgame_solver_reset(solver, endgame_args);

  Timer solve_timer;
  ctimer_start(&solve_timer);

  // Compute initial stuck-tile fraction for root move ordering.
  // Per-node detection in abdada_negamax recomputes this dynamically.
  solver->initial_opp_stuck_frac = 0.0F;
  if (solver->use_heuristics) {
    solver->initial_opp_stuck_frac =
        compute_initial_stuck_fraction(solver, endgame_args->game);
  }

  // Generate base seed for ABDADA jitter using current time
  uint64_t base_seed = (uint64_t)ctime_get_current_time();

  // Kick-off iterative deepening threads (ABDADA)
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

  // The endgame_results already tracks the best PV on the fly (updated by
  // each worker after completing each depth). Read it back now that all
  // threads have joined.
  const PVLine *best_pv =
      endgame_results_get_pvline(results, ENDGAME_RESULT_BEST);
  solver->principal_variation = *best_pv;

  // Multi-PV: extract top-K root moves from best thread's arena before
  // destroying workers.
  const int num_top = solver->solve_multiple_variations;
  int num_pvs = 1;
  PVLine multi_pvs[MAX_VARIANT_LENGTH];
  multi_pvs[0] = solver->principal_variation;

  if (num_top > 1) {
    // Find the thread that completed the deepest search for its root move arena
    int best_thread = 0;
    int best_depth = solver_workers[0]->completed_depth;
    for (int thread_index = 1; thread_index < solver->threads; thread_index++) {
      int thread_depth = solver_workers[thread_index]->completed_depth;
      if (thread_depth > best_depth) {
        best_depth = thread_depth;
        best_thread = thread_index;
      }
    }
    num_pvs = extract_multi_pvs(solver, solver_workers[best_thread],
                                endgame_args->game, multi_pvs, num_top);
  }

  // Clean up workers
  for (int thread_index = 0; thread_index < solver->threads; thread_index++) {
    solver_worker_destroy(solver_workers[thread_index]);
  }

  free(solver_workers);
  free(worker_ids);

  // Calculate elapsed time
  double elapsed = ctimer_elapsed_seconds(&solve_timer);

  // Extend best PV with TT + greedy playout for display (single-PV case).
  // Skip when interrupted by timer — extension calls generate_moves per ply
  // and would add seconds of post-search overhead.
  bool interrupted = thread_control_get_status(solver->thread_control) ==
                     THREAD_CONTROL_STATUS_USER_INTERRUPT;
  if (!interrupted && num_top <= 1 && solver->transposition_table) {
    Game *ext_game = game_duplicate(endgame_args->game);
    pvline_extend_from_tt(&solver->principal_variation, ext_game,
                          solver->transposition_table, solver->solving_player,
                          solver->requested_plies);
    game_destroy(ext_game);
    multi_pvs[0] = solver->principal_variation;
  }

  if (!interrupted) {
    log_final_pvs(multi_pvs, num_pvs, solver, endgame_args->game, elapsed);
  }
}
