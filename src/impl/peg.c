#include "peg.h"

#include "../compat/cpthread.h"
#include "../compat/ctime.h"
#include "../def/board_defs.h"
#include "../def/equity_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/move_defs.h"
#include "../ent/bag.h"
#include "../ent/board.h"
#include "../ent/endgame_results.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/move_undo.h"
#include "../ent/player.h"
#include "../ent/rack.h"
#include "../str/letter_distribution_string.h"
#include "../str/move_string.h"
#include "../str/rack_string.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "endgame.h"
#include "gameplay.h"
#include "move_gen.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

// PEG_MOVELIST_CAPACITY: capacity for move list used during move generation.

enum {
  PEG_MOVELIST_CAPACITY = 250000,
};

struct PegSolver {
  int _unused;
};

// ---------------------------------------------------------------------------
// Candidate tracking
// ---------------------------------------------------------------------------

typedef struct PegCandidate {
  Move move;
  // Win fraction in [0,1]: win=1, tie=0.5, loss=0 averaged over bag scenarios.
  // When pruned is true, this is the upper bound (best possible win_pct).
  double win_pct;
  // Expected spread from mover's perspective, averaged over bag scenarios.
  double expected_value;
  // True if evaluation was cut short because this candidate cannot possibly
  // reach the cutoff_k-th best win_pct.
  bool pruned;
} PegCandidate;

// ---------------------------------------------------------------------------
// Cutoff tracker for early pruning
// ---------------------------------------------------------------------------

// Tracks the K-th best win_pct among completed candidates. Threads update
// this after each candidate finishes evaluation; the current cutoff is read
// (without locking) during per-scenario evaluation to prune hopeless
// candidates.
typedef struct PegCutoff {
  cpthread_mutex_t mutex;
  double *completed; // win_pcts of completed (non-pruned) candidates
  int num_completed;
  int capacity;
  int cutoff_k;              // position threshold (K-th best matters)
  int total_weight;          // total bag-tile weight (sum of unseen counts)
  _Atomic double cutoff;     // current K-th best win_pct, -1 initially
} PegCutoff;

static void peg_cutoff_init(PegCutoff *pc, int cutoff_k, int total_weight,
                            int capacity) {
  cpthread_mutex_init(&pc->mutex);
  pc->completed = malloc_or_die(capacity * sizeof(double));
  pc->num_completed = 0;
  pc->capacity = capacity;
  pc->cutoff_k = cutoff_k;
  pc->total_weight = total_weight;
  atomic_init(&pc->cutoff, -1.0);
}

static void peg_cutoff_destroy(PegCutoff *pc) {
  free(pc->completed);
}

static int compare_doubles_desc(const void *a, const void *b) {
  double da = *(const double *)a, db = *(const double *)b;
  return (db > da) ? 1 : (db < da) ? -1 : 0;
}

// Called after a non-pruned candidate completes evaluation.
static void peg_cutoff_update(PegCutoff *pc, double win_pct) {
  cpthread_mutex_lock(&pc->mutex);
  pc->completed[pc->num_completed++] = win_pct;
  if (pc->num_completed >= pc->cutoff_k) {
    // Find the K-th best (sort descending, take [k-1]).
    qsort(pc->completed, pc->num_completed, sizeof(double),
          compare_doubles_desc);
    atomic_store_explicit(&pc->cutoff, pc->completed[pc->cutoff_k - 1],
                          memory_order_relaxed);
  }
  cpthread_mutex_unlock(&pc->mutex);
}

// Read the current cutoff. Returns -1 if not yet established.
static double peg_cutoff_get(const PegCutoff *pc) {
  return atomic_load_explicit(&pc->cutoff, memory_order_relaxed);
}

static int compare_peg_candidates_by_equity_desc(const void *a, const void *b) {
  const PegCandidate *ca = (const PegCandidate *)a;
  const PegCandidate *cb = (const PegCandidate *)b;
  Equity ea = move_get_equity(&ca->move);
  Equity eb = move_get_equity(&cb->move);
  return (eb > ea) ? 1 : (eb < ea) ? -1 : 0;
}

static int compare_peg_candidates_desc(const void *a, const void *b) {
  const PegCandidate *ca = (const PegCandidate *)a;
  const PegCandidate *cb = (const PegCandidate *)b;
  // Primary: win% (higher = better).
  if (cb->win_pct > ca->win_pct + 1e-9)
    return 1;
  if (ca->win_pct > cb->win_pct + 1e-9)
    return -1;
  // Tiebreaker: expected spread (higher = better).
  if (cb->expected_value > ca->expected_value)
    return 1;
  if (cb->expected_value < ca->expected_value)
    return -1;
  return 0;
}

// ---------------------------------------------------------------------------
// 1-ply endgame search with greedy leaf playout
// ---------------------------------------------------------------------------

// Performs 1-ply exact search: generates all moves for the side to move,
// plays each, evaluates with negamax_greedy_leaf_playout, unplays, and
// returns the best value. This avoids the pure-greedy problem of setting
// up bingo spots for the opponent.
//
// undo_base: first undo slot available for this search (the 1-ply move
//            goes in undo_base, greedy playout starts at undo_base+1).
// on_turn_idx: player index of side to move.
// Returns spread from on_turn_idx's perspective (same convention as
// negamax_greedy_leaf_playout).
static int32_t oneply_endgame_search(EndgameSolverWorker *worker,
                                     int undo_base, int on_turn_idx,
                                     int thread_index, PVLine *best_pv) {
  Game *gc = endgame_solver_worker_get_game(worker);

  if (game_get_game_end_reason(gc) != GAME_END_REASON_NONE) {
    best_pv->num_moves = 0;
    int32_t spread = equity_to_int(
        player_get_score(game_get_player(gc, on_turn_idx)) -
        player_get_score(game_get_player(gc, 1 - on_turn_idx)));
    return spread;
  }

  Board *board = game_get_board(gc);

  MoveList *ml = move_list_create_small(PEG_MOVELIST_CAPACITY);
  {
    const MoveGenArgs gen_args = {
        .game = gc,
        .move_list = ml,
        .move_record_type = MOVE_RECORD_ALL_SMALL,
        .move_sort_type = MOVE_SORT_SCORE,
        .thread_index = thread_index,
        .eq_margin_movegen = 0,
        .target_equity = EQUITY_MAX_VALUE,
        .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
    };
    generate_moves(&gen_args);
  }

  int nplays = ml->count;
  if (nplays == 0) {
    small_move_list_destroy(ml);
    best_pv->num_moves = 0;
    int32_t spread = equity_to_int(
        player_get_score(game_get_player(gc, on_turn_idx)) -
        player_get_score(game_get_player(gc, 1 - on_turn_idx)));
    return spread;
  }

  int saved_plies = endgame_solver_worker_get_requested_plies(worker);
  endgame_solver_worker_set_requested_plies(worker, undo_base + 1);

  int32_t best_val = INT32_MIN;

  for (int j = 0; j < nplays; j++) {
    SmallMove sm = *ml->small_moves[j];
    small_move_to_move(ml->spare_move, &sm, board);

    MoveUndo *undo = endgame_solver_worker_get_move_undo(worker, undo_base);
    move_undo_reset(undo);
    play_move_incremental(ml->spare_move, gc, undo);

    if (!board_get_cross_sets_valid(board)) {
      if (undo->move_tiles_length > 0)
        update_cross_set_for_move_from_undo(undo, gc);
      board_set_cross_sets_valid(board, true);
    }

    PVLine child_pv;
    int opp_idx = 1 - on_turn_idx;
    int32_t child_val = negamax_greedy_leaf_playout(
        worker, 0, opp_idx, 0, &child_pv, 0.0f);
    int32_t val = -child_val;

    if (val > best_val) {
      best_val = val;
      best_pv->moves[0] = sm;
      int child_len =
          child_pv.num_moves < MAX_VARIANT_LENGTH - 1
              ? child_pv.num_moves
              : MAX_VARIANT_LENGTH - 1;
      for (int k = 0; k < child_len; k++)
        best_pv->moves[k + 1] = child_pv.moves[k];
      best_pv->num_moves = 1 + child_len;
      best_pv->negamax_depth = 1;
    }

    unplay_move_incremental(gc, undo);
    if (undo->move_tiles_length > 0) {
      update_cross_sets_after_unplay_from_undo(undo, gc);
      board_set_cross_sets_valid(board, true);
    }
  }

  endgame_solver_worker_set_requested_plies(worker, saved_plies);
  small_move_list_destroy(ml);
  return best_val;
}

// ---------------------------------------------------------------------------
// Unseen tile computation
// ---------------------------------------------------------------------------

// Compute the tiles not in the mover's rack and not on the board.
// With 1 tile in the bag and RACK_SIZE-tile opponent rack, total_unseen == 8
// (for the standard game). Returns total count of unseen tiles.
static int compute_unseen(const Game *game, int mover_idx,
                          uint8_t unseen[MAX_ALPHABET_SIZE]) {
  const LetterDistribution *ld = game_get_ld(game);
  int ld_size = ld_get_size(ld);

  // Start with the full letter distribution.
  for (int ml = 0; ml < ld_size; ml++) {
    unseen[ml] = (uint8_t)ld_get_dist(ld, ml);
  }

  // Subtract mover's known rack.
  const Rack *mover_rack =
      player_get_rack(game_get_player(game, mover_idx));
  for (int ml = 0; ml < ld_size; ml++) {
    unseen[ml] -= (uint8_t)rack_get_letter(mover_rack, ml);
  }

  // Subtract tiles on the board.
  const Board *board = game_get_board(game);
  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      if (board_is_empty(board, row, col))
        continue;
      MachineLetter ml = board_get_letter(board, row, col);
      if (get_is_blanked(ml)) {
        // A blank tile played as a letter: decrement blank count.
        if (unseen[BLANK_MACHINE_LETTER] > 0)
          unseen[BLANK_MACHINE_LETTER]--;
      } else {
        if (unseen[ml] > 0)
          unseen[ml]--;
      }
    }
  }

  int total = 0;
  for (int ml = 0; ml < ld_size; ml++) {
    total += unseen[ml];
  }
  return total;
}

// ---------------------------------------------------------------------------
// Rack helpers
// ---------------------------------------------------------------------------

// Set the opponent's rack to (unseen - {one copy of bag_tile}).
static void set_opp_rack_for_scenario(Rack *opp_rack,
                                      const uint8_t unseen[MAX_ALPHABET_SIZE],
                                      int ld_size, MachineLetter bag_tile) {
  rack_reset(opp_rack);
  for (int ml = 0; ml < ld_size; ml++) {
    int cnt = (int)unseen[ml] - (ml == bag_tile ? 1 : 0);
    for (int k = 0; k < cnt; k++) {
      rack_add_letter(opp_rack, (MachineLetter)ml);
    }
  }
}

// Set the opponent's rack to (unseen - {bag_t1} - {bag_t2}).
static void set_opp_rack_for_scenario_pair(Rack *opp_rack,
                                           const uint8_t unseen[MAX_ALPHABET_SIZE],
                                           int ld_size, MachineLetter bag_t1,
                                           MachineLetter bag_t2) {
  rack_reset(opp_rack);
  for (int ml = 0; ml < ld_size; ml++) {
    int cnt = (int)unseen[ml] - (ml == bag_t1 ? 1 : 0) - (ml == bag_t2 ? 1 : 0);
    for (int k = 0; k < cnt; k++) {
      rack_add_letter(opp_rack, (MachineLetter)ml);
    }
  }
}

// ---------------------------------------------------------------------------
// Greedy evaluation of a play (non-pass) candidate
// ---------------------------------------------------------------------------

