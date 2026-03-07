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
#include "../util/io_util.h"
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
  SmallMove move;
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

// ---------------------------------------------------------------------------
// Greedy evaluation of a play (non-pass) candidate
// ---------------------------------------------------------------------------

// Evaluates a play move by enumerating all possible bag tiles (weighted by
// probability). For each bag tile T:
//   - plays the candidate (bag empty in game_copy, so no automatic draw)
//   - adds T to the mover's rack
//   - sets the opponent's rack to (unseen - T)
//   - calls negamax_greedy_leaf_playout
//   - undoes everything
// Returns the weighted expected spread from the mover's perspective.
static double peg_greedy_eval_play(EndgameSolverWorker *worker,
                                   const SmallMove *move, int mover_idx,
                                   int opp_idx,
                                   const uint8_t unseen[MAX_ALPHABET_SIZE],
                                   int ld_size, int total_unseen,
                                   double *win_pct_out) {
  Game *game_copy = endgame_solver_worker_get_game(worker);
  Board *board = game_get_board(game_copy);
  Rack *mover_rack = player_get_rack(game_get_player(game_copy, mover_idx));
  Rack *opp_rack = player_get_rack(game_get_player(game_copy, opp_idx));

  // Convert SmallMove to Move and play it onto the game_copy.
  Move m;
  small_move_to_move(&m, move, board);
  MoveUndo candidate_undo;
  move_undo_reset(&candidate_undo);
  MoveUndo *candidate_undo_ptr = &candidate_undo;
  play_move_incremental(&m, game_copy, candidate_undo_ptr);
  // play_move_incremental assumes the bag is empty. If the candidate was a
  // bingo (rack now empty), it falsely triggers GAME_END_REASON_STANDARD and
  // adds 2x opp's rack to mover's score. Undo that — PEG manually simulates
  // the draw in the per-scenario loop below.
  if (game_get_game_end_reason(game_copy) == GAME_END_REASON_STANDARD) {
    Equity end_pts =
        calculate_end_rack_points(opp_rack, game_get_ld(game_copy));
    player_add_to_score(game_get_player(game_copy, mover_idx), -end_pts);
    game_set_game_end_reason(game_copy, GAME_END_REASON_NONE);
  }

  // Update cross-sets after the candidate play so the greedy playout's
  // stuck-tile heuristic and move generation work correctly.
  if (!board_get_cross_sets_valid(board)) {
    if (candidate_undo_ptr->move_tiles_length > 0) {
      update_cross_set_for_move_from_undo(candidate_undo_ptr, game_copy);
    }
    board_set_cross_sets_valid(board, true);
  }

  // Temporarily pretend we are at depth 1 so negamax_greedy_leaf_playout
  // uses undo slots 1+ and does not clobber slot 0 (the candidate undo).
  int saved_plies = endgame_solver_worker_get_requested_plies(worker);
  endgame_solver_worker_set_requested_plies(worker, 1);

  // After playing the candidate, it is now the opponent's turn.
  // negamax_greedy_leaf_playout returns spread from solving_player's
  // perspective (= mover_idx, set during endgame_solver_reset).

  double total = 0.0;
  double wins = 0.0;
  int weight = 0;
  PVLine pv;

  for (int t = 0; t < ld_size; t++) {
    int cnt = (int)unseen[t];
    if (cnt == 0)
      continue;
    // Mover draws bag tile t.
    rack_add_letter(mover_rack, (MachineLetter)t);
    // Set opponent's rack to (unseen - {t}).
    Rack saved_opp;
    rack_copy(&saved_opp, opp_rack);
    set_opp_rack_for_scenario(opp_rack, unseen, ld_size, (MachineLetter)t);

    // Greedy playout from the opponent's turn (on_turn = opp_idx).
    // Returns spread from on_turn's (opp's) perspective; negate for mover.
    // The playout includes pass as a candidate; if both players pass
    // the game ends naturally with CONSECUTIVE_ZEROS.
    int32_t mover_spread = -negamax_greedy_leaf_playout(worker, 0, opp_idx, 0,
                                                         &pv, 0.0f);
    // Weight by the number of copies of this tile in the unseen pool.
    total += (double)mover_spread * cnt;
    wins += ((mover_spread > 0) ? 1.0 : (mover_spread == 0 ? 0.5 : 0.0)) * cnt;
    weight += cnt;

    rack_take_letter(mover_rack, (MachineLetter)t);
    rack_copy(opp_rack, &saved_opp);
  }

  endgame_solver_worker_set_requested_plies(worker, saved_plies);

  // Unplay the candidate move.
  unplay_move_incremental(game_copy, candidate_undo_ptr);
  if (candidate_undo_ptr->move_tiles_length > 0) {
    update_cross_sets_after_unplay_from_undo(candidate_undo_ptr, game_copy);
    board_set_cross_sets_valid(board, true);
  }

  if (weight == 0) {
    *win_pct_out = 0.0;
    return 0.0;
  }
  (void)total_unseen;
  *win_pct_out = wins / weight;
  return total / weight;
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
                                    const SmallMove *move, int mover_idx,
                                    int opp_idx, MachineLetter bag_tile,
                                    const uint8_t unseen[MAX_ALPHABET_SIZE],
                                    int ld_size) {
  Game *g = game_duplicate(base_game);
  game_set_endgame_solving_mode(g);
  game_set_backup_mode(g, BACKUP_MODE_OFF);

  // Play the candidate onto the board (no draw since base_game bag is empty).
  Move m;
  small_move_to_move(&m, move, game_get_board(g));
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

// Evaluate one candidate move across all bag-tile scenarios using K-ply
// endgame search. Returns the weighted expected spread from the mover's
// perspective (positive = mover winning).
static double peg_endgame_eval_candidate(EndgameSolver *endgame_solver,
                                         EndgameResults *results,
                                         const Game *base_game,
                                         const SmallMove *move, int mover_idx,
                                         int opp_idx, int plies,
                                         const uint8_t unseen[MAX_ALPHABET_SIZE],
                                         int ld_size,
                                         ThreadControl *tc,
                                         TranspositionTable *shared_tt,
                                         dual_lexicon_mode_t dual_lexicon_mode,
                                         PegCutoff *cutoff,
                                         double *win_pct_out,
                                         bool *pruned_out) {
  double total = 0.0;
  double wins = 0.0;
  int weight = 0;
  *pruned_out = false;
  ErrorStack *local_es = error_stack_create();

  for (int t = 0; t < ld_size; t++) {
    int cnt = (int)unseen[t];
    if (cnt == 0)
      continue;

    // Early cutoff: if even winning all remaining scenarios can't reach the
    // K-th best, stop evaluating and report the upper bound.
    if (cutoff) {
      double threshold = peg_cutoff_get(cutoff);
      if (threshold >= 0) {
        int remaining = cutoff->total_weight - weight;
        double best_possible = (wins + remaining) / (double)cutoff->total_weight;
        if (best_possible < threshold - 1e-9) {
          *win_pct_out = best_possible;
          *pruned_out = true;
          error_stack_destroy(local_es);
          return total / (weight > 0 ? weight : 1);
        }
      }
    }

    Game *scenario =
        setup_endgame_scenario(base_game, move, mover_idx, opp_idx,
                               (MachineLetter)t, unseen, ld_size);

    // Read the score differential before endgame_solve: endgame_results_get_value
    // returns the *incremental* spread (val - initial_spread), not the
    // absolute game result.  We need the current lead to recover the true
    // final spread and determine the correct win/loss outcome.
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
    };

    error_stack_reset(local_es);
    endgame_solve(endgame_solver, &ea, results, local_es);

    // endgame_val = opp's incremental endgame advantage (from opp's perspective).
    // mover_total = mover_lead + (-endgame_val) = absolute final spread for mover.
    int endgame_val =
        endgame_results_get_value(results, ENDGAME_RESULT_BEST);
    int32_t mover_total = mover_lead - endgame_val;
    // Weight by the number of copies of this tile in the unseen pool.
    total += (double)mover_total * cnt;
    wins += ((mover_total > 0) ? 1.0 : (mover_total == 0 ? 0.5 : 0.0)) * cnt;
    weight += cnt;

    game_destroy(scenario);
  }

  error_stack_destroy(local_es);
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

// Evaluates a pass by calling peg_solve from the opponent's perspective.
// After the mover passes, the opponent faces their own 1-PEG position:
// the bag still has 1 tile, and the opponent (now on turn) must decide
// their best play without knowing which tile is in the bag.
//
// The inner peg_solve uses skip_pass=1 (prevents infinite recursion) and
// num_passes = plies-1 (lags the outer pipeline by one stage), so the
// opp's move selection mirrors the main PEG pipeline at one stage less
// depth.  Opp's choice is based on expected value (imperfect info).
//
// For each possible bag tile T (weighted by unseen[T]):
//   - Call inner peg_solve to find opp's best tile play.
//   - Compute the "opp also passes" outcome: both players lose their rack
//     tile values (2-consecutive-passes game-ending rule).
//   - Opp picks whichever option gives better expected value.
//   - Score the ACTUAL outcome with draw=T (deterministic endgame).
static void peg_eval_pass_recursive(const PegArgs *outer_args,
                                    int opp_idx,
                                    const uint8_t unseen[MAX_ALPHABET_SIZE],
                                    int ld_size, int plies,
                                    TranspositionTable *shared_tt,
                                    PegCutoff *cutoff,
                                    double *win_pct_out,
                                    double *expected_value_out,
                                    bool *pruned_out) {
  int mover_idx = 1 - opp_idx;
  const LetterDistribution *ld = game_get_ld(outer_args->game);
  double total = 0.0;
  double wins = 0.0;
  int weight = 0;
  if (pruned_out)
    *pruned_out = false;

  ErrorStack *inner_es = error_stack_create();
  EndgameSolver *eg_solver = endgame_solver_create();
  EndgameResults *eg_results = endgame_results_create();

  for (int t = 0; t < ld_size; t++) {
    int cnt = (int)unseen[t];
    if (cnt == 0)
      continue;

    // Early cutoff check.
    if (cutoff) {
      double threshold = peg_cutoff_get(cutoff);
      if (threshold >= 0) {
        int remaining = cutoff->total_weight - weight;
        double best_possible = (wins + remaining) / (double)cutoff->total_weight;
        if (best_possible < threshold - 1e-9) {
          *win_pct_out = best_possible;
          *expected_value_out = (weight > 0) ? total / weight : 0.0;
          if (pruned_out)
            *pruned_out = true;
          goto cleanup;
        }
      }
    }

    // Create inner game: opp on turn, rack = unseen-{T}.
    Game *inner_game = game_duplicate(outer_args->game);
    Rack *opp_rack = player_get_rack(game_get_player(inner_game, opp_idx));
    set_opp_rack_for_scenario(opp_rack, unseen, ld_size, (MachineLetter)t);
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
    int inner_passes = plies > 0 ? plies - 1 : 0;
    PegSolver inner_solver = {0};
    PegArgs inner_args = {
        .game = inner_game,
        .thread_control = outer_args->thread_control,
        .time_budget_seconds = 0.0,
        .num_threads = 1,
        .tt_fraction_of_mem = outer_args->tt_fraction_of_mem,
        .dual_lexicon_mode = outer_args->dual_lexicon_mode,
        .num_passes = inner_passes,
        .skip_pass = 1,
        .shared_tt = shared_tt,
    };
    // Copy pass_candidate_limits from outer args.
    for (int i = 0; i < PEG_MAX_PASSES; i++)
      inner_args.pass_candidate_limits[i] =
          outer_args->pass_candidate_limits[i];

    PegResult inner_result;
    error_stack_reset(inner_es);
    peg_solve(&inner_solver, &inner_args, &inner_result, inner_es);

    bool found_tile_play =
        error_stack_is_empty(inner_es) &&
        !small_move_is_pass(&inner_result.best_move);
    double opp_play_expected_win = inner_result.best_win_pct;
    double opp_play_expected_spread = inner_result.best_expected_spread;

    // Drain the bag from inner_game so setup_endgame_scenario works correctly.
    // inner_game was duplicated from outer_args->game which has 1 tile in the
    // bag. setup_endgame_scenario assumes an empty bag (play_move must not draw).
    {
      Bag *ibag = game_get_bag(inner_game);
      for (int ml = 0; ml < ld_size; ml++) {
        while (bag_get_letter(ibag, ml) > 0)
          bag_draw_letter(ibag, (MachineLetter)ml, opp_idx);
      }
    }

    // Opp picks whichever option gives better expected outcome.
    // Primary: higher win_pct. Secondary: higher expected spread.
    bool opp_chose_pass;
    if (!found_tile_play ||
        opp_pass_expected_win > opp_play_expected_win + 1e-9 ||
        (opp_pass_expected_win >= opp_play_expected_win - 1e-9 &&
         opp_pass_expected_spread > opp_play_expected_spread)) {
      opp_chose_pass = true;
    } else {
      opp_chose_pass = false;
    }

    // Score the ACTUAL outcome with the known bag tile T.
    double opp_spread;
    if (opp_chose_pass) {
      opp_spread = (double)both_pass_opp_spread;
    } else {
      // Set up the specific scenario: opp plays their chosen move,
      // draws the known bag tile T, endgame solved deterministically.
      // Need unseen from opp's perspective for setup_endgame_scenario.
      // Unseen from opp's perspective = mover's rack + bag tile t.
      // No need to duplicate the game: compute_unseen doesn't use the bag,
      // and the algebra simplifies to mover_rack[ml] + (ml == t ? 1 : 0).
      uint8_t inner_unseen[MAX_ALPHABET_SIZE];
      {
        const Rack *mr =
            player_get_rack(game_get_player(inner_game, mover_idx));
        for (int ml = 0; ml < ld_size; ml++) {
          inner_unseen[ml] = (uint8_t)rack_get_letter(mr, ml);
        }
        inner_unseen[t]++;
      }

      Game *scenario = setup_endgame_scenario(
          inner_game, &inner_result.best_move, opp_idx, mover_idx,
          (MachineLetter)t, inner_unseen, ld_size);

      int32_t opp_lead =
          equity_to_int(
              player_get_score(game_get_player(scenario, opp_idx))) -
          equity_to_int(
              player_get_score(game_get_player(scenario, mover_idx)));

      int solve_plies = plies > 0 ? plies : 1;
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
      };
      error_stack_reset(inner_es);
      endgame_solve(eg_solver, &ea, eg_results, inner_es);

      // endgame_val from solving_player's (mover's) perspective.
      int endgame_val =
          endgame_results_get_value(eg_results, ENDGAME_RESULT_BEST);
      int32_t opp_total = opp_lead - endgame_val;
      opp_spread = (double)opp_total;

      game_destroy(scenario);
    }

    // Convert to mover's perspective, weighted by tile count.
    double mover_win =
        (opp_spread < 0) ? 1.0 : (opp_spread == 0 ? 0.5 : 0.0);
    total += -opp_spread * cnt;
    wins += mover_win * cnt;
    weight += cnt;

    game_destroy(inner_game);
  }

  if (weight == 0) {
    *win_pct_out = 0.0;
    *expected_value_out = 0.0;
  } else {
    *win_pct_out = wins / weight;
    *expected_value_out = total / weight;
  }

cleanup:
  endgame_results_destroy(eg_results);
  endgame_solver_destroy(eg_solver);
  error_stack_destroy(inner_es);
}