// Evaluates a play move by enumerating all possible bag-tile scenarios.
// For 1-in-bag: enumerate each tile T with weight unseen[T].
// For 2-in-bag: enumerate ordered pairs (T1,T2) with weight
//   unseen[T1] * (unseen[T2] - (T1==T2?1:0)).
// For each scenario, plays the candidate, adds drawn tiles to mover's rack,
// sets the opponent's rack, and runs a greedy playout.
// Returns the weighted expected spread from the mover's perspective.
// When use_oneply is true, evaluates each scenario with a 1-ply endgame
// search (opponent generates all moves) instead of a greedy leaf playout.
// This gives the opponent the chance to block bingo setups.
static double peg_greedy_eval_play(EndgameSolverWorker *worker,
                                   const Move *move, int mover_idx,
                                   int opp_idx,
                                   const uint8_t unseen[MAX_ALPHABET_SIZE],
                                   int ld_size, int total_unseen,
                                   int tiles_in_bag,
                                   PegCutoff *cutoff,
                                   double *win_pct_out,
                                   bool *pruned_out,
                                   bool use_oneply,
                                   int thread_index) {
  Game *game_copy = endgame_solver_worker_get_game(worker);
  Board *board = game_get_board(game_copy);
  Rack *mover_rack = player_get_rack(game_get_player(game_copy, mover_idx));
  Rack *opp_rack = player_get_rack(game_get_player(game_copy, opp_idx));

  int tiles_played = move->tiles_played;
  Move m_play;
  move_copy(&m_play, move);
  MoveUndo candidate_undo;
  move_undo_reset(&candidate_undo);
  MoveUndo *candidate_undo_ptr = &candidate_undo;
  play_move_incremental(&m_play, game_copy, candidate_undo_ptr);
  if (game_get_game_end_reason(game_copy) == GAME_END_REASON_STANDARD) {
    Equity end_pts =
        calculate_end_rack_points(opp_rack, game_get_ld(game_copy));
    player_add_to_score(game_get_player(game_copy, mover_idx), -end_pts);
    game_set_game_end_reason(game_copy, GAME_END_REASON_NONE);
  }

  if (!board_get_cross_sets_valid(board)) {
    if (candidate_undo_ptr->move_tiles_length > 0) {
      update_cross_set_for_move_from_undo(candidate_undo_ptr, game_copy);
    }
    board_set_cross_sets_valid(board, true);
  }

  int saved_plies = endgame_solver_worker_get_requested_plies(worker);
  endgame_solver_worker_set_requested_plies(worker, 1);

  double total = 0.0;
  double wins = 0.0;
  int weight = 0;
  PVLine pv;
  bool did_prune = false;

  // Number of tiles mover draws from the bag after playing.
  int draws = tiles_played < tiles_in_bag ? tiles_played : tiles_in_bag;

  if (tiles_in_bag == 1) {
    // 1-in-bag: enumerate single tiles.
    for (int t = 0; t < ld_size; t++) {
      int cnt = (int)unseen[t];
      if (cnt == 0)
        continue;
      rack_add_letter(mover_rack, (MachineLetter)t);
      Rack saved_opp;
      rack_copy(&saved_opp, opp_rack);
      set_opp_rack_for_scenario(opp_rack, unseen, ld_size, (MachineLetter)t);

      int32_t mover_spread;
      bool scenario_oneply = use_oneply &&
          rack_get_total_letters(mover_rack) == RACK_SIZE &&
          has_playable_or_possible_bingo(game_copy, thread_index);
      if (scenario_oneply) {
        mover_spread = oneply_endgame_search(worker, 0, opp_idx,
                                              thread_index, &pv);
      } else {
        mover_spread = -negamax_greedy_leaf_playout(worker, 0, opp_idx, 0,
                                                     &pv, 0.0f);
      }
      total += (double)mover_spread * cnt;
      wins += ((mover_spread > 0) ? 1.0 : (mover_spread == 0 ? 0.5 : 0.0)) * cnt;
      weight += cnt;

      rack_take_letter(mover_rack, (MachineLetter)t);
      rack_copy(opp_rack, &saved_opp);
      if (cutoff) {
        double threshold = peg_cutoff_get(cutoff);
        if (threshold >= 0) {
          int remaining = cutoff->total_weight - weight;
          double best_possible =
              (wins + remaining) / (double)cutoff->total_weight;
          if (best_possible < threshold - 1e-9) {
            did_prune = true;
            break;
          }
        }
      }
    }
  } else {
    // 2-in-bag: enumerate ordered pairs (t1, t2).
    for (int t1 = 0; t1 < ld_size; t1++) {
      if (unseen[t1] == 0)
        continue;
      for (int t2 = 0; t2 < ld_size; t2++) {
        if (unseen[t2] == 0)
          continue;
        if (t1 == t2 && unseen[t1] < 2)
          continue;
        int cnt = (int)unseen[t1] *
                  (t1 == t2 ? (int)unseen[t1] - 1 : (int)unseen[t2]);

        Rack saved_mover, saved_opp;
        rack_copy(&saved_mover, mover_rack);
        rack_copy(&saved_opp, opp_rack);

        // Mover draws min(tiles_played, 2) tiles from {t1, t2}.
        if (draws >= 1)
          rack_add_letter(mover_rack, (MachineLetter)t1);
        if (draws >= 2)
          rack_add_letter(mover_rack, (MachineLetter)t2);

        // Opp gets unseen minus both bag tiles.
        set_opp_rack_for_scenario_pair(opp_rack, unseen, ld_size,
                                       (MachineLetter)t1, (MachineLetter)t2);

        // Use 1-ply endgame if mover has a playable bingo (so opp blocks),
        // otherwise use greedy playout.
        int32_t mover_spread;
        bool scenario_oneply = use_oneply &&
            rack_get_total_letters(mover_rack) == RACK_SIZE &&
            has_playable_or_possible_bingo(game_copy, thread_index);
        if (scenario_oneply) {
          mover_spread = oneply_endgame_search(worker, 0, opp_idx,
                                                thread_index, &pv);
        } else {
          mover_spread = -negamax_greedy_leaf_playout(
              worker, 0, opp_idx, 0, &pv, 0.0f);
        }
        total += (double)mover_spread * cnt;
        wins +=
            ((mover_spread > 0) ? 1.0 : (mover_spread == 0 ? 0.5 : 0.0)) *
            cnt;
        weight += cnt;

        rack_copy(mover_rack, &saved_mover);
        rack_copy(opp_rack, &saved_opp);
        if (cutoff) {
          double threshold = peg_cutoff_get(cutoff);
          if (threshold >= 0) {
            int remaining = cutoff->total_weight - weight;
            double best_possible =
                (wins + remaining) / (double)cutoff->total_weight;
            if (best_possible < threshold - 1e-9) {
              did_prune = true;
              break;
            }
          }
        }
      }
      if (did_prune)
        break;
    }
  }

  endgame_solver_worker_set_requested_plies(worker, saved_plies);

  unplay_move_incremental(game_copy, candidate_undo_ptr);
  if (candidate_undo_ptr->move_tiles_length > 0) {
    update_cross_sets_after_unplay_from_undo(candidate_undo_ptr, game_copy);
    board_set_cross_sets_valid(board, true);
  }

  if (did_prune) {
    *pruned_out = true;
    *win_pct_out =
        (wins + (cutoff->total_weight - weight)) / (double)cutoff->total_weight;
    return total / (weight > 0 ? weight : 1);
  }
  *pruned_out = false;
  if (weight == 0) {
    *win_pct_out = 0.0;
    return 0.0;
  }
  (void)total_unseen;
  *win_pct_out = wins / weight;
  return total / weight;
}

// ---------------------------------------------------------------------------
// Scenario ordering: sort by weight descending for earlier pruning
// ---------------------------------------------------------------------------

typedef struct {
  MachineLetter t1, t2;
  int weight;
} PegPairScenario;

static int compare_pair_scenarios_desc(const void *a, const void *b) {
  return ((const PegPairScenario *)b)->weight -
         ((const PegPairScenario *)a)->weight;
}

typedef struct {
  MachineLetter ml;
  int count;
} PegSingleScenario;

static int compare_single_scenarios_desc(const void *a, const void *b) {
  return ((const PegSingleScenario *)b)->count -
         ((const PegSingleScenario *)a)->count;
}


// ---------------------------------------------------------------------------
// PV logging for a single move across all bag-tile scenarios
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Endgame pass evaluation for a single (candidate, bag_tile) scenario
// ---------------------------------------------------------------------------

// Sets up an endgame position from the base_game:
//   1. Plays the candidate move (bag is empty, so no automatic draw).
//   2. Manually adds bag_tile to the mover's rack.
//   3. Sets the opponent's rack to (unseen - {bag_tile}).
// The resulting game has an empty bag and is ready for endgame_solve.
// Caller must game_destroy the returned game.
static Game *setup_endgame_scenario(const Game *base_game,
                                    const Move *move, int mover_idx,
                                    int opp_idx, MachineLetter bag_tile,
                                    const uint8_t unseen[MAX_ALPHABET_SIZE],
                                    int ld_size) {
  Game *g = game_duplicate(base_game);
  game_set_endgame_solving_mode(g);
  game_set_backup_mode(g, BACKUP_MODE_OFF);

  // Play the candidate onto the board (no draw since base_game bag is empty).
  Move m;
  move_copy(&m, move);
  play_move(&m, g, NULL);
  // play_move may falsely detect game-end when a bingo empties the rack
  // (PEG uses an empty bag but conceptually 1 tile remains). Undo the
  // end-game score adjustment so the endgame solver starts from a clean state.
  if (game_get_game_end_reason(g) == GAME_END_REASON_STANDARD) {
    Equity bonus = calculate_end_rack_points(
        player_get_rack(game_get_player(g, opp_idx)), game_get_ld(g));
    player_add_to_score(game_get_player(g, mover_idx), -bonus);
    game_set_game_end_reason(g, GAME_END_REASON_NONE);
  }

  // Set opponent's rack to (unseen - {bag_tile}).
  Rack *opp_rack = player_get_rack(game_get_player(g, opp_idx));
  set_opp_rack_for_scenario(opp_rack, unseen, ld_size, bag_tile);

  // Mover played tiles: they draw the bag tile to replenish their rack.
  Rack *mover_rack = player_get_rack(game_get_player(g, mover_idx));
  rack_add_letter(mover_rack, bag_tile);

  return g;
}

// Helper: evaluate one endgame scenario (bag empty, all tiles known).
// Returns mover's final spread.
static int32_t peg_eval_endgame_scenario(EndgameSolver *endgame_solver,
                                         EndgameResults *results,
                                         Game *scenario, int mover_idx,
                                         int opp_idx, int plies,
                                         ThreadControl *tc,
                                         TranspositionTable *shared_tt,
                                         dual_lexicon_mode_t dual_lexicon_mode,
                                         int thread_index) {
  int32_t mover_lead =
      equity_to_int(player_get_score(game_get_player(scenario, mover_idx))) -
      equity_to_int(player_get_score(game_get_player(scenario, opp_idx)));

  EndgameArgs ea = {
      .thread_control = tc,
      .game = scenario,
      .plies = plies,
      .shared_tt = shared_tt,
      .initial_small_move_arena_size = DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
      .num_threads = 1,
      .use_heuristics = true,
      .num_top_moves = 1,
      .dual_lexicon_mode = dual_lexicon_mode,
      .skip_word_pruning = true,
      .thread_index_offset = thread_index,
  };

  ErrorStack *local_es = error_stack_create();
  endgame_solve(endgame_solver, &ea, results, local_es);
  error_stack_destroy(local_es);

  int endgame_val = endgame_results_get_value(results, ENDGAME_RESULT_BEST);
  return mover_lead - endgame_val;
}

// Set up a bag-empty endgame scenario for a 2-bag candidate where
// mover draws both bag tiles (tiles_played >= 2).
static Game *setup_endgame_scenario_pair(const Game *base_game,
                                         const Move *move, int mover_idx,
                                         int opp_idx, MachineLetter bag_t1,
                                         MachineLetter bag_t2,
                                         const uint8_t unseen[MAX_ALPHABET_SIZE],
                                         int ld_size) {
  Game *g = game_duplicate(base_game);
  game_set_endgame_solving_mode(g);
  game_set_backup_mode(g, BACKUP_MODE_OFF);

  Move m;
  move_copy(&m, move);
  play_move(&m, g, NULL);
  if (game_get_game_end_reason(g) == GAME_END_REASON_STANDARD) {
    Equity bonus = calculate_end_rack_points(
        player_get_rack(game_get_player(g, opp_idx)), game_get_ld(g));
    player_add_to_score(game_get_player(g, mover_idx), -bonus);
    game_set_game_end_reason(g, GAME_END_REASON_NONE);
  }

  Rack *opp_rack = player_get_rack(game_get_player(g, opp_idx));
  set_opp_rack_for_scenario_pair(opp_rack, unseen, ld_size, bag_t1, bag_t2);

  Rack *mover_rack = player_get_rack(game_get_player(g, mover_idx));
  rack_add_letter(mover_rack, bag_t1);
  rack_add_letter(mover_rack, bag_t2);

  return g;
}

// ---------------------------------------------------------------------------
// Unordered pair for 2-bag recursive evaluation.
// ---------------------------------------------------------------------------

typedef struct {
  MachineLetter a, b;
  int total_weight;
} RecUpair;

typedef struct {
  double total, wins;
  int weight;
} RecUpairResult;

typedef struct {
  const RecUpair *upairs;
  int num_upairs;
  atomic_int *next_upair;
  const uint8_t *unseen;
  int ld_size;
  int opp_multi_limit;
  int opp_one_limit;
  int mover_idx;
  int opp_idx;
  bool verbose;
  const LetterDistribution *base_ld;
  const char *move_str;
  EndgameSolverWorker *worker;
  MoveList *opp_ml;
  int thread_index;
  uint8_t mover_leave[MAX_ALPHABET_SIZE];
  Rack saved_mover_leave;
  RecUpairResult *results; // shared array indexed by upair index
} RecUpairThreadArgs;

enum { REC_MAX_OPP_SEL = 128 };