// ---------------------------------------------------------------------------
// Thread arguments and worker functions — greedy pass
// ---------------------------------------------------------------------------

typedef struct PegGreedyThreadArgs {
  PegCandidate *candidates;
  int start;
  int end;
  EndgameSolverWorker *worker;
  int mover_idx;
  int opp_idx;
  const uint8_t *unseen;
  int ld_size;
  int total_unseen;
  // For recursive pass evaluation.
  PegSolver *solver;
  const PegArgs *outer_args;
} PegGreedyThreadArgs;

static void *peg_greedy_thread(void *arg) {
  PegGreedyThreadArgs *a = (PegGreedyThreadArgs *)arg;
  for (int i = a->start; i < a->end; i++) {
    PegCandidate *c = &a->candidates[i];
    c->pruned = false;
    if (small_move_is_pass(&c->move)) {
      peg_eval_pass_recursive(a->outer_args, a->opp_idx,
                              a->unseen, a->ld_size, 0, NULL, NULL,
                              &c->win_pct, &c->expected_value, NULL);
    } else {
      c->expected_value =
          peg_greedy_eval_play(a->worker, &c->move, a->mover_idx, a->opp_idx,
                               a->unseen, a->ld_size, a->total_unseen,
                               &c->win_pct);
    }
  }
  return NULL;
}