static void *rec_upair_thread(void *arg) {
  RecUpairThreadArgs *a = (RecUpairThreadArgs *)arg;

  Game *game_copy = endgame_solver_worker_get_game(a->worker);
  Board *board = game_get_board(game_copy);
  Rack *mover_rack =
      player_get_rack(game_get_player(game_copy, a->mover_idx));
  Rack *opp_rack =
      player_get_rack(game_get_player(game_copy, a->opp_idx));

  endgame_solver_worker_set_requested_plies(a->worker, 2);

  PVLine pv;

  while (true) {
    int pi = atomic_fetch_add(a->next_upair, 1);
    if (pi >= a->num_upairs)
      break;

    MachineLetter ua = a->upairs[pi].a;
    MachineLetter ub = a->upairs[pi].b;

    double pair_total = 0.0, pair_wins = 0.0;
    int pair_weight = 0;

    // Set opp rack = unseen - ua - ub.
    set_opp_rack_for_scenario_pair(opp_rack, a->unseen, a->ld_size, ua, ub);

    // Compute opp's unseen = mover_leave + ua + ub.
    uint8_t opp_unseen[MAX_ALPHABET_SIZE];
    int opp_unseen_total = 0;
    for (int ml = 0; ml < a->ld_size; ml++) {
      opp_unseen[ml] =
          a->mover_leave[ml] + (ml == ua ? 1 : 0) + (ml == ub ? 1 : 0);
      opp_unseen_total += opp_unseen[ml];
    }

    // Generate opp's moves.
    move_list_reset(a->opp_ml);
    {
      const MoveGenArgs gen_args = {
          .game = game_copy,
          .move_list = a->opp_ml,
          .move_record_type = MOVE_RECORD_ALL,
          .move_sort_type = MOVE_SORT_EQUITY,
          .thread_index = a->thread_index,
          .eq_margin_movegen = 0,
          .target_equity = EQUITY_MAX_VALUE,
          .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
      };
      generate_moves(&gen_args);
    }

    // Select top opp responses by equity (insertion sort into top-N).
    Move sel_multi[REC_MAX_OPP_SEL], sel_one[REC_MAX_OPP_SEL];
    Equity eq_multi[REC_MAX_OPP_SEL], eq_one[REC_MAX_OPP_SEL];
    int n_multi = 0, n_one = 0;
    int eff_multi =
        a->opp_multi_limit < REC_MAX_OPP_SEL ? a->opp_multi_limit
                                              : REC_MAX_OPP_SEL;
    int eff_one =
        a->opp_one_limit < REC_MAX_OPP_SEL ? a->opp_one_limit
                                            : REC_MAX_OPP_SEL;

    for (int j = 0; j < a->opp_ml->count; j++) {
      const Move *sm = move_list_get_move(a->opp_ml, j);
      if (move_get_type(sm) == GAME_EVENT_PASS)
        continue;
      int tp = sm->tiles_played;
      Equity eq = move_get_equity(sm);

      Move *arr;
      Equity *eq_arr;
      int *n;
      int lim;
      if (tp >= 2) {
        arr = sel_multi;
        eq_arr = eq_multi;
        n = &n_multi;
        lim = eff_multi;
      } else if (tp == 1) {
        arr = sel_one;
        eq_arr = eq_one;
        n = &n_one;
        lim = eff_one;
      } else {
        continue;
      }

      if (*n < lim) {
        arr[*n] = *sm;
        eq_arr[*n] = eq;
        (*n)++;
        for (int k = *n - 1; k > 0 && eq_arr[k] > eq_arr[k - 1]; k--) {
          Move ts = arr[k];
          arr[k] = arr[k - 1];
          arr[k - 1] = ts;
          Equity te = eq_arr[k];
          eq_arr[k] = eq_arr[k - 1];
          eq_arr[k - 1] = te;
        }
      } else if (eq > eq_arr[lim - 1]) {
        arr[lim - 1] = *sm;
        eq_arr[lim - 1] = eq;
        for (int k = lim - 1; k > 0 && eq_arr[k] > eq_arr[k - 1]; k--) {
          Move ts = arr[k];
          arr[k] = arr[k - 1];
          arr[k - 1] = ts;
          Equity te = eq_arr[k];
          eq_arr[k] = eq_arr[k - 1];
          eq_arr[k - 1] = te;
        }
      }
    }

    // --- OPP SELECTION under imperfect info ---
    double best_opp_winpct = -1.0;
    double best_opp_avg = -1e18;
    Move best_opp_move;
    bool have_best_opp = false;

    for (int pass_type = 0; pass_type < 2; pass_type++) {
      Move *arr = (pass_type == 0) ? sel_multi : sel_one;
      int n = (pass_type == 0) ? n_multi : n_one;
      for (int j = 0; j < n; j++) {
        Move opp_m;
        move_copy(&opp_m, &arr[j]);
        MoveUndo *opp_undo =
            endgame_solver_worker_get_move_undo(a->worker, 1);
        move_undo_reset(opp_undo);
        play_move_incremental(&opp_m, game_copy, opp_undo);

        if (game_get_game_end_reason(game_copy) ==
            GAME_END_REASON_STANDARD) {
          Equity end_pts = calculate_end_rack_points(
              mover_rack, game_get_ld(game_copy));
          player_add_to_score(game_get_player(game_copy, a->opp_idx),
                              -end_pts);
          game_set_game_end_reason(game_copy, GAME_END_REASON_NONE);
        }
        if (!board_get_cross_sets_valid(board)) {
          if (opp_undo->move_tiles_length > 0)
            update_cross_set_for_move_from_undo(opp_undo, game_copy);
          board_set_cross_sets_valid(board, true);
        }

        Rack saved_m_inner, saved_o_inner;
        rack_copy(&saved_m_inner, mover_rack);
        rack_copy(&saved_o_inner, opp_rack);

        double opp_sum = 0.0;
        double opp_wins_v = 0.0;
        int opp_w = 0;
        bool opp_pruned = false;
        for (int u = 0; u < a->ld_size; u++) {
          if (opp_unseen[u] == 0)
            continue;
          int wu = opp_unseen[u];

          rack_reset(mover_rack);
          for (int ml = 0; ml < a->ld_size; ml++) {
            int cnt = (int)opp_unseen[ml] - (ml == u ? 1 : 0);
            for (int k = 0; k < cnt; k++)
              rack_add_letter(mover_rack, (MachineLetter)ml);
          }
          rack_add_letter(opp_rack, (MachineLetter)u);

          int32_t mover_abs;
          game_set_player_on_turn_index(game_copy, a->opp_idx);
          bool bingo_threat =
              rack_get_total_letters(opp_rack) == RACK_SIZE &&
              has_playable_or_possible_bingo(game_copy, a->thread_index);
          game_set_player_on_turn_index(game_copy, a->mover_idx);
          if (bingo_threat) {
            mover_abs = oneply_endgame_search(
                a->worker, 2, a->mover_idx, a->thread_index, &pv);
          } else {
            mover_abs = -negamax_greedy_leaf_playout(
                a->worker, 2, 1 - a->mover_idx, 0, &pv, 0.0f);
          }
          double opp_spread = (double)(-mover_abs);
          opp_sum += opp_spread * wu;
          opp_wins_v += ((opp_spread > 0)    ? 1.0
                         : (opp_spread == 0) ? 0.5
                                             : 0.0) *
                        wu;
          opp_w += wu;

          rack_take_letter(opp_rack, (MachineLetter)u);

          if (have_best_opp) {
            int remaining = opp_unseen_total - opp_w;
            double best_possible_wpct =
                (opp_wins_v + remaining) / (double)opp_unseen_total;
            if (best_possible_wpct < best_opp_winpct - 1e-9) {
              opp_pruned = true;
              break;
            }
          }
        }

        rack_copy(mover_rack, &saved_m_inner);
        rack_copy(opp_rack, &saved_o_inner);

        double opp_wpct = (opp_w > 0) ? opp_wins_v / opp_w : -1.0;
        double opp_avg = (opp_w > 0) ? opp_sum / opp_w : -1e18;

        if (!opp_pruned &&
            (opp_wpct > best_opp_winpct + 1e-9 ||
             (opp_wpct > best_opp_winpct - 1e-9 &&
              opp_avg > best_opp_avg))) {
          best_opp_winpct = opp_wpct;
          best_opp_avg = opp_avg;
          best_opp_move = arr[j];
          have_best_opp = true;
        }

        unplay_move_incremental(game_copy, opp_undo);
        if (opp_undo->move_tiles_length > 0) {
          update_cross_sets_after_unplay_from_undo(opp_undo, game_copy);
          board_set_cross_sets_valid(board, true);
        }
      }
    }

    // --- ACTUAL RESULTS: evaluate with known racks for each ordering ---
    int num_orderings = (ua == ub) ? 1 : 2;

    if (have_best_opp) {
      Move opp_m;
      move_copy(&opp_m, &best_opp_move);
      MoveUndo *opp_undo =
          endgame_solver_worker_get_move_undo(a->worker, 1);
      move_undo_reset(opp_undo);
      play_move_incremental(&opp_m, game_copy, opp_undo);

      if (game_get_game_end_reason(game_copy) ==
          GAME_END_REASON_STANDARD) {
        Equity end_pts = calculate_end_rack_points(
            mover_rack, game_get_ld(game_copy));
        player_add_to_score(game_get_player(game_copy, a->opp_idx),
                            -end_pts);
        game_set_game_end_reason(game_copy, GAME_END_REASON_NONE);
      }
      if (!board_get_cross_sets_valid(board)) {
        if (opp_undo->move_tiles_length > 0)
          update_cross_set_for_move_from_undo(opp_undo, game_copy);
        board_set_cross_sets_valid(board, true);
      }

      for (int ord = 0; ord < num_orderings; ord++) {
        MachineLetter t1 = (ord == 0) ? ua : ub;
        MachineLetter t2 = (ord == 0) ? ub : ua;
        int cnt = (int)a->unseen[t1] *
                  (t1 == t2 ? (int)a->unseen[t1] - 1
                            : (int)a->unseen[t2]);

        rack_add_letter(mover_rack, t1);
        rack_add_letter(opp_rack, t2);

        int32_t mover_abs;
        game_set_player_on_turn_index(game_copy, a->opp_idx);
        int opp_tiles = rack_get_total_letters(opp_rack);
        bool opp_full = (opp_tiles == RACK_SIZE);
        bool opp_bingo = opp_full && has_playable_or_possible_bingo(game_copy, a->thread_index);
        game_set_player_on_turn_index(game_copy, a->mover_idx);
        if (a->verbose) {
          StringBuilder *dbg = string_builder_create();
          string_builder_add_rack(dbg, opp_rack, a->base_ld, false);
          printf("      bingo_check: opp_tiles=%d opp_full=%d opp_bingo=%d opp_rack=%s\n",
                 opp_tiles, opp_full, opp_bingo, string_builder_peek(dbg));
          string_builder_destroy(dbg);
        }
        if (opp_bingo) {
          mover_abs = oneply_endgame_search(
              a->worker, 2, a->mover_idx, a->thread_index, &pv);
        } else {
          mover_abs = -negamax_greedy_leaf_playout(
              a->worker, 2, 1 - a->mover_idx, 0, &pv, 0.0f);
        }
        double mover_spread = (double)mover_abs;

        if (a->verbose) {
          StringBuilder *sb_v = string_builder_create();
          string_builder_add_move(sb_v, board, &opp_m, a->base_ld, false);
          StringBuilder *line = string_builder_create();
          string_builder_add_formatted_string(
              line, "    [recursive %s] pair (%s,%s x%d)  opp=%s  "
                    "mover_rack=",
              a->move_str, a->base_ld->ld_ml_to_hl[t1],
              a->base_ld->ld_ml_to_hl[t2], cnt,
              string_builder_peek(sb_v));
          string_builder_clear(sb_v);
          string_builder_add_rack(sb_v, mover_rack, a->base_ld, false);
          string_builder_add_formatted_string(line, "%s  PV:",
                                              string_builder_peek(sb_v));
          Rack pv_mover_rack, pv_opp_rack;
          rack_copy(&pv_mover_rack, mover_rack);
          rack_copy(&pv_opp_rack, opp_rack);
          for (int pvi = 0; pvi < pv.num_moves; pvi++) {
            Move pv_m;
            small_move_to_move(&pv_m, &pv.moves[pvi], board);
            int pv_score = (int)small_move_get_score(&pv.moves[pvi]);
            string_builder_clear(sb_v);
            string_builder_add_move(sb_v, board, &pv_m, a->base_ld, false);
            string_builder_add_formatted_string(
                line, " %s(%d)", string_builder_peek(sb_v), pv_score);
            Rack *pr =
                (pvi % 2 == 0) ? &pv_mover_rack : &pv_opp_rack;
            for (int ti = 0; ti < pv_m.tiles_length; ti++) {
              MachineLetter ml = pv_m.tiles[ti];
              if (ml != PLAYED_THROUGH_MARKER)
                rack_take_letter(
                    pr, get_is_blanked(ml) ? BLANK_MACHINE_LETTER : ml);
            }
          }
          string_builder_clear(sb_v);
          if (pv_mover_rack.number_of_letters > 0) {
            string_builder_add_rack(sb_v, &pv_mover_rack, a->base_ld,
                                    false);
            int rack_val =
                equity_to_int(rack_get_score(a->base_ld, &pv_mover_rack));
            string_builder_add_formatted_string(
                line, "  [-%s %d]", string_builder_peek(sb_v), rack_val);
          } else if (pv_opp_rack.number_of_letters > 0) {
            string_builder_add_rack(sb_v, &pv_opp_rack, a->base_ld, false);
            int rack_val =
                equity_to_int(rack_get_score(a->base_ld, &pv_opp_rack));
            string_builder_add_formatted_string(
                line, "  [+%s %d]", string_builder_peek(sb_v), rack_val);
          }
          const char *outcome = (mover_spread > 0)    ? "mover wins"
                                : (mover_spread < 0) ? "mover loses"
                                                     : "tie";
          string_builder_add_formatted_string(
              line, "  [%s by %.0f]",
              outcome, mover_spread > 0 ? mover_spread : -mover_spread);
          printf("%s\n", string_builder_peek(line));
          string_builder_destroy(sb_v);
          string_builder_destroy(line);
        }

        rack_take_letter(opp_rack, t2);
        rack_take_letter(mover_rack, t1);

        pair_total += mover_spread * cnt;
        pair_wins += ((mover_spread > 0)    ? 1.0
                      : (mover_spread == 0) ? 0.5
                                            : 0.0) *
                     cnt;
        pair_weight += cnt;
      }

      unplay_move_incremental(game_copy, opp_undo);
      if (opp_undo->move_tiles_length > 0) {
        update_cross_sets_after_unplay_from_undo(opp_undo, game_copy);
        board_set_cross_sets_valid(board, true);
      }
    } else {
      // No opp tile response: both-pass outcome for each ordering.
      const LetterDistribution *ld = game_get_ld(game_copy);
      int ms = equity_to_int(
          player_get_score(game_get_player(game_copy, a->mover_idx)));
      int os = equity_to_int(
          player_get_score(game_get_player(game_copy, a->opp_idx)));
      for (int ord = 0; ord < num_orderings; ord++) {
        MachineLetter t1 = (ord == 0) ? ua : ub;
        MachineLetter t2 = (ord == 0) ? ub : ua;
        int cnt = (int)a->unseen[t1] *
                  (t1 == t2 ? (int)a->unseen[t1] - 1
                            : (int)a->unseen[t2]);
        rack_add_letter(mover_rack, t1);
        int mp = equity_to_int(rack_get_score(ld, mover_rack));
        int op = equity_to_int(rack_get_score(ld, opp_rack));
        rack_take_letter(mover_rack, t1);
        double mover_spread = (double)((ms - mp) - (os - op));
        pair_total += mover_spread * cnt;
        pair_wins += ((mover_spread > 0)    ? 1.0
                      : (mover_spread == 0) ? 0.5
                                            : 0.0) *
                     cnt;
        pair_weight += cnt;
      }
    }

    // Restore mover rack to leave for next unordered pair.
    rack_copy(mover_rack, &a->saved_mover_leave);

    a->results[pi] =
        (RecUpairResult){pair_total, pair_wins, pair_weight};
  }

  return NULL;
}

// Evaluate a non-bag-emptying candidate in 2-bag using imperfect information
// for the opponent. For each unordered pair {a,b} of bag tiles, opp has the
// same rack and faces the same 1-PEG decision (doesn't know which tile mover
// drew vs which is in the bag). Opp selects their best response by evaluating
// each candidate move across all possible bag tiles from opp's perspective.
// Then the actual W/L/spread is recorded using known racks per ordered pair.
// num_rc_threads workers process upairs in parallel via work-stealing.
static double peg_endgame_eval_recursive_candidate(
    const Game *base_game, const Move *move, int mover_idx, int opp_idx,
    int plies, const uint8_t unseen[MAX_ALPHABET_SIZE], int ld_size,
    int tiles_in_bag, int tiles_drawn, const PegArgs *outer_args,
    TranspositionTable *shared_tt, int thread_index_base, int num_rc_threads,
    PegCutoff *cutoff, double *win_pct_out, bool *pruned_out) {
  (void)plies;
  (void)shared_tt;
  (void)tiles_in_bag;
  (void)tiles_drawn;

  double total = 0.0;
  double wins = 0.0;
  int weight = 0;
  double ret_val = 0.0;
  *pruned_out = false;

  if (num_rc_threads < 1)
    num_rc_threads = 1;

  bool verbose = outer_args->per_pass_callback != NULL;
  int opp_multi_limit = outer_args->inner_opp_multi_tile_limit > 0
                            ? outer_args->inner_opp_multi_tile_limit
                            : 8;
  int opp_one_limit = outer_args->inner_opp_one_tile_limit > 0
                          ? outer_args->inner_opp_one_tile_limit
                          : 8;

  const LetterDistribution *base_ld = game_get_ld(base_game);

  // Format the candidate move for logging.
  StringBuilder *move_sb = string_builder_create();
  string_builder_add_move(move_sb, game_get_board(base_game), (Move *)move,
                          base_ld, false);
  const char *move_str = string_builder_peek(move_sb);

  // Create one EndgameSolver per thread (not shared) so that each thread
  // has its own requested_plies — oneply_endgame_search temporarily sets
  // requested_plies to undo_base+1 and a shared solver would race.
  EndgameSolverWorker **workers =
      malloc_or_die((size_t)num_rc_threads * sizeof(EndgameSolverWorker *));
  EndgameSolver **eg_solvers =
      malloc_or_die((size_t)num_rc_threads * sizeof(EndgameSolver *));
  MoveList **opp_mls =
      malloc_or_die((size_t)num_rc_threads * sizeof(MoveList *));

  for (int t = 0; t < num_rc_threads; t++) {
    int tidx = thread_index_base + t;
    eg_solvers[t] = endgame_solver_create();
    EndgameArgs eg_ea = {
        .thread_control = outer_args->thread_control,
        .game = base_game,
        .plies = 0,
        .tt_fraction_of_mem = 0,
        .initial_small_move_arena_size = DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
        .num_threads = 1,
        .use_heuristics = true,
        .dual_lexicon_mode = outer_args->dual_lexicon_mode,
        .skip_word_pruning = true,
    };
    endgame_solver_reset(eg_solvers[t], &eg_ea);
    workers[t] = endgame_solver_create_worker(
        eg_solvers[t], tidx, (uint64_t)tidx * 54321 + 1);
    opp_mls[t] = move_list_create(PEG_MOVELIST_CAPACITY);

    // Play candidate on this worker's game copy.
    Game *gc = endgame_solver_worker_get_game(workers[t]);
    Board *bd = game_get_board(gc);
    Rack *mr = player_get_rack(game_get_player(gc, mover_idx));
    Rack *opp_r = player_get_rack(game_get_player(gc, opp_idx));

    Move mc;
    move_copy(&mc, move);
    MoveUndo *undo = endgame_solver_worker_get_move_undo(workers[t], 0);
    move_undo_reset(undo);
    play_move_incremental(&mc, gc, undo);

    if (game_get_game_end_reason(gc) == GAME_END_REASON_STANDARD) {
      Equity end_pts =
          calculate_end_rack_points(opp_r, game_get_ld(gc));
      player_add_to_score(game_get_player(gc, mover_idx), -end_pts);
      game_set_game_end_reason(gc, GAME_END_REASON_NONE);
    }
    if (!board_get_cross_sets_valid(bd)) {
      if (undo->move_tiles_length > 0)
        update_cross_set_for_move_from_undo(undo, gc);
      board_set_cross_sets_valid(bd, true);
    }
    (void)mr;
  }

  // Compute mover's leave from worker[0].
  Game *game0 = endgame_solver_worker_get_game(workers[0]);
  Rack *mover_rack0 =
      player_get_rack(game_get_player(game0, mover_idx));
  uint8_t mover_leave[MAX_ALPHABET_SIZE];
  for (int ml = 0; ml < ld_size; ml++)
    mover_leave[ml] = (uint8_t)rack_get_letter(mover_rack0, ml);

  // Build unordered-pair list sorted by total weight descending.
  RecUpair upairs[MAX_ALPHABET_SIZE * MAX_ALPHABET_SIZE];
  int num_upairs = 0;
  for (int ia = 0; ia < ld_size; ia++) {
    if (unseen[ia] == 0)
      continue;
    for (int ib = ia; ib < ld_size; ib++) {
      if (unseen[ib] == 0)
        continue;
      if (ia == ib && unseen[ia] < 2)
        continue;
      int w_ab = (int)unseen[ia] *
                 (ia == ib ? (int)unseen[ia] - 1 : (int)unseen[ib]);
      int w_ba = (ia == ib) ? 0 : w_ab;
      int tw = w_ab + w_ba;
      upairs[num_upairs++] =
          (RecUpair){(MachineLetter)ia, (MachineLetter)ib, tw};
    }
  }
  // Sort by weight descending for earlier pruning.
  for (int i = 0; i < num_upairs - 1; i++)
    for (int j = i + 1; j < num_upairs; j++)
      if (upairs[j].total_weight > upairs[i].total_weight) {
        RecUpair tmp = upairs[i];
        upairs[i] = upairs[j];
        upairs[j] = tmp;
      }

  // Allocate per-upair results.
  RecUpairResult *upair_results =
      calloc((size_t)num_upairs, sizeof(RecUpairResult));

  Timer rc_timer;
  ctimer_start(&rc_timer);

  // Launch threads.
  atomic_int next_upair;
  atomic_init(&next_upair, 0);

  RecUpairThreadArgs *targs =
      malloc_or_die((size_t)num_rc_threads * sizeof(RecUpairThreadArgs));
  for (int t = 0; t < num_rc_threads; t++) {
    int tidx = thread_index_base + t;
    Game *gc = endgame_solver_worker_get_game(workers[t]);
    Rack *mr = player_get_rack(game_get_player(gc, mover_idx));
    targs[t] = (RecUpairThreadArgs){
        .upairs = upairs,
        .num_upairs = num_upairs,
        .next_upair = &next_upair,
        .unseen = unseen,
        .ld_size = ld_size,
        .opp_multi_limit = opp_multi_limit,
        .opp_one_limit = opp_one_limit,
        .mover_idx = mover_idx,
        .opp_idx = opp_idx,
        .verbose = verbose,
        .base_ld = base_ld,
        .move_str = move_str,
        .worker = workers[t],
        .opp_ml = opp_mls[t],
        .thread_index = tidx,
        .results = upair_results,
    };
    memcpy(targs[t].mover_leave, mover_leave, sizeof(mover_leave));
    rack_copy(&targs[t].saved_mover_leave, mr);
  }

  if (num_rc_threads == 1) {
    rec_upair_thread(&targs[0]);
  } else {
    pthread_t *threads =
        malloc_or_die((size_t)num_rc_threads * sizeof(pthread_t));
    for (int t = 0; t < num_rc_threads; t++)
      cpthread_create(&threads[t], rec_upair_thread, &targs[t]);
    for (int t = 0; t < num_rc_threads; t++)
      cpthread_join(threads[t]);
    free(threads);
  }

  // Merge per-upair results.
  for (int i = 0; i < num_upairs; i++) {
    total += upair_results[i].total;
    wins += upair_results[i].wins;
    weight += upair_results[i].weight;
  }

  // Check inter-candidate cutoff after full evaluation.
  if (cutoff) {
    double threshold = peg_cutoff_get(cutoff);
    if (threshold >= 0 && weight > 0) {
      double wp = wins / weight;
      if (wp < threshold - 1e-9) {
        *win_pct_out = wp;
        *pruned_out = true;
        ret_val = total / weight;
        goto cleanup;
      }
    }
  }

  if (verbose) {
    double elapsed = ctimer_elapsed_seconds(&rc_timer);
    printf("    [recursive %s] done %d upairs  win%%=%.1f%%  spread=%+.2f  "
           "total=%.3fs\n",
           move_str, num_upairs,
           weight > 0 ? (wins / weight) * 100.0 : 0.0,
           weight > 0 ? total / weight : 0.0, elapsed);
  }

  *win_pct_out = (weight > 0) ? wins / weight : 0.0;
  ret_val = (weight > 0) ? total / weight : 0.0;

cleanup:
  // Cleanup: unplay candidate on each worker, destroy resources.
  for (int t = 0; t < num_rc_threads; t++) {
    Game *gc = endgame_solver_worker_get_game(workers[t]);
    Board *bd = game_get_board(gc);
    MoveUndo *undo = endgame_solver_worker_get_move_undo(workers[t], 0);
    unplay_move_incremental(gc, undo);
    if (undo->move_tiles_length > 0) {
      update_cross_sets_after_unplay_from_undo(undo, gc);
      board_set_cross_sets_valid(bd, true);
    }
    endgame_solver_worker_destroy(workers[t]);
    move_list_destroy(opp_mls[t]);
    endgame_solver_destroy(eg_solvers[t]);
  }

  free(targs);
  free(upair_results);
  free(workers);
  free(eg_solvers);
  free(opp_mls);
  string_builder_destroy(move_sb);
  return ret_val;
}

// Evaluate one candidate move across all bag-tile scenarios using K-ply
// endgame search. Returns the weighted expected spread from the mover's
// perspective (positive = mover winning).
static double peg_endgame_eval_candidate(EndgameSolver *endgame_solver,
                                         EndgameResults *results,
                                         const Game *base_game,
                                         const Move *move, int mover_idx,
                                         int opp_idx, int plies,
                                         const uint8_t unseen[MAX_ALPHABET_SIZE],
                                         int ld_size,
                                         int tiles_in_bag,
                                         ThreadControl *tc,
                                         TranspositionTable *shared_tt,
                                         dual_lexicon_mode_t dual_lexicon_mode,
                                         int thread_index,
                                         const PegArgs *outer_args,
                                         PegCutoff *cutoff,
                                         double *win_pct_out,
                                         bool *pruned_out) {
  int tiles_played = move->tiles_played;

  // Non-bag-emptying candidate in 2-bag: recursive sub-PEG.
  if (tiles_in_bag == 2 && tiles_played < 2) {
    return peg_endgame_eval_recursive_candidate(
        base_game, move, mover_idx, opp_idx, plies, unseen, ld_size,
        tiles_in_bag, tiles_played, outer_args, shared_tt, thread_index, 1,
        cutoff, win_pct_out, pruned_out);
  }

  // Bag-emptying candidate: enumerate scenarios and solve endgame directly.
  // Sort scenarios by weight descending for earlier pruning.
  double total = 0.0;
  double wins = 0.0;
  int weight = 0;
  *pruned_out = false;

  if (tiles_in_bag == 1) {
    // 1-in-bag: build sorted tile list.
    PegSingleScenario singles[MAX_ALPHABET_SIZE];
    int num_singles = 0;
    for (int t = 0; t < ld_size; t++) {
      if (unseen[t] > 0)
        singles[num_singles++] =
            (PegSingleScenario){(MachineLetter)t, (int)unseen[t]};
    }
    qsort(singles, num_singles, sizeof(PegSingleScenario),
          compare_single_scenarios_desc);

    for (int si = 0; si < num_singles; si++) {
      MachineLetter t = singles[si].ml;
      int cnt = singles[si].count;

      if (cutoff) {
        double threshold = peg_cutoff_get(cutoff);
        if (threshold >= 0) {
          int remaining = cutoff->total_weight - weight;
          double best_possible =
              (wins + remaining) / (double)cutoff->total_weight;
          if (best_possible < threshold - 1e-9) {
            *win_pct_out = best_possible;
            *pruned_out = true;
            return total / (weight > 0 ? weight : 1);
          }
        }
      }

      Game *scenario = setup_endgame_scenario(base_game, move, mover_idx,
                                              opp_idx, t, unseen, ld_size);
      int32_t mover_total = peg_eval_endgame_scenario(
          endgame_solver, results, scenario, mover_idx, opp_idx, plies, tc,
          shared_tt, dual_lexicon_mode, thread_index);
      total += (double)mover_total * cnt;
      wins +=
          ((mover_total > 0) ? 1.0 : (mover_total == 0 ? 0.5 : 0.0)) * cnt;
      weight += cnt;
      game_destroy(scenario);
    }
  } else {
    // 2-in-bag bag-emptying: build sorted pair list.
    PegPairScenario ep[MAX_ALPHABET_SIZE * MAX_ALPHABET_SIZE];
    int num_ep = 0;
    for (int t1 = 0; t1 < ld_size; t1++) {
      if (unseen[t1] == 0)
        continue;
      for (int t2 = 0; t2 < ld_size; t2++) {
        if (unseen[t2] == 0)
          continue;
        if (t1 == t2 && unseen[t1] < 2)
          continue;
        int w = (int)unseen[t1] *
                (t1 == t2 ? (int)unseen[t1] - 1 : (int)unseen[t2]);
        ep[num_ep++] =
            (PegPairScenario){(MachineLetter)t1, (MachineLetter)t2, w};
      }
    }
    qsort(ep, num_ep, sizeof(PegPairScenario), compare_pair_scenarios_desc);

    for (int pi = 0; pi < num_ep; pi++) {
      int cnt = ep[pi].weight;

      if (cutoff) {
        double threshold = peg_cutoff_get(cutoff);
        if (threshold >= 0) {
          int remaining = cutoff->total_weight - weight;
          double best_possible =
              (wins + remaining) / (double)cutoff->total_weight;
          if (best_possible < threshold - 1e-9) {
            *win_pct_out = best_possible;
            *pruned_out = true;
            return total / (weight > 0 ? weight : 1);
          }
        }
      }

      Game *scenario = setup_endgame_scenario_pair(
          base_game, move, mover_idx, opp_idx, ep[pi].t1, ep[pi].t2, unseen,
          ld_size);
      int32_t mover_total = peg_eval_endgame_scenario(
          endgame_solver, results, scenario, mover_idx, opp_idx, plies, tc,
          shared_tt, dual_lexicon_mode, thread_index);
      total += (double)mover_total * cnt;
      wins +=
          ((mover_total > 0) ? 1.0 : (mover_total == 0 ? 0.5 : 0.0)) * cnt;
      weight += cnt;
      game_destroy(scenario);
    }
  }

  if (weight == 0) {
    *win_pct_out = 0.0;
    return 0.0;
  }
  *win_pct_out = wins / weight;
  return total / weight;
}

// ---------------------------------------------------------------------------
// Recursive pass evaluation
// ---------------------------------------------------------------------------