// ---------------------------------------------------------------------------
// Thread arguments and worker function — endgame passes
// ---------------------------------------------------------------------------

typedef struct PegEndgameThreadArgs {
  PegCandidate *candidates;
  atomic_int *next_candidate; // work-stealing index
  int num_candidates;
  EndgameSolver *endgame_solver; // one per thread
  EndgameResults *endgame_results;
  const Game *base_game;
  int mover_idx;
  int opp_idx;
  int plies;
  const uint8_t *unseen;
  int ld_size;
  ThreadControl *thread_control;
  TranspositionTable *shared_tt;
  dual_lexicon_mode_t dual_lexicon_mode;
  // For recursive pass evaluation.
  PegSolver *solver;
  const PegArgs *outer_args;
  // Early cutoff tracker (NULL if disabled).
  PegCutoff *cutoff;
} PegEndgameThreadArgs;

static void *peg_endgame_thread(void *arg) {
  PegEndgameThreadArgs *a = (PegEndgameThreadArgs *)arg;
  while (true) {
    int idx = atomic_fetch_add(a->next_candidate, 1);
    if (idx >= a->num_candidates)
      break;
    PegCandidate *c = &a->candidates[idx];
    bool pruned = false;
    if (small_move_is_pass(&c->move)) {
      peg_eval_pass_recursive(a->outer_args, a->opp_idx,
                              a->unseen, a->ld_size, a->plies,
                              a->shared_tt, a->cutoff,
                              &c->win_pct, &c->expected_value, &pruned);
    } else {
      c->expected_value = peg_endgame_eval_candidate(
          a->endgame_solver, a->endgame_results, a->base_game, &c->move,
          a->mover_idx, a->opp_idx, a->plies, a->unseen, a->ld_size,
          a->thread_control, a->shared_tt, a->dual_lexicon_mode,
          a->cutoff, &c->win_pct, &pruned);
    }
    c->pruned = pruned;
    if (!pruned && a->cutoff) {
      peg_cutoff_update(a->cutoff, c->win_pct);
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
  SmallMove moves[PEG_CALLBACK_MAX_TOP];
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
                         "but found %d",
                         total_unseen));
    return;
  }
  // The opponent receives (total_unseen - 1) tiles per scenario. If this
  // exceeds RACK_SIZE the move generator would index out of its tile-score
  // arrays. Reject positions where the opponent would need an over-full rack.
  if (total_unseen > RACK_SIZE + 1) {
    error_stack_push(error_stack, ERROR_STATUS_ENDGAME_BAG_NOT_EMPTY,
                     get_formatted_string(
                         "peg_solve: %d unseen tiles would give opponent %d "
                         "tiles, exceeding RACK_SIZE=%d",
                         total_unseen, total_unseen - 1, RACK_SIZE));
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
  MoveList *initial_ml = move_list_create_small(PEG_MOVELIST_CAPACITY);
  {
    const MoveGenArgs gen_args = {
        .game = base_game,
        .move_list = initial_ml,
        .move_record_type = MOVE_RECORD_ALL_SMALL,
        .move_sort_type = MOVE_SORT_SCORE,
        .override_kwg = NULL,
        .thread_index = 0,
        .eq_margin_movegen = 0,
        .target_equity = EQUITY_MAX_VALUE,
        .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
    };
    generate_moves(&gen_args);
  }
  int num_candidates = initial_ml->count;
  if (num_candidates == 0) {
    small_move_list_destroy(initial_ml);
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
      if (args->skip_pass && small_move_is_pass(initial_ml->small_moves[i]))
        continue;
      candidates[j].move = *initial_ml->small_moves[i];
      candidates[j].expected_value = 0.0;
      j++;
    }
    num_candidates = j;
  }
  small_move_list_destroy(initial_ml);

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

    greedy_workers[ti] = endgame_solver_create_worker(
        greedy_solvers[ti], ti, (uint64_t)ti * 54321 + 1);
  }

  // Distribute candidates evenly among threads.
  int chunk = (num_candidates + num_threads - 1) / num_threads;
  PegGreedyThreadArgs *greedy_targs =
      malloc_or_die(num_threads * sizeof(PegGreedyThreadArgs));
  cpthread_t *greedy_threads = malloc_or_die(num_threads * sizeof(cpthread_t));

  for (int ti = 0; ti < num_threads; ti++) {
    greedy_targs[ti] = (PegGreedyThreadArgs){
        .candidates = candidates,
        .start = ti * chunk,
        .end = (ti + 1) * chunk > num_candidates ? num_candidates
                                                  : (ti + 1) * chunk,
        .worker = greedy_workers[ti],
        .mover_idx = mover_idx,
        .opp_idx = opp_idx,
        .unseen = unseen,
        .ld_size = ld_size,
        .total_unseen = total_unseen,
        .solver = solver,
        .outer_args = args,
    };
    cpthread_create(&greedy_threads[ti], peg_greedy_thread, &greedy_targs[ti]);
  }
  for (int ti = 0; ti < num_threads; ti++) {
    cpthread_join(greedy_threads[ti]);
  }

  // Clean up greedy infrastructure.
  for (int ti = 0; ti < num_threads; ti++) {
    endgame_solver_worker_destroy(greedy_workers[ti]);
    endgame_solver_destroy(greedy_solvers[ti]);
  }
  free(greedy_workers);
  free(greedy_solvers);
  free(greedy_targs);
  free(greedy_threads);

  // Sort by expected value (descending).
  qsort(candidates, num_candidates, sizeof(PegCandidate),
        compare_peg_candidates_desc);

  double prev_elapsed = ctimer_elapsed_seconds(&peg_timer);
  invoke_per_pass_callback(args, 0, num_candidates, candidates, num_candidates,
                           args->pass_candidate_limits[0], prev_elapsed,
                           prev_elapsed);

  int passes_completed = 0;

  // =========================================================================
  // Passes 1..num_passes: progressively deeper endgame search on top-K
  // =========================================================================

  // Use a single shared TT for all endgame passes and threads.  The TT
  // uses lockless hashing (atomic loads/stores) so concurrent access is safe.
  // Entries from shallower passes remain valid at deeper depths thanks to the
  // depth guard in the endgame solver's TT lookup (ttentry_depth >= depth).
  bool tt_is_owned = false;
  TranspositionTable *shared_tt = args->shared_tt;
  if (!shared_tt && args->num_passes > 0 && args->tt_fraction_of_mem > 0) {
    shared_tt = transposition_table_create(args->tt_fraction_of_mem);
    tt_is_owned = true;
  }

  for (int pass = 0; pass < args->num_passes; pass++) {
    // Check time budget.
    if (args->time_budget_seconds > 0.0 &&
        ctimer_elapsed_seconds(&peg_timer) >= args->time_budget_seconds) {
      break;
    }

    int plies = pass + 1;
    int limit = args->pass_candidate_limits[pass];
    if (limit <= 0) {
      // Apply built-in defaults when the caller left the limit unset.
      static const int kDefaultLimits[] = {
          PEG_DEFAULT_PASS0_LIMIT, PEG_DEFAULT_PASS1_LIMIT,
          PEG_DEFAULT_PASS2_LIMIT, PEG_DEFAULT_PASS3_LIMIT,
          PEG_DEFAULT_PASS4_LIMIT,
      };
      int ndefaults = (int)(sizeof(kDefaultLimits) / sizeof(kDefaultLimits[0]));
      limit = (pass < ndefaults) ? kDefaultLimits[pass] : PEG_DEFAULT_PASS4_LIMIT;
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
    // Final pass: only the best matters (K=1).
    // Non-final: K = next pass's limit.
    PegCutoff cutoff_state;
    PegCutoff *cutoff_ptr = NULL;
    if (args->early_cutoff) {
      int cutoff_k;
      if (pass == args->num_passes - 1) {
        cutoff_k = 1;
      } else {
        int next_limit = args->pass_candidate_limits[pass + 1];
        if (next_limit <= 0) {
          static const int kDefaults[] = {
              PEG_DEFAULT_PASS0_LIMIT, PEG_DEFAULT_PASS1_LIMIT,
              PEG_DEFAULT_PASS2_LIMIT, PEG_DEFAULT_PASS3_LIMIT,
              PEG_DEFAULT_PASS4_LIMIT,
          };
          int nd = (int)(sizeof(kDefaults) / sizeof(kDefaults[0]));
          next_limit = (pass + 1 < nd) ? kDefaults[pass + 1]
                                       : PEG_DEFAULT_PASS4_LIMIT;
        }
        cutoff_k = next_limit < limit ? next_limit : limit;
      }
      peg_cutoff_init(&cutoff_state, cutoff_k, total_unseen, limit);
      cutoff_ptr = &cutoff_state;
    }

    // Work-stealing: threads pull candidates from a shared atomic index.
    atomic_int next_candidate;
    atomic_init(&next_candidate, 0);

    PegEndgameThreadArgs *eg_targs =
        malloc_or_die(num_threads * sizeof(PegEndgameThreadArgs));
    cpthread_t *eg_threads = malloc_or_die(num_threads * sizeof(cpthread_t));

    for (int ti = 0; ti < num_threads; ti++) {
      eg_targs[ti] = (PegEndgameThreadArgs){
          .candidates = candidates,
          .next_candidate = &next_candidate,
          .num_candidates = limit,
          .endgame_solver = eg_solvers[ti],
          .endgame_results = eg_results[ti],
          .base_game = base_game,
          .mover_idx = mover_idx,
          .opp_idx = opp_idx,
          .plies = plies,
          .unseen = unseen,
          .ld_size = ld_size,
          .thread_control = args->thread_control,
          .shared_tt = shared_tt,
          .dual_lexicon_mode = args->dual_lexicon_mode,
          .solver = solver,
          .outer_args = args,
          .cutoff = cutoff_ptr,
      };
      cpthread_create(&eg_threads[ti], peg_endgame_thread, &eg_targs[ti]);
    }
    for (int ti = 0; ti < num_threads; ti++) {
      cpthread_join(eg_threads[ti]);
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

    // Sort the top-limit candidates by their new values.
    qsort(candidates, limit, sizeof(PegCandidate), compare_peg_candidates_desc);

    passes_completed++;
    num_candidates = limit;
    double now = ctimer_elapsed_seconds(&peg_timer);
    invoke_per_pass_callback(args, pass + 1, limit, candidates, num_candidates,
                             num_candidates, now, now - prev_elapsed);
    prev_elapsed = now;
  }

  // =========================================================================
  // Return the best candidate
  // =========================================================================

  result->best_move = candidates[0].move;
  result->best_win_pct = candidates[0].win_pct;
  result->best_expected_spread = candidates[0].expected_value;
  result->passes_completed = passes_completed;
  result->candidates_remaining = num_candidates;

  free(candidates);
  game_destroy(base_game);
  if (tt_is_owned) {
    transposition_table_destroy(shared_tt);
  }
}