// Evaluate one bag-tile scenario for the pass candidate.
// Computes the mover's spread and win outcome when bag tile = bag_tile.
static void peg_eval_pass_one_scenario(
    const PegArgs *outer_args, int opp_idx,
    const uint8_t *unseen, int ld_size, int plies,
    TranspositionTable *shared_tt, int thread_index,
    MachineLetter bag_tile, double *spread_out, double *win_out) {
  int mover_idx = 1 - opp_idx;
  const LetterDistribution *ld = game_get_ld(outer_args->game);

  Game *inner_game = game_duplicate(outer_args->game);
  Rack *opp_rack = player_get_rack(game_get_player(inner_game, opp_idx));
  set_opp_rack_for_scenario(opp_rack, unseen, ld_size, bag_tile);
  game_set_player_on_turn_index(inner_game, opp_idx);

  int ms = equity_to_int(
      player_get_score(game_get_player(inner_game, mover_idx)));
  int os = equity_to_int(
      player_get_score(game_get_player(inner_game, opp_idx)));
  int mp = equity_to_int(rack_get_score(
      ld, player_get_rack(game_get_player(inner_game, mover_idx))));
  int op = equity_to_int(rack_get_score(
      ld, player_get_rack(game_get_player(inner_game, opp_idx))));

  // Option A: opp passes too → game ends, both lose rack tile values.
  int both_pass_opp_spread = (os - op) - (ms - mp);
  double opp_pass_expected_win =
      (both_pass_opp_spread > 0)
          ? 1.0
          : (both_pass_opp_spread == 0 ? 0.5 : 0.0);
  double opp_pass_expected_spread = (double)both_pass_opp_spread;

  // Option B: opp plays a tile move, chosen via inner peg_solve.
  // The inner solve lags the outer pipeline by one stage, and uses
  // skip_pass=1 to prevent infinite recursion.
  int inner_stages = plies > 0 ? plies : 1;
  PegSolver *inner_solver = peg_solver_create();
  PegArgs inner_args = {
      .game = inner_game,
      .thread_control = outer_args->thread_control,
      .time_budget_seconds = 0.0,
      .num_threads = 1,
      .tt_fraction_of_mem = outer_args->tt_fraction_of_mem,
      .dual_lexicon_mode = outer_args->dual_lexicon_mode,
      .num_stages = inner_stages,
      .skip_pass = 1,
      .shared_tt = shared_tt,
      .thread_index_base = thread_index,
  };
  for (int i = 0; i < PEG_MAX_STAGES; i++)
    inner_args.stage_candidate_limits[i] =
        outer_args->stage_candidate_limits[i];

  PegResult inner_result;
  ErrorStack *inner_es = error_stack_create();
  peg_solve(inner_solver, &inner_args, &inner_result, inner_es);

  bool found_tile_play =
      error_stack_is_empty(inner_es) &&
      move_get_type(&inner_result.best_move) != GAME_EVENT_PASS;
  double opp_play_expected_win = inner_result.best_win_pct;
  double opp_play_expected_spread = inner_result.best_expected_spread;

  error_stack_destroy(inner_es);
  peg_solver_destroy(inner_solver);

  // Drain the bag from inner_game so setup_endgame_scenario works correctly.
  {
    Bag *ibag = game_get_bag(inner_game);
    for (int ml = 0; ml < ld_size; ml++) {
      while (bag_get_letter(ibag, ml) > 0)
        bag_draw_letter(ibag, (MachineLetter)ml, opp_idx);
    }
  }

  // Opp picks whichever option gives better expected outcome.
  bool opp_chose_pass;
  if (!found_tile_play ||
      opp_pass_expected_win > opp_play_expected_win + 1e-9 ||
      (opp_pass_expected_win >= opp_play_expected_win - 1e-9 &&
       opp_pass_expected_spread > opp_play_expected_spread)) {
    opp_chose_pass = true;
  } else {
    opp_chose_pass = false;
  }

  double opp_spread;
  if (opp_chose_pass) {
    opp_spread = (double)both_pass_opp_spread;
  } else {
    uint8_t inner_unseen[MAX_ALPHABET_SIZE];
    {
      Game *tmp = game_duplicate(inner_game);
      Bag *tbag = game_get_bag(tmp);
      for (int ml = 0; ml < ld_size; ml++) {
        while (bag_get_letter(tbag, ml) > 0)
          bag_draw_letter(tbag, (MachineLetter)ml, opp_idx);
      }
      compute_unseen(tmp, opp_idx, inner_unseen);
      game_destroy(tmp);
    }

    Game *scenario = setup_endgame_scenario(
        inner_game, &inner_result.best_move, opp_idx, mover_idx,
        bag_tile, inner_unseen, ld_size);

    int32_t opp_lead =
        equity_to_int(
            player_get_score(game_get_player(scenario, opp_idx))) -
        equity_to_int(
            player_get_score(game_get_player(scenario, mover_idx)));

    int solve_plies = plies > 0 ? plies : 1;
    EndgameSolver *eg_solver = endgame_solver_create();
    EndgameResults *eg_results = endgame_results_create();
    EndgameArgs ea = {
        .thread_control = outer_args->thread_control,
        .game = scenario,
        .plies = solve_plies,
        .shared_tt = shared_tt,
        .initial_small_move_arena_size =
            DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
        .num_threads = 1,
        .use_heuristics = true,
        .num_top_moves = 1,
        .dual_lexicon_mode = outer_args->dual_lexicon_mode,
        .skip_word_pruning = true,
        .thread_index_offset = thread_index,
    };
    ErrorStack *local_es = error_stack_create();
    endgame_solve(eg_solver, &ea, eg_results, local_es);
    error_stack_destroy(local_es);

    int endgame_val =
        endgame_results_get_value(eg_results, ENDGAME_RESULT_BEST);
    int32_t opp_total = opp_lead - endgame_val;
    opp_spread = (double)opp_total;

    endgame_results_destroy(eg_results);
    endgame_solver_destroy(eg_solver);
    game_destroy(scenario);
  }

  // Convert to mover's perspective.
  *spread_out = -opp_spread;
  *win_out = (opp_spread < 0) ? 1.0 : (opp_spread == 0 ? 0.5 : 0.0);

  game_destroy(inner_game);
}

// Evaluate one ordered-pair bag scenario for the pass candidate in 2-bag PEG.
// Bag contains {bag_t1, bag_t2}. Opp has unseen - bag_t1 - bag_t2.
// The opp can pass too (game over) or play via inner 2-PEG (skip_pass=1).
static void peg_eval_pass_one_scenario_pair(
    const PegArgs *outer_args, int opp_idx,
    const uint8_t *unseen, int ld_size, int plies,
    TranspositionTable *shared_tt, int thread_index,
    MachineLetter bag_t1, MachineLetter bag_t2,
    double *spread_out, double *win_out) {
  int mover_idx = 1 - opp_idx;
  const LetterDistribution *ld = game_get_ld(outer_args->game);

  Game *inner_game = game_duplicate(outer_args->game);
  Rack *opp_rack = player_get_rack(game_get_player(inner_game, opp_idx));
  set_opp_rack_for_scenario_pair(opp_rack, unseen, ld_size, bag_t1, bag_t2);
  game_set_player_on_turn_index(inner_game, opp_idx);

  // The original game already has 2 tiles in bag. The inner peg_solve will
  // drain it and compute unseen correctly (inner unseen = mover_rack + bag).

  int ms = equity_to_int(
      player_get_score(game_get_player(inner_game, mover_idx)));
  int os = equity_to_int(
      player_get_score(game_get_player(inner_game, opp_idx)));
  int mp = equity_to_int(rack_get_score(
      ld, player_get_rack(game_get_player(inner_game, mover_idx))));
  int op = equity_to_int(rack_get_score(
      ld, player_get_rack(game_get_player(inner_game, opp_idx))));

  // Option A: opp passes too → game ends.
  int both_pass_opp_spread = (os - op) - (ms - mp);

  // Option B: opp plays via inner peg_solve (2-PEG, skip_pass=1).
  int inner_stages = plies > 0 ? plies : 1;
  PegSolver *inner_solver = peg_solver_create();
  PegArgs inner_args = {
      .game = inner_game,
      .thread_control = outer_args->thread_control,
      .time_budget_seconds = 0.0,
      .num_threads = 1,
      .tt_fraction_of_mem = outer_args->tt_fraction_of_mem,
      .dual_lexicon_mode = outer_args->dual_lexicon_mode,
      .num_stages = inner_stages,
      .skip_pass = 1,
      .shared_tt = shared_tt,
      .thread_index_base = thread_index,
      .early_cutoff = outer_args->early_cutoff,
  };
  for (int i = 0; i < PEG_MAX_STAGES; i++)
    inner_args.stage_candidate_limits[i] =
        outer_args->stage_candidate_limits[i];

  PegResult inner_result;
  ErrorStack *inner_es = error_stack_create();
  peg_solve(inner_solver, &inner_args, &inner_result, inner_es);

  bool found_tile_play =
      error_stack_is_empty(inner_es) &&
      move_get_type(&inner_result.best_move) != GAME_EVENT_PASS;
  double opp_play_expected_win = inner_result.best_win_pct;
  double opp_play_expected_spread = inner_result.best_expected_spread;

  error_stack_destroy(inner_es);
  peg_solver_destroy(inner_solver);

  // Opp picks whichever option gives better expected outcome.
  double opp_spread;
  if (!found_tile_play ||
      (both_pass_opp_spread > 0 ? 1.0 : (both_pass_opp_spread == 0 ? 0.5 : 0.0))
          > opp_play_expected_win + 1e-9 ||
      ((both_pass_opp_spread > 0 ? 1.0 : (both_pass_opp_spread == 0 ? 0.5 : 0.0))
          >= opp_play_expected_win - 1e-9 &&
       (double)both_pass_opp_spread > opp_play_expected_spread)) {
    opp_spread = (double)both_pass_opp_spread;
  } else {
    // Opp chose to play. Use inner result's expected spread directly.
    opp_spread = opp_play_expected_spread;
  }

  *spread_out = -opp_spread;
  *win_out = (opp_spread < 0) ? 1.0 : (opp_spread == 0 ? 0.5 : 0.0);

  game_destroy(inner_game);
}

// ---------------------------------------------------------------------------
// Thread arguments and worker functions — greedy pass
// ---------------------------------------------------------------------------

typedef struct PegGreedyThreadArgs {
  PegCandidate *candidates;
  atomic_int *next_work_item; // work-stealing index
  int num_non_pass;           // non-pass candidates (items 0..num_non_pass-1)
  int num_work_items;         // total: non_pass + pass scenarios
  EndgameSolverWorker *worker;
  const Game *base_game;      // base game with drained bag
  int mover_idx;
  int opp_idx;
  const uint8_t *unseen;
  int ld_size;
  int total_unseen;
  int tiles_in_bag;
  int thread_index; // unique thread index for this worker
  // Pass scenario data (items num_non_pass..num_work_items-1).
  MachineLetter *scenario_t1;
  MachineLetter *scenario_t2; // -1 for 1-bag scenarios
  int *scenario_counts;
  double *pass_spreads; // output per scenario
  double *pass_wins;    // output per scenario
  const PegArgs *outer_args;
  PegCutoff *cutoff; // NULL for Phase 1 (bag-emptying), set for Phase 2
  atomic_int *progress; // shared completed-item counter (NULL to disable logging)
  bool use_oneply; // true for bingo-threatening plays (2-tile, 7 on rack)
} PegGreedyThreadArgs;

static void *peg_greedy_thread(void *arg) {
  PegGreedyThreadArgs *a = (PegGreedyThreadArgs *)arg;
  while (true) {
    int i = atomic_fetch_add(a->next_work_item, 1);
    if (i >= a->num_work_items)
      break;
    if (i < a->num_non_pass) {
      PegCandidate *c = &a->candidates[i];
      c->pruned = false;
      // For 2-bag, check if this candidate is non-bag-emptying.
      // Non-bag-emptying plays (tiles_played < tiles_in_bag) must be
      // evaluated recursively since they leave tiles in the bag.
      if (a->tiles_in_bag >= 2 && c->move.tiles_played < a->tiles_in_bag) {
        c->expected_value = peg_endgame_eval_recursive_candidate(
            a->base_game, &c->move, a->mover_idx, a->opp_idx,
            0 /* plies=0 for greedy */, a->unseen, a->ld_size, a->tiles_in_bag,
            c->move.tiles_played, a->outer_args, NULL /* no shared_tt at greedy */,
            a->thread_index, 1, a->cutoff, &c->win_pct, &c->pruned);
        if (!c->pruned && a->cutoff)
          peg_cutoff_update(a->cutoff, c->win_pct);
      } else {
        c->expected_value =
            peg_greedy_eval_play(a->worker, &c->move, a->mover_idx, a->opp_idx,
                                 a->unseen, a->ld_size, a->total_unseen,
                                 a->tiles_in_bag, a->cutoff, &c->win_pct,
                                 &c->pruned, a->use_oneply, a->thread_index);
        if (!c->pruned && a->cutoff)
          peg_cutoff_update(a->cutoff, c->win_pct);
      }
    } else {
      int si = i - a->num_non_pass;
      if (a->tiles_in_bag == 1) {
        peg_eval_pass_one_scenario(a->outer_args, a->opp_idx, a->unseen,
                                   a->ld_size, 0, NULL, a->thread_index,
                                   a->scenario_t1[si], &a->pass_spreads[si],
                                   &a->pass_wins[si]);
      } else {
        peg_eval_pass_one_scenario_pair(
            a->outer_args, a->opp_idx, a->unseen, a->ld_size, 0, NULL,
            a->thread_index, a->scenario_t1[si], a->scenario_t2[si],
            &a->pass_spreads[si], &a->pass_wins[si]);
      }
    }
    (void)0; // progress display removed
  }
  return NULL;
}

// ---------------------------------------------------------------------------
// Thread arguments and worker function — endgame passes
// ---------------------------------------------------------------------------

typedef struct PegEndgameThreadArgs {
  PegCandidate *candidates;
  atomic_int *next_work_item; // work-stealing index
  int num_non_pass;           // non-pass candidates (items 0..num_non_pass-1)
  int num_work_items;         // total: non_pass + pass scenarios
  EndgameSolver *endgame_solver; // one per thread
  EndgameResults *endgame_results;
  const Game *base_game;
  int mover_idx;
  int opp_idx;
  int plies;
  const uint8_t *unseen;
  int ld_size;
  int tiles_in_bag;
  int thread_index; // unique thread index for this worker
  ThreadControl *thread_control;
  TranspositionTable *shared_tt;
  dual_lexicon_mode_t dual_lexicon_mode;
  // For recursive pass evaluation.
  PegSolver *solver;
  const PegArgs *outer_args;
  // Early cutoff tracker (NULL if disabled).
  PegCutoff *cutoff;
  // Pass scenario data (items num_non_pass..num_work_items-1).
  MachineLetter *scenario_t1;
  MachineLetter *scenario_t2; // -1 for 1-bag scenarios
  int *scenario_counts;
  double *pass_spreads;
  double *pass_wins;
} PegEndgameThreadArgs;

static void *peg_endgame_thread(void *arg) {
  PegEndgameThreadArgs *a = (PegEndgameThreadArgs *)arg;
  while (true) {
    int idx = atomic_fetch_add(a->next_work_item, 1);
    if (idx >= a->num_work_items)
      break;
    if (idx < a->num_non_pass) {
      PegCandidate *c = &a->candidates[idx];
      bool pruned = false;
      c->expected_value = peg_endgame_eval_candidate(
          a->endgame_solver, a->endgame_results, a->base_game, &c->move,
          a->mover_idx, a->opp_idx, a->plies, a->unseen, a->ld_size,
          a->tiles_in_bag, a->thread_control, a->shared_tt,
          a->dual_lexicon_mode, a->thread_index, a->outer_args, a->cutoff,
          &c->win_pct, &pruned);
      c->pruned = pruned;
      if (!pruned && a->cutoff) {
        peg_cutoff_update(a->cutoff, c->win_pct);
      }
    } else {
      int si = idx - a->num_non_pass;
      if (a->tiles_in_bag == 1) {
        peg_eval_pass_one_scenario(a->outer_args, a->opp_idx, a->unseen,
                                   a->ld_size, a->plies, a->shared_tt,
                                   a->thread_index, a->scenario_t1[si],
                                   &a->pass_spreads[si], &a->pass_wins[si]);
      } else {
        peg_eval_pass_one_scenario_pair(
            a->outer_args, a->opp_idx, a->unseen, a->ld_size, a->plies,
            a->shared_tt, a->thread_index, a->scenario_t1[si],
            a->scenario_t2[si], &a->pass_spreads[si], &a->pass_wins[si]);
      }
    }
  }
  return NULL;
}

// ---------------------------------------------------------------------------
// Main solver
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Per-pass callback helper
// ---------------------------------------------------------------------------

enum { PEG_CALLBACK_MAX_TOP = 128 };

static void invoke_per_pass_callback(const PegArgs *args, int pass,
                                     int num_evaluated,
                                     const PegCandidate *sorted,
                                     int num_sorted, int default_top,
                                     double elapsed, double stage_seconds) {
  if (!args->per_pass_callback)
    return;
  int top = args->per_pass_num_top > 0 ? args->per_pass_num_top : default_top;
  if (top > num_sorted)
    top = num_sorted;
  if (top > PEG_CALLBACK_MAX_TOP)
    top = PEG_CALLBACK_MAX_TOP;
  Move moves[PEG_CALLBACK_MAX_TOP];
  double values[PEG_CALLBACK_MAX_TOP];
  double win_pcts[PEG_CALLBACK_MAX_TOP];
  bool pruned[PEG_CALLBACK_MAX_TOP];
  for (int i = 0; i < top; i++) {
    moves[i] = sorted[i].move;
    values[i] = sorted[i].expected_value;
    win_pcts[i] = sorted[i].win_pct;
    pruned[i] = sorted[i].pruned;
  }
  args->per_pass_callback(pass, num_evaluated, moves, values, win_pcts, pruned,
                          top, args->game, elapsed, stage_seconds,
                          args->per_pass_callback_data);
}

PegSolver *peg_solver_create(void) {
  PegSolver *solver = malloc_or_die(sizeof(PegSolver));
  solver->_unused = 0;
  return solver;
}

void peg_solver_destroy(PegSolver *solver) { free(solver); }

void peg_solve(PegSolver *solver, const PegArgs *args, PegResult *result,
               ErrorStack *error_stack) {
  (void)solver;

  const LetterDistribution *ld = game_get_ld(args->game);
  int ld_size = ld_get_size(ld);
  int mover_idx = game_get_player_on_turn_index(args->game);
  int opp_idx = 1 - mover_idx;
  int num_threads = args->num_threads > 0 ? args->num_threads : 1;

  // --- Compute unseen tiles ---
  // Unseen = total distribution - mover's rack - board tiles.
  // The opponent's rack in the game struct is intentionally ignored: the
  // on-turn player cannot know it, so we treat all unseen tiles as the pool
  // from which each bag-tile scenario is drawn.
  uint8_t unseen[MAX_ALPHABET_SIZE];
  int total_unseen = compute_unseen(args->game, mover_idx, unseen);

  // --- Validate input ---
  if (total_unseen < 1) {
    error_stack_push(error_stack, ERROR_STATUS_ENDGAME_BAG_NOT_EMPTY,
                     get_formatted_string(
                         "peg_solve requires at least 1 unseen tile, "
                         "but found unseen=%d",
                         total_unseen));
    return;
  }
  // Derive the effective bag size from total_unseen.  The CGP may have an
  // empty opponent rack (all unseen tiles in the game bag), so we cannot
  // rely on bag_get_letters.  The opponent gets min(total_unseen-1, RACK_SIZE)
  // tiles; the rest are bag tiles.
  int tiles_in_bag = total_unseen > RACK_SIZE ? total_unseen - RACK_SIZE : 1;
  if (tiles_in_bag > 2) {
    error_stack_push(error_stack, ERROR_STATUS_ENDGAME_BAG_NOT_EMPTY,
                     get_formatted_string(
                         "peg_solve: %d unseen tiles implies %d in bag, "
                         "max supported is 2",
                         total_unseen, tiles_in_bag));
    return;
  }
  // The opponent receives (total_unseen - tiles_in_bag) tiles per scenario.
  int opp_tiles = total_unseen - tiles_in_bag;
  if (opp_tiles > RACK_SIZE) {
    error_stack_push(error_stack, ERROR_STATUS_ENDGAME_BAG_NOT_EMPTY,
                     get_formatted_string(
                         "peg_solve: %d unseen with %d in bag gives opponent "
                         "%d tiles, exceeding RACK_SIZE=%d",
                         total_unseen, tiles_in_bag, opp_tiles, RACK_SIZE));
    return;
  }

  // --- Build a base game with an empty bag ---
  // Drain all tiles from the bag so that play_move_incremental does not try
  // to draw from it; draws are simulated explicitly per bag-tile scenario.
  Game *base_game = game_duplicate(args->game);
  {
    Bag *base_bag = game_get_bag(base_game);
    for (int ml = 0; ml < ld_size; ml++) {
      while (bag_get_letter(base_bag, ml) > 0) {
        bag_draw_letter(base_bag, (MachineLetter)ml, mover_idx);
      }
    }
  }

  Timer peg_timer;
  ctimer_start(&peg_timer);

  // --- Generate all candidate moves ---
  // Use the base_game (empty bag) to generate: this produces plays and passes
  // but not exchanges (no bag tiles to draw from). Exchanges with 1 tile in
  // the bag are rare and unlikely to be optimal; they can be added later.
  MoveList *initial_ml = move_list_create(PEG_MOVELIST_CAPACITY);
  {
    const MoveGenArgs gen_args = {
        .game = base_game,
        .move_list = initial_ml,
        .move_record_type = MOVE_RECORD_ALL,
        .move_sort_type = MOVE_SORT_EQUITY,
        .override_kwg = NULL,
        .thread_index = args->thread_index_base,
        .eq_margin_movegen = 0,
        .target_equity = EQUITY_MAX_VALUE,
        .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
    };
    generate_moves(&gen_args);
  }
  int num_candidates = initial_ml->count;
  if (num_candidates == 0) {
    move_list_destroy(initial_ml);
    game_destroy(base_game);
    error_stack_push(error_stack, ERROR_STATUS_ENDGAME_BAG_NOT_EMPTY,
                     get_formatted_string("peg_solve: no legal moves found"));
    return;
  }

  PegCandidate *candidates = malloc_or_die(num_candidates * sizeof(PegCandidate));
  {
    int j = 0;
    for (int i = 0; i < num_candidates; i++) {
      // skip_pass is set by recursive inner calls to prevent infinite mutual
      // recursion (mover passes → opp's peg_solve → opp passes → ...).
      const Move *im = move_list_get_move(initial_ml, i);
      if ((args->skip_pass || args->skip_root_pass) &&
          move_get_type(im) == GAME_EVENT_PASS)
        continue;
      move_copy(&candidates[j].move, im);
      candidates[j].expected_value = 0.0;
      j++;
    }
    num_candidates = j;
  }
  move_list_destroy(initial_ml);

  // Apply candidate allowlist if provided.
  if (args->candidate_allowlist && args->candidate_allowlist_count > 0) {
    const Board *board = game_get_board(base_game);
    const LetterDistribution *fld = game_get_ld(args->game);
    int j = 0;
    for (int i = 0; i < num_candidates; i++) {
      StringBuilder *sb = string_builder_create();
      string_builder_add_move(sb, board, &candidates[i].move, fld, false);
      const char *move_str = string_builder_peek(sb);
      bool keep = false;
      for (int k = 0; k < args->candidate_allowlist_count; k++) {
        if (strcmp(move_str, args->candidate_allowlist[k]) == 0) {
          keep = true;
          break;
        }
      }
      string_builder_destroy(sb);
      if (keep) {
        if (j != i)
          candidates[j] = candidates[i];
        j++;
      }
    }
    num_candidates = j;
  }

  if (num_candidates == 0) {
    free(candidates);
    game_destroy(base_game);
    error_stack_push(error_stack, ERROR_STATUS_ENDGAME_BAG_NOT_EMPTY,
                     get_formatted_string("peg_solve: no legal moves after "
                                         "filtering"));
    return;
  }

  // =========================================================================
  // Pass 0: greedy evaluation for all candidates
  // =========================================================================

  // Set up one EndgameSolver and one EndgameSolverWorker per thread.
  // Each worker's game_copy is a duplicate of base_game (empty bag).
  EndgameSolver **greedy_solvers =
      malloc_or_die(num_threads * sizeof(EndgameSolver *));
  EndgameSolverWorker **greedy_workers =
      malloc_or_die(num_threads * sizeof(EndgameSolverWorker *));
  for (int ti = 0; ti < num_threads; ti++) {
    greedy_solvers[ti] = endgame_solver_create();
    EndgameArgs greedy_ea = {
        .thread_control = args->thread_control,
        .game = base_game,
        .plies = 0,
        .tt_fraction_of_mem = 0, // no TT for greedy
        .initial_small_move_arena_size = DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
        .num_threads = 1,
        .use_heuristics = true,
        .dual_lexicon_mode = args->dual_lexicon_mode,
        .skip_word_pruning = true,
    };
    // endgame_solver_reset configures the solver (solving_player, etc.)
    // and leaves pruned_kwgs NULL (skip_word_pruning=true).
    endgame_solver_reset(greedy_solvers[ti], &greedy_ea);

    int tidx = args->thread_index_base + ti;
    greedy_workers[ti] = endgame_solver_create_worker(
        greedy_solvers[ti], tidx, (uint64_t)tidx * 54321 + 1);
  }

  // Separate pass from non-pass candidates: move pass to end.
  int pass_candidate_idx = -1;
  for (int i = 0; i < num_candidates; i++) {
    if (move_get_type(&candidates[i].move) == GAME_EVENT_PASS) {
      pass_candidate_idx = i;
      break;
    }
  }
  int non_pass_count = num_candidates;
  if (pass_candidate_idx >= 0) {
    PegCandidate tmp = candidates[pass_candidate_idx];
    candidates[pass_candidate_idx] = candidates[num_candidates - 1];
    candidates[num_candidates - 1] = tmp;
    non_pass_count = num_candidates - 1;
  }

  // Partition non-pass candidates into bag-emptying (front) and
  // non-bag-emptying (back) for 2-bag positions.
  // Within bag-emptying, further split into:
  //   Phase 1a: plays with tiles_played > tiles_in_bag (opponent gets nonfull
  //             rack, can't bingo → pure greedy)
  //   Phase 1b: plays with tiles_played == tiles_in_bag (opponent gets full
  //             rack, might bingo → per-scenario bingo check)
  int num_bag_emptying = non_pass_count;
  int num_bag_emptying_safe = 0; // Phase 1a: nonfull rack (> tiles_in_bag)
  int num_bag_emptying_bingo = 0; // Phase 1b: full rack (== tiles_in_bag)
  int num_non_emptying = 0;
  if (tiles_in_bag >= 2) {
    // Three-way partition: [nonfull rack | full rack | non-emptying]
    // First pass: move all bag-emptying to front.
    int write = 0;
    for (int i = 0; i < non_pass_count; i++) {
      if (candidates[i].move.tiles_played >= tiles_in_bag) {
        if (i != write) {
          PegCandidate tmp = candidates[write];
          candidates[write] = candidates[i];
          candidates[i] = tmp;
        }
        write++;
      }
    }
    num_bag_emptying = write;
    num_non_emptying = non_pass_count - write;

    // Second pass within bag-emptying: safe (>tiles_in_bag) to front,
    // bingo (==tiles_in_bag) after.
    int safe_write = 0;
    for (int i = 0; i < num_bag_emptying; i++) {
      if (candidates[i].move.tiles_played > tiles_in_bag) {
        if (i != safe_write) {
          PegCandidate tmp = candidates[safe_write];
          candidates[safe_write] = candidates[i];
          candidates[i] = tmp;
        }
        safe_write++;
      }
    }
    num_bag_emptying_safe = safe_write;
    num_bag_emptying_bingo = num_bag_emptying - safe_write;
  }

  if (tiles_in_bag >= 2 && args->per_pass_callback) {
    printf("[PEG] %d-bag: %d candidates (%d bag-emptying [%d nonfull rack, %d full rack], "
           "%d non-emptying%s)\n",
           tiles_in_bag, num_candidates, num_bag_emptying,
           num_bag_emptying_safe, num_bag_emptying_bingo,
           num_non_emptying, pass_candidate_idx >= 0 ? ", +pass" : "");
  }

  // Build pass scenario arrays for the unified work queue.
  // For 1-bag: scenarios are per unseen tile (up to MAX_ALPHABET_SIZE).
  // For 2-bag: scenarios are ordered pairs (up to MAX_ALPHABET_SIZE^2).
  int max_scenarios = (tiles_in_bag == 1) ? MAX_ALPHABET_SIZE
                                          : MAX_ALPHABET_SIZE * MAX_ALPHABET_SIZE;
  int greedy_num_scenarios = 0;
  MachineLetter *greedy_scenario_t1 =
      malloc_or_die(max_scenarios * sizeof(MachineLetter));
  MachineLetter *greedy_scenario_t2 =
      malloc_or_die(max_scenarios * sizeof(MachineLetter));
  int *greedy_scenario_counts = malloc_or_die(max_scenarios * sizeof(int));
  double *greedy_pass_spreads = malloc_or_die(max_scenarios * sizeof(double));
  double *greedy_pass_wins = malloc_or_die(max_scenarios * sizeof(double));
  if (pass_candidate_idx >= 0) {
    if (tiles_in_bag == 1) {
      for (int t = 0; t < ld_size; t++) {
        if (unseen[t] > 0) {
          greedy_scenario_t1[greedy_num_scenarios] = (MachineLetter)t;
          greedy_scenario_t2[greedy_num_scenarios] = (MachineLetter)-1;
          greedy_scenario_counts[greedy_num_scenarios] = (int)unseen[t];
          greedy_num_scenarios++;
        }
      }
    } else {
      for (int t1 = 0; t1 < ld_size; t1++) {
        if (unseen[t1] == 0)
          continue;
        for (int t2 = 0; t2 < ld_size; t2++) {
          if (unseen[t2] == 0)
            continue;
          if (t1 == t2 && unseen[t1] < 2)
            continue;
          greedy_scenario_t1[greedy_num_scenarios] = (MachineLetter)t1;
          greedy_scenario_t2[greedy_num_scenarios] = (MachineLetter)t2;
          greedy_scenario_counts[greedy_num_scenarios] =
              (int)unseen[t1] *
              (t1 == t2 ? (int)unseen[t1] - 1 : (int)unseen[t2]);
          greedy_num_scenarios++;
        }
      }
    }
  }
  bool top_level = args->per_pass_callback != NULL;
  atomic_int greedy_next;
  atomic_init(&greedy_next, 0);
  atomic_int greedy_progress;
  atomic_init(&greedy_progress, 0);
  PegGreedyThreadArgs *greedy_targs =
      malloc_or_die(num_threads * sizeof(PegGreedyThreadArgs));
  cpthread_t *greedy_threads = malloc_or_die(num_threads * sizeof(cpthread_t));

  // For 2-bag: three phases (bag-emptying, 1-tile plays, pass).
  // For 1-bag: single unified phase (bag-emptying + pass scenarios together).
  bool defer_pass = (tiles_in_bag >= 2 && pass_candidate_idx >= 0);
  int phase1_pass_scenarios = defer_pass ? 0 : greedy_num_scenarios;

  // Unified cutoff for all greedy phases (if early_cutoff is enabled).
  // cutoff_k=1: prune any candidate that can't reach the current best win%.
  PegCutoff greedy_cutoff;
  bool greedy_cutoff_active = false;
  if (args->early_cutoff) {
    int tw = (tiles_in_bag == 1) ? total_unseen
                                 : total_unseen * (total_unseen - 1);
    peg_cutoff_init(&greedy_cutoff, 1, tw, num_candidates);
    greedy_cutoff_active = true;
  }

  // -----------------------------------------------------------------------
  // Phase 1a: safe bag-emptying candidates (>tiles_in_bag tiles played).
  // Opponent gets <RACK_SIZE tiles, so can't bingo → pure greedy.
  // For 1-bag positions, there are no safe candidates (all are bingo-threat),
  // so this phase is skipped and pass scenarios go into Phase 1b.
  // -----------------------------------------------------------------------
  int phase1a_items = (tiles_in_bag >= 2) ? num_bag_emptying_safe : 0;
  if (phase1a_items > 0) {
    atomic_init(&greedy_next, 0);
    for (int ti = 0; ti < num_threads; ti++) {
      greedy_targs[ti] = (PegGreedyThreadArgs){
          .candidates = candidates,
          .next_work_item = &greedy_next,
          .num_non_pass = num_bag_emptying_safe,
          .num_work_items = phase1a_items,
          .worker = greedy_workers[ti],
          .base_game = base_game,
          .mover_idx = mover_idx,
          .opp_idx = opp_idx,
          .unseen = unseen,
          .ld_size = ld_size,
          .total_unseen = total_unseen,
          .tiles_in_bag = tiles_in_bag,
          .thread_index = args->thread_index_base + ti,
          .scenario_t1 = greedy_scenario_t1,
          .scenario_t2 = greedy_scenario_t2,
          .scenario_counts = greedy_scenario_counts,
          .pass_spreads = greedy_pass_spreads,
          .pass_wins = greedy_pass_wins,
          .outer_args = args,
          .cutoff = greedy_cutoff_active ? &greedy_cutoff : NULL,
          .progress = NULL,
          .use_oneply = false,
      };
      cpthread_create(&greedy_threads[ti], peg_greedy_thread, &greedy_targs[ti]);
    }
    for (int ti = 0; ti < num_threads; ti++) {
      cpthread_join(greedy_threads[ti]);
    }
  }
  double phase1a_end = ctimer_elapsed_seconds(&peg_timer);

  // -----------------------------------------------------------------------
  // Phase 1b: bingo-threat bag-emptying candidates (==tiles_in_bag tiles
  // played, so opponent gets RACK_SIZE tiles).
  // Per-scenario bingo check (has_playable_or_possible_bingo) determines whether to
  // use 1-ply endgame (opponent blocks bingo) or greedy playout.
  // For 1-bag: all bag-emptying + pass scenarios are included here.
  // -----------------------------------------------------------------------
  int phase1b_non_pass = (tiles_in_bag >= 2) ? num_bag_emptying_bingo
                                              : num_bag_emptying;
  int phase1b_items = phase1b_non_pass + phase1_pass_scenarios;
  if (args->skip_phase_1b) {
    if (top_level)
      printf("[PEG] Phase 1b: SKIPPED (%d candidates)\n", phase1b_items);
    phase1b_items = 0;
  }
  if (phase1b_items > 0) {
    atomic_init(&greedy_next, 0);
    PegCandidate *phase1b_start =
        (tiles_in_bag >= 2) ? candidates + num_bag_emptying_safe : candidates;
    for (int ti = 0; ti < num_threads; ti++) {
      greedy_targs[ti] = (PegGreedyThreadArgs){
          .candidates = phase1b_start,
          .next_work_item = &greedy_next,
          .num_non_pass = phase1b_non_pass,
          .num_work_items = phase1b_items,
          .worker = greedy_workers[ti],
          .base_game = base_game,
          .mover_idx = mover_idx,
          .opp_idx = opp_idx,
          .unseen = unseen,
          .ld_size = ld_size,
          .total_unseen = total_unseen,
          .tiles_in_bag = tiles_in_bag,
          .thread_index = args->thread_index_base + ti,
          .scenario_t1 = greedy_scenario_t1,
          .scenario_t2 = greedy_scenario_t2,
          .scenario_counts = greedy_scenario_counts,
          .pass_spreads = greedy_pass_spreads,
          .pass_wins = greedy_pass_wins,
          .outer_args = args,
          .cutoff = greedy_cutoff_active ? &greedy_cutoff : NULL,
          .progress = NULL,
          .use_oneply = true,
      };
      cpthread_create(&greedy_threads[ti], peg_greedy_thread, &greedy_targs[ti]);
    }
    for (int ti = 0; ti < num_threads; ti++) {
      cpthread_join(greedy_threads[ti]);
    }
  }
  double phase1b_end = ctimer_elapsed_seconds(&peg_timer);

  // Aggregate pass scenario results (1-bag only; 2-bag defers pass).
  if (pass_candidate_idx >= 0 && !defer_pass) {
    PegCandidate *pass_c = &candidates[num_candidates - 1];
    double total = 0.0, wins = 0.0;
    int weight = 0;
    for (int si = 0; si < greedy_num_scenarios; si++) {
      total += greedy_pass_spreads[si] * greedy_scenario_counts[si];
      wins += greedy_pass_wins[si] * greedy_scenario_counts[si];
      weight += greedy_scenario_counts[si];
    }
    pass_c->win_pct = (weight > 0) ? wins / weight : 0.0;
    pass_c->expected_value = (weight > 0) ? total / weight : 0.0;
    pass_c->pruned = false;
  }

  // -----------------------------------------------------------------------
  // Phases 2 & 3 (2-bag only): 1-tile plays then pass, with cutoff
  // -----------------------------------------------------------------------
  int num_evaluated_be = args->skip_phase_1b ? num_bag_emptying_safe
                                                : num_bag_emptying;
  if (tiles_in_bag >= 2 && (num_non_emptying > 0 || defer_pass)) {
    // Display Phase 1a and 1b in static-eval equity order.
    if (top_level) {
      const LetterDistribution *disp_ld = game_get_ld(args->game);
      double best_completed_wpct = -1.0;
      for (int i = 0; i < num_evaluated_be; i++) {
        if (!candidates[i].pruned && candidates[i].win_pct > best_completed_wpct)
          best_completed_wpct = candidates[i].win_pct;
      }

      // Phase 1a display (nonfull rack, greedy-only).
      if (num_bag_emptying_safe > 0) {
        qsort(candidates, num_bag_emptying_safe, sizeof(PegCandidate),
              compare_peg_candidates_by_equity_desc);
        int pruned_1a = 0;
        for (int i = 0; i < num_bag_emptying_safe; i++)
          pruned_1a += candidates[i].pruned ? 1 : 0;
        printf("[PEG] Phase 1a (bag-emptying, nonfull rack): "
               "%d candidates (%d pruned) in %.3fs\n",
               num_bag_emptying_safe, pruned_1a, phase1a_end);
        for (int i = 0; i < num_bag_emptying_safe; i++) {
          StringBuilder *sb = string_builder_create();
          string_builder_add_move(sb, game_get_board(base_game),
                                  &candidates[i].move, disp_ld, false);
          int score = equity_to_int(move_get_score(&candidates[i].move));
          if (candidates[i].pruned) {
            printf("  %3d. %-20s  score=%d  equity=%.1f  win%%<=%.1f%%"
                   "  spread=%+.2f  (pruned)\n",
                   i + 1, string_builder_peek(sb), score,
                   equity_to_double(move_get_equity(&candidates[i].move)),
                   candidates[i].win_pct * 100.0,
                   candidates[i].expected_value);
          } else {
            printf("  %3d. %-20s  score=%d  equity=%.1f  win%%=%.1f%%"
                   "  spread=%+.2f\n",
                   i + 1, string_builder_peek(sb), score,
                   equity_to_double(move_get_equity(&candidates[i].move)),
                   candidates[i].win_pct * 100.0,
                   candidates[i].expected_value);
          }
          string_builder_destroy(sb);
        }
      }

      // Phase 1b display (full rack, 1-ply where applicable).
      if (num_bag_emptying_bingo > 0 && !args->skip_phase_1b) {
        PegCandidate *bingo_start = candidates + num_bag_emptying_safe;
        qsort(bingo_start, num_bag_emptying_bingo, sizeof(PegCandidate),
              compare_peg_candidates_by_equity_desc);
        int pruned_1b = 0;
        for (int i = 0; i < num_bag_emptying_bingo; i++)
          pruned_1b += bingo_start[i].pruned ? 1 : 0;
        printf("[PEG] Phase 1b (bag-emptying, full rack): "
               "%d candidates (%d pruned) in %.3fs (%.3fs cumulative)\n",
               num_bag_emptying_bingo, pruned_1b,
               phase1b_end - phase1a_end, phase1b_end);
        for (int i = 0; i < num_bag_emptying_bingo; i++) {
          StringBuilder *sb = string_builder_create();
          string_builder_add_move(sb, game_get_board(base_game),
                                  &bingo_start[i].move, disp_ld, false);
          int score = equity_to_int(move_get_score(&bingo_start[i].move));
          if (bingo_start[i].pruned) {
            printf("  %3d. %-20s  score=%d  equity=%.1f  win%%<=%.1f%%"
                   "  spread=%+.2f  (pruned)\n",
                   i + 1, string_builder_peek(sb), score,
                   equity_to_double(move_get_equity(&bingo_start[i].move)),
                   bingo_start[i].win_pct * 100.0,
                   bingo_start[i].expected_value);
          } else {
            printf("  %3d. %-20s  score=%d  equity=%.1f  win%%=%.1f%%"
                   "  spread=%+.2f\n",
                   i + 1, string_builder_peek(sb), score,
                   equity_to_double(move_get_equity(&bingo_start[i].move)),
                   bingo_start[i].win_pct * 100.0,
                   bingo_start[i].expected_value);
          }
          string_builder_destroy(sb);
        }
      }
    }

    // Sort evaluated Phase 1 (bag-emptying) results by win% for subsequent
    // phases.  When Phase 1b was skipped, only Phase 1a candidates are valid.
    qsort(candidates, num_evaluated_be, sizeof(PegCandidate),
          compare_peg_candidates_desc);

    // If early_cutoff was not enabled for Phase 1, create cutoff now.
    if (!greedy_cutoff_active) {
      int tw = total_unseen * (total_unseen - 1);
      int ck = args->stage_candidate_limits[0];
      if (ck <= 0)
        ck = PEG_DEFAULT_STAGE_LIMIT_0;
      peg_cutoff_init(&greedy_cutoff, ck, tw, num_candidates);
      greedy_cutoff_active = true;
      for (int i = 0; i < num_evaluated_be; i++)
        if (!candidates[i].pruned)
          peg_cutoff_update(&greedy_cutoff, candidates[i].win_pct);
    }

    // Phase 2: non-bag-emptying candidates (1-tile plays), evaluated
    // sequentially in descending score order so logging is coherent.
    if (num_non_emptying > 0) {
      // Sort non-emptying slice by score descending.
      PegCandidate *ne_start = candidates + num_bag_emptying;
      qsort(ne_start, num_non_emptying, sizeof(PegCandidate),
            compare_peg_candidates_by_equity_desc);

      if (args->max_non_emptying > 0 && num_non_emptying > args->max_non_emptying)
        num_non_emptying = args->max_non_emptying;

      if (top_level) {
        printf("[PEG] Phase 2 (1-tile plays): %d candidates, sequential\n",
               num_non_emptying);
        int disp = num_non_emptying < 10 ? num_non_emptying : 10;
        for (int ci = 0; ci < disp; ci++) {
          StringBuilder *sb = string_builder_create();
          string_builder_add_move(sb, game_get_board(base_game), &ne_start[ci].move,
                                  game_get_ld(args->game), false);
          printf("  %3d. %-20s  equity=%.1f  score=%d\n", ci + 1,
                 string_builder_peek(sb),
                 equity_to_double(move_get_equity(&ne_start[ci].move)),
                 equity_to_int(move_get_score(&ne_start[ci].move)));
          string_builder_destroy(sb);
        }
      }

      // Evaluate non-emptying candidates sequentially (one at a time) so
      // that verbose output is not interleaved across candidates.
      for (int ci = 0; ci < num_non_emptying; ci++) {
        PegCandidate *c = &ne_start[ci];
        c->pruned = false;
        c->expected_value = peg_endgame_eval_recursive_candidate(
            base_game, &c->move, mover_idx, opp_idx,
            0 /* plies=0 for greedy */, unseen, ld_size, tiles_in_bag,
            c->move.tiles_played, args, NULL /* no shared_tt at greedy */,
            args->thread_index_base, num_threads,
            greedy_cutoff_active ? &greedy_cutoff : NULL,
            &c->win_pct, &c->pruned);
        if (top_level) {
          StringBuilder *sb = string_builder_create();
          string_builder_add_move(sb, game_get_board(base_game), &c->move,
                                  game_get_ld(args->game), false);
          if (c->pruned) {
            printf("  [Phase 2] %3d. %-20s  win%%<=%.1f%%  spread=%+.2f  (pruned)\n",
                   ci + 1, string_builder_peek(sb),
                   c->win_pct * 100.0, c->expected_value);
          } else {
            printf("  [Phase 2] %3d. %-20s  win%%=%.1f%%  spread=%+.2f\n",
                   ci + 1, string_builder_peek(sb),
                   c->win_pct * 100.0, c->expected_value);
          }
          string_builder_destroy(sb);
        }
        if (!c->pruned && greedy_cutoff_active)
          peg_cutoff_update(&greedy_cutoff, c->win_pct);
      }

      if (top_level) {
        double phase2_end = ctimer_elapsed_seconds(&peg_timer);
        printf("[PEG] Phase 2 done in %.3fs (%.3fs cumulative)\n",
               phase2_end - phase1b_end, phase2_end);
      }
    }

    // Phase 3: pass evaluation (deferred from Phase 1).
    if (defer_pass) {
      if (top_level)
        printf("[PEG] Phase 3 (pass): %d scenarios\n",
               greedy_num_scenarios);

      double phase3_start = ctimer_elapsed_seconds(&peg_timer);
      atomic_init(&greedy_next, 0);
      atomic_init(&greedy_progress, 0);
      // Pass scenarios only — no non-pass candidates in this phase.
      for (int ti = 0; ti < num_threads; ti++) {
        greedy_targs[ti].candidates = candidates; // unused for pass
        greedy_targs[ti].num_non_pass = 0;
        greedy_targs[ti].num_work_items = greedy_num_scenarios;
        greedy_targs[ti].cutoff = NULL;
        greedy_targs[ti].progress = top_level ? &greedy_progress : NULL;
        cpthread_create(&greedy_threads[ti], peg_greedy_thread,
                        &greedy_targs[ti]);
      }
      for (int ti = 0; ti < num_threads; ti++) {
        cpthread_join(greedy_threads[ti]);
      }

      // Aggregate pass scenario results.
      PegCandidate *pass_c = &candidates[num_candidates - 1];
      double total = 0.0, wins = 0.0;
      int weight = 0;
      for (int si = 0; si < greedy_num_scenarios; si++) {
        total += greedy_pass_spreads[si] * greedy_scenario_counts[si];
        wins += greedy_pass_wins[si] * greedy_scenario_counts[si];
        weight += greedy_scenario_counts[si];
      }
      pass_c->win_pct = (weight > 0) ? wins / weight : 0.0;
      pass_c->expected_value = (weight > 0) ? total / weight : 0.0;
      pass_c->pruned = false;
      peg_cutoff_update(&greedy_cutoff, pass_c->win_pct);

      if (top_level) {
        double phase3_end = ctimer_elapsed_seconds(&peg_timer);
        printf("[PEG] Phase 3 done in %.3fs (%.3fs cumulative), "
               "pass win%%=%.1f%%\n",
               phase3_end - phase3_start, phase3_end,
               pass_c->win_pct * 100.0);
      }
    }

  }

  if (greedy_cutoff_active)
    peg_cutoff_destroy(&greedy_cutoff);

  // Clean up greedy infrastructure.
  for (int ti = 0; ti < num_threads; ti++) {
    endgame_solver_worker_destroy(greedy_workers[ti]);
    endgame_solver_destroy(greedy_solvers[ti]);
  }
  free(greedy_workers);
  free(greedy_solvers);
  free(greedy_targs);
  free(greedy_threads);
  free(greedy_scenario_t1);
  free(greedy_scenario_t2);
  free(greedy_scenario_counts);
  free(greedy_pass_spreads);
  free(greedy_pass_wins);

  // Sort all candidates by expected value (descending).
  qsort(candidates, num_candidates, sizeof(PegCandidate),
        compare_peg_candidates_desc);

  double prev_elapsed = ctimer_elapsed_seconds(&peg_timer);
  invoke_per_pass_callback(args, 0, num_candidates, candidates, num_candidates,
                           args->stage_candidate_limits[0], prev_elapsed,
                           prev_elapsed);

  int stages_completed = 1; // stage 0 (greedy) is already done

  // =========================================================================
  // Stages 1..num_stages-1: progressively deeper endgame search on top-K
  // =========================================================================

  // Use a single shared TT for all endgame stages and threads.  The TT
  // uses lockless hashing (atomic loads/stores) so concurrent access is safe.
  // Entries from shallower stages remain valid at deeper depths thanks to the
  // depth guard in the endgame solver's TT lookup (ttentry_depth >= depth).
  bool tt_is_owned = false;
  TranspositionTable *shared_tt = args->shared_tt;
  if (!shared_tt && args->num_stages > 1 && args->tt_fraction_of_mem > 0) {
    shared_tt = transposition_table_create(args->tt_fraction_of_mem);
    tt_is_owned = true;
  }

  for (int stage = 1; stage < args->num_stages; stage++) {
    // Check time budget.
    if (args->time_budget_seconds > 0.0 &&
        ctimer_elapsed_seconds(&peg_timer) >= args->time_budget_seconds) {
      break;
    }

    int plies = stage;
    int limit = args->stage_candidate_limits[stage - 1];
    if (limit <= 0) {
      // Apply built-in defaults when the caller left the limit unset.
      static const int kDefaultLimits[] = {
          PEG_DEFAULT_STAGE_LIMIT_0, PEG_DEFAULT_STAGE_LIMIT_1,
          PEG_DEFAULT_STAGE_LIMIT_2, PEG_DEFAULT_STAGE_LIMIT_3,
          PEG_DEFAULT_STAGE_LIMIT_4,
      };
      int ndefaults = (int)(sizeof(kDefaultLimits) / sizeof(kDefaultLimits[0]));
      int di = stage - 1;
      limit = (di < ndefaults) ? kDefaultLimits[di] : PEG_DEFAULT_STAGE_LIMIT_4;
    }
    if (limit > num_candidates)
      limit = num_candidates;

    // Set up per-thread endgame solvers and results.
    EndgameSolver **eg_solvers =
        malloc_or_die(num_threads * sizeof(EndgameSolver *));
    EndgameResults **eg_results =
        malloc_or_die(num_threads * sizeof(EndgameResults *));
    for (int ti = 0; ti < num_threads; ti++) {
      eg_solvers[ti] = endgame_solver_create();
      eg_results[ti] = endgame_results_create();
    }

    // Early cutoff: determine K (how many candidates must survive).
    // Final stage: only the best matters (K=1).
    // Non-final: K = next stage's limit.
    PegCutoff cutoff_state;
    PegCutoff *cutoff_ptr = NULL;
    if (args->early_cutoff) {
      int cutoff_k;
      if (stage == args->num_stages - 1) {
        cutoff_k = 1;
      } else {
        int next_limit = args->stage_candidate_limits[stage];
        if (next_limit <= 0) {
          static const int kDefaults[] = {
              PEG_DEFAULT_STAGE_LIMIT_0, PEG_DEFAULT_STAGE_LIMIT_1,
              PEG_DEFAULT_STAGE_LIMIT_2, PEG_DEFAULT_STAGE_LIMIT_3,
              PEG_DEFAULT_STAGE_LIMIT_4,
          };
          int nd = (int)(sizeof(kDefaults) / sizeof(kDefaults[0]));
          int di = stage;
          next_limit = (di < nd) ? kDefaults[di] : PEG_DEFAULT_STAGE_LIMIT_4;
        }
        cutoff_k = next_limit < limit ? next_limit : limit;
      }
      int total_weight = (tiles_in_bag == 1) ? total_unseen
                                              : total_unseen * (total_unseen - 1);
      peg_cutoff_init(&cutoff_state, cutoff_k, total_weight, limit);
      cutoff_ptr = &cutoff_state;
    }

    // Separate pass from non-pass candidates in the top-K range.
    int eg_pass_idx = -1;
    for (int i = 0; i < limit; i++) {
      if (move_get_type(&candidates[i].move) == GAME_EVENT_PASS) {
        eg_pass_idx = i;
        break;
      }
    }
    int eg_non_pass_count = limit;
    if (eg_pass_idx >= 0) {
      PegCandidate tmp = candidates[eg_pass_idx];
      candidates[eg_pass_idx] = candidates[limit - 1];
      candidates[limit - 1] = tmp;
      eg_non_pass_count = limit - 1;
    }

    // Build pass scenario arrays for the unified work queue.
    int eg_num_scenarios = 0;
    MachineLetter *eg_scenario_t1 =
        malloc_or_die(max_scenarios * sizeof(MachineLetter));
    MachineLetter *eg_scenario_t2 =
        malloc_or_die(max_scenarios * sizeof(MachineLetter));
    int *eg_scenario_counts = malloc_or_die(max_scenarios * sizeof(int));
    double *eg_pass_spreads = malloc_or_die(max_scenarios * sizeof(double));
    double *eg_pass_wins = malloc_or_die(max_scenarios * sizeof(double));
    if (eg_pass_idx >= 0) {
      if (tiles_in_bag == 1) {
        for (int t = 0; t < ld_size; t++) {
          if (unseen[t] > 0) {
            eg_scenario_t1[eg_num_scenarios] = (MachineLetter)t;
            eg_scenario_t2[eg_num_scenarios] = (MachineLetter)-1;
            eg_scenario_counts[eg_num_scenarios] = (int)unseen[t];
            eg_num_scenarios++;
          }
        }
      } else {
        for (int t1 = 0; t1 < ld_size; t1++) {
          if (unseen[t1] == 0)
            continue;
          for (int t2 = 0; t2 < ld_size; t2++) {
            if (unseen[t2] == 0)
              continue;
            if (t1 == t2 && unseen[t1] < 2)
              continue;
            eg_scenario_t1[eg_num_scenarios] = (MachineLetter)t1;
            eg_scenario_t2[eg_num_scenarios] = (MachineLetter)t2;
            eg_scenario_counts[eg_num_scenarios] =
                (int)unseen[t1] *
                (t1 == t2 ? (int)unseen[t1] - 1 : (int)unseen[t2]);
            eg_num_scenarios++;
          }
        }
      }
    }
    PegEndgameThreadArgs *eg_targs =
        malloc_or_die(num_threads * sizeof(PegEndgameThreadArgs));
    cpthread_t *eg_threads = malloc_or_die(num_threads * sizeof(cpthread_t));

    // Process non-pass candidates one at a time (sequential) so verbose
    // output for each candidate is not interleaved with other candidates.
    for (int ci = 0; ci < eg_non_pass_count; ci++) {
      PegCandidate *c = &candidates[ci];
      bool pruned = false;
      c->expected_value = peg_endgame_eval_candidate(
          eg_solvers[0], eg_results[0], base_game, &c->move,
          mover_idx, opp_idx, plies, unseen, ld_size,
          tiles_in_bag, args->thread_control, shared_tt,
          args->dual_lexicon_mode, args->thread_index_base,
          args, cutoff_ptr, &c->win_pct, &pruned);
      c->pruned = pruned;
      if (!pruned && cutoff_ptr) {
        peg_cutoff_update(cutoff_ptr, c->win_pct);
      }
    }

    // Pass scenarios can run in parallel (no verbose per-candidate output).
    if (eg_num_scenarios > 0) {
      atomic_int next_work_item;
      atomic_init(&next_work_item, 0);
      for (int ti = 0; ti < num_threads; ti++) {
        eg_targs[ti] = (PegEndgameThreadArgs){
            .candidates = candidates,
            .next_work_item = &next_work_item,
            .num_non_pass = 0,
            .num_work_items = eg_num_scenarios,
            .endgame_solver = eg_solvers[ti],
            .endgame_results = eg_results[ti],
            .base_game = base_game,
            .mover_idx = mover_idx,
            .opp_idx = opp_idx,
            .plies = plies,
            .unseen = unseen,
            .ld_size = ld_size,
            .tiles_in_bag = tiles_in_bag,
            .thread_index = args->thread_index_base + ti,
            .thread_control = args->thread_control,
            .shared_tt = shared_tt,
            .dual_lexicon_mode = args->dual_lexicon_mode,
            .solver = solver,
            .outer_args = args,
            .cutoff = NULL,
            .scenario_t1 = eg_scenario_t1,
            .scenario_t2 = eg_scenario_t2,
            .scenario_counts = eg_scenario_counts,
            .pass_spreads = eg_pass_spreads,
            .pass_wins = eg_pass_wins,
        };
        cpthread_create(&eg_threads[ti], peg_endgame_thread, &eg_targs[ti]);
      }
      for (int ti = 0; ti < num_threads; ti++) {
        cpthread_join(eg_threads[ti]);
      }
    }

    // Aggregate pass scenario results into the pass candidate.
    if (eg_pass_idx >= 0) {
      PegCandidate *pass_c = &candidates[limit - 1];
      double total = 0.0, wins = 0.0;
      int weight = 0;
      for (int si = 0; si < eg_num_scenarios; si++) {
        total += eg_pass_spreads[si] * eg_scenario_counts[si];
        wins += eg_pass_wins[si] * eg_scenario_counts[si];
        weight += eg_scenario_counts[si];
      }
      pass_c->win_pct = (weight > 0) ? wins / weight : 0.0;
      pass_c->expected_value = (weight > 0) ? total / weight : 0.0;
      pass_c->pruned = false;
    }

    if (cutoff_ptr) {
      peg_cutoff_destroy(cutoff_ptr);
    }

    for (int ti = 0; ti < num_threads; ti++) {
      endgame_solver_destroy(eg_solvers[ti]);
      endgame_results_destroy(eg_results[ti]);
    }
    free(eg_solvers);
    free(eg_results);
    free(eg_targs);
    free(eg_threads);
    free(eg_scenario_t1);
    free(eg_scenario_t2);
    free(eg_scenario_counts);
    free(eg_pass_spreads);
    free(eg_pass_wins);

    // Sort the top-limit candidates by their new values.
    qsort(candidates, limit, sizeof(PegCandidate), compare_peg_candidates_desc);

    stages_completed++;
    num_candidates = limit;
    double now = ctimer_elapsed_seconds(&peg_timer);
    invoke_per_pass_callback(args, stage, limit, candidates, num_candidates,
                             num_candidates, now, now - prev_elapsed);
    prev_elapsed = now;
  }

  // =========================================================================
  // Return the best candidate
  // =========================================================================

  result->best_move = candidates[0].move;
  result->best_win_pct = candidates[0].win_pct;
  result->best_expected_spread = candidates[0].expected_value;
  result->stages_completed = stages_completed;
  result->candidates_remaining = num_candidates;

  free(candidates);
  game_destroy(base_game);
  if (tt_is_owned) {
    transposition_table_destroy(shared_tt);
  }
}
