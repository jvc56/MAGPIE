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
#include "../ent/transposition_table.h"
#include "../util/io_util.h"
#include "endgame.h"
#include "gameplay.h"
#include "kwg_maker.h"
#include "move_gen.h"
#include "word_prune.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

enum {
  PEG_MOVELIST_CAPACITY = 250000,
};

struct PegSolver {
  int _unused;
};

// Returns true if the PEG time budget has been exhausted.
// When triggered, sets USER_INTERRUPT on the ThreadControl so that any
// in-flight endgame solves and greedy playouts bail out promptly.
static bool peg_budget_exhausted(const Timer *peg_timer,
                                 double time_budget_seconds,
                                 ThreadControl *tc) {
  if (time_budget_seconds <= 0.0)
    return false;
  if (ctimer_elapsed_seconds(peg_timer) < time_budget_seconds)
    return false;
  if (tc) {
    thread_control_set_status(tc, THREAD_CONTROL_STATUS_USER_INTERRUPT);
  }
  return true;
}

// ---------------------------------------------------------------------------
// Candidate tracking
// ---------------------------------------------------------------------------

typedef struct PegCandidate {
  SmallMove move;
  double win_pct;
  double expected_value;
} PegCandidate;

static int compare_peg_candidates_desc(const void *a, const void *b) {
  const PegCandidate *ca = (const PegCandidate *)a;
  const PegCandidate *cb = (const PegCandidate *)b;
  if (cb->win_pct > ca->win_pct + 1e-9)
    return 1;
  if (ca->win_pct > cb->win_pct + 1e-9)
    return -1;
  if (cb->expected_value > ca->expected_value)
    return 1;
  if (cb->expected_value < ca->expected_value)
    return -1;
  return 0;
}

// Sort by static score descending (for skip_greedy mode).
static int compare_peg_candidates_score_desc(const void *a, const void *b) {
  const PegCandidate *ca = (const PegCandidate *)a;
  const PegCandidate *cb = (const PegCandidate *)b;
  int sa = (int)small_move_get_score(&ca->move);
  int sb = (int)small_move_get_score(&cb->move);
  return sb - sa;
}

// ---------------------------------------------------------------------------
// Unseen tile computation
// ---------------------------------------------------------------------------

static int compute_unseen(const Game *game, int mover_idx,
                          uint8_t unseen[MAX_ALPHABET_SIZE]) {
  const LetterDistribution *ld = game_get_ld(game);
  int ld_size = ld_get_size(ld);

  for (int ml = 0; ml < ld_size; ml++) {
    unseen[ml] = (uint8_t)ld_get_dist(ld, ml);
  }

  const Rack *mover_rack =
      player_get_rack(game_get_player(game, mover_idx));
  for (int ml = 0; ml < ld_size; ml++) {
    unseen[ml] -= (uint8_t)rack_get_letter(mover_rack, ml);
  }

  const Board *board = game_get_board(game);
  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      if (board_is_empty(board, row, col))
        continue;
      MachineLetter ml = board_get_letter(board, row, col);
      if (get_is_blanked(ml)) {
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
// Greedy evaluation of a play candidate
// ---------------------------------------------------------------------------

// Evaluates a play move by enumerating all possible bag tiles. For each tile T:
//   - plays the candidate (onto a game with empty bag)
//   - updates cross-sets ONCE after candidate placement
//   - adds T to the mover's rack, sets opp rack to unseen-T
//   - runs negamax_greedy_leaf_playout (which handles conservation heuristic)
//   - undoes everything
// The cross-set update after the candidate is done once and reused
// across all ~8 scenarios (negamax_greedy_leaf_playout handles its own
// play/unplay internally).
static double peg_greedy_eval_play(EndgameSolverWorker *worker,
                                   const SmallMove *move, int mover_idx,
                                   int opp_idx,
                                   const uint8_t unseen[MAX_ALPHABET_SIZE],
                                   int ld_size,
                                   double *win_pct_out) {
  Game *game_copy = endgame_solver_worker_get_game(worker);
  Board *board = game_get_board(game_copy);

  // Play candidate move with undo tracking (for later unplay).
  Move m;
  small_move_to_move(&m, move, board);
  MoveUndo candidate_undo;
  move_undo_reset(&candidate_undo);
  play_move_incremental(&m, game_copy, &candidate_undo);

  // Handle false game-end from bingo (empty bag triggers end prematurely).
  if (game_get_game_end_reason(game_copy) == GAME_END_REASON_STANDARD) {
    Rack *opp_rack = player_get_rack(game_get_player(game_copy, opp_idx));
    Equity end_pts =
        calculate_end_rack_points(opp_rack, game_get_ld(game_copy));
    player_add_to_score(game_get_player(game_copy, mover_idx), -end_pts);
    game_set_game_end_reason(game_copy, GAME_END_REASON_NONE);
  }

  // Update cross-sets ONCE after candidate placement.
  // This is shared across all ~8 bag-tile scenarios.
  if (!board_get_cross_sets_valid(board)) {
    if (candidate_undo.move_tiles_length > 0) {
      update_cross_set_for_move_from_undo(&candidate_undo, game_copy);
    }
    board_set_cross_sets_valid(board, true);
  }

  // Temporarily set requested_plies to 1 so negamax_greedy_leaf_playout
  // uses undo slots 1+ and does not clobber slot 0 (the candidate undo).
  int saved_plies = endgame_solver_worker_get_requested_plies(worker);
  endgame_solver_worker_set_requested_plies(worker, 1);

  double total = 0.0;
  double wins = 0.0;
  int weight = 0;
  PVLine pv;

  Rack *mover_rack =
      player_get_rack(game_get_player(game_copy, mover_idx));
  Rack *opp_rack = player_get_rack(game_get_player(game_copy, opp_idx));

  for (int t = 0; t < ld_size; t++) {
    int cnt = (int)unseen[t];
    if (cnt == 0)
      continue;

    // Set up scenario: mover draws tile t, opp gets the rest.
    rack_add_letter(mover_rack, (MachineLetter)t);
    Rack saved_opp;
    rack_copy(&saved_opp, opp_rack);
    set_opp_rack_for_scenario(opp_rack, unseen, ld_size, (MachineLetter)t);

    // Greedy playout from opp's turn. Returns spread from opp's perspective;
    // negate for mover.
    int32_t mover_spread = -negamax_greedy_leaf_playout(worker, 0, opp_idx, 0,
                                                        &pv, 0.0f);

    total += (double)mover_spread * cnt;
    wins +=
        ((mover_spread > 0) ? 1.0 : (mover_spread == 0 ? 0.5 : 0.0)) * cnt;
    weight += cnt;

    rack_take_letter(mover_rack, (MachineLetter)t);
    rack_copy(opp_rack, &saved_opp);
  }

  endgame_solver_worker_set_requested_plies(worker, saved_plies);

  // Unplay candidate move.
  unplay_move_incremental(game_copy, &candidate_undo);
  if (candidate_undo.move_tiles_length > 0) {
    update_cross_sets_after_unplay_from_undo(&candidate_undo, game_copy);
    board_set_cross_sets_valid(board, true);
  }

  if (weight == 0) {
    *win_pct_out = 0.0;
    return 0.0;
  }
  *win_pct_out = wins / weight;
  return total / weight;
}

// ---------------------------------------------------------------------------
// Endgame evaluation of a candidate across all bag-tile scenarios
// ---------------------------------------------------------------------------

static Game *setup_endgame_scenario(const Game *base_game,
                                    const SmallMove *move, int mover_idx,
                                    int opp_idx, MachineLetter bag_tile,
                                    const uint8_t unseen[MAX_ALPHABET_SIZE],
                                    int ld_size) {
  Game *g = game_duplicate(base_game);
  game_set_endgame_solving_mode(g);
  game_set_backup_mode(g, BACKUP_MODE_OFF);

  Move m;
  small_move_to_move(&m, move, game_get_board(g));
  play_move_without_drawing_tiles(&m, g);

  if (game_get_game_end_reason(g) == GAME_END_REASON_STANDARD) {
    Equity bonus = calculate_end_rack_points(
        player_get_rack(game_get_player(g, opp_idx)), game_get_ld(g));
    player_add_to_score(game_get_player(g, mover_idx), -bonus);
    game_set_game_end_reason(g, GAME_END_REASON_NONE);
  }

  Rack *opp_rack = player_get_rack(game_get_player(g, opp_idx));
  set_opp_rack_for_scenario(opp_rack, unseen, ld_size, bag_tile);

  Rack *mover_rack = player_get_rack(game_get_player(g, mover_idx));
  rack_add_letter(mover_rack, bag_tile);

  return g;
}

// peg_endgame_eval_candidate removed: replaced by decomposed parallel
// evaluation via peg_decomp_thread.

// ---------------------------------------------------------------------------
// Recursive pass evaluation
// ---------------------------------------------------------------------------

static void peg_eval_pass_recursive(const PegArgs *outer_args,
                                    int opp_idx,
                                    const uint8_t unseen[MAX_ALPHABET_SIZE],
                                    int ld_size, int plies,
                                    TranspositionTable *shared_tt,
                                    double *win_pct_out,
                                    double *expected_value_out) {
  int mover_idx = 1 - opp_idx;
  const LetterDistribution *ld = game_get_ld(outer_args->game);
  double total = 0.0;
  double wins = 0.0;
  int weight = 0;

  for (int t = 0; t < ld_size; t++) {
    int cnt = (int)unseen[t];
    if (cnt == 0)
      continue;

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

    // Option A: opp passes too -> game ends, both lose rack tile values.
    int both_pass_opp_spread = (os - op) - (ms - mp);
    double opp_pass_expected_win =
        (both_pass_opp_spread > 0)
            ? 1.0
            : (both_pass_opp_spread == 0 ? 0.5 : 0.0);
    double opp_pass_expected_spread = (double)both_pass_opp_spread;

    // Option B: opp plays a tile move, chosen via inner peg_solve.
    int inner_passes = plies > 0 ? plies - 1 : 0;
    PegSolver *inner_solver = peg_solver_create();
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
    for (int i = 0; i < PEG_MAX_PASSES; i++)
      inner_args.pass_candidate_limits[i] =
          outer_args->pass_candidate_limits[i];

    PegResult inner_result;
    ErrorStack *inner_es = error_stack_create();
    peg_solve(inner_solver, &inner_args, &inner_result, inner_es);

    bool found_tile_play =
        error_stack_is_empty(inner_es) &&
        !small_move_is_pass(&inner_result.best_move);
    double opp_play_expected_win = inner_result.best_win_pct;
    double opp_play_expected_spread = inner_result.best_expected_spread;

    error_stack_destroy(inner_es);
    peg_solver_destroy(inner_solver);

    // Drain the bag from inner_game.
    {
      Bag *ibag = game_get_bag(inner_game);
      for (int ml = 0; ml < ld_size; ml++) {
        while (bag_get_letter(ibag, ml) > 0)
          bag_draw_letter(ibag, (MachineLetter)ml, opp_idx);
      }
    }

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
          (MachineLetter)t, inner_unseen, ld_size);

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
    return;
  }
  *win_pct_out = wins / weight;
  *expected_value_out = total / weight;
}

// ---------------------------------------------------------------------------
// Thread arguments and worker functions -- greedy pass
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
  const Timer *peg_timer;
  double time_budget_seconds;
  ThreadControl *thread_control;
  // For recursive pass evaluation.
  const PegArgs *outer_args;
} PegGreedyThreadArgs;

static void *peg_greedy_thread(void *arg) {
  PegGreedyThreadArgs *a = (PegGreedyThreadArgs *)arg;
  for (int i = a->start; i < a->end; i++) {
    if (peg_budget_exhausted(a->peg_timer, a->time_budget_seconds,
                             a->thread_control))
      break;
    PegCandidate *c = &a->candidates[i];
    if (small_move_is_pass(&c->move)) {
      peg_eval_pass_recursive(a->outer_args, a->opp_idx,
                              a->unseen, a->ld_size, 0, NULL,
                              &c->win_pct, &c->expected_value);
    } else {
      c->expected_value = peg_greedy_eval_play(
          a->worker, &c->move, a->mover_idx, a->opp_idx,
          a->unseen, a->ld_size, &c->win_pct);
    }
  }
  return NULL;
}

// ---------------------------------------------------------------------------
// Decomposed parallel endgame evaluation (scenario-level work stealing)
// ---------------------------------------------------------------------------

// Per-candidate state for parallel aggregation.
typedef struct PegCandAccum {
  atomic_int wins_x2;     // wins*2 + ties*1 (avoids float atomics)
  atomic_int spread_sum;  // weighted spread sum (integer)
  atomic_int weight_done; // scenarios completed so far
  int total_weight;       // total weight (sum of tile counts)
  Game *post_cand_game;   // post-candidate game (play + cross-sets), shared
  atomic_int prepared;    // 0=not started, 1=in progress, 2=done
} PegCandAccum;

// Flat work item: one (candidate, tile_type) pair.
typedef struct PegWorkItem {
  int cand_idx;
  MachineLetter tile;
  int tile_count;
} PegWorkItem;

typedef struct PegDecompThreadArgs {
  PegWorkItem *work_items;
  atomic_int *next_item;
  int total_items;
  PegCandAccum *accums;
  PegCandidate *candidates;
  const Game *base_game;
  int mover_idx;
  int opp_idx;
  int plies;
  int thread_index; // unique per thread, for movegen cache isolation
  const uint8_t *unseen;
  int ld_size;
  EndgameSolver *endgame_solver;
  EndgameResults *endgame_results;
  ThreadControl *thread_control;
  TranspositionTable *shared_tt;
  dual_lexicon_mode_t dual_lexicon_mode;
  const Timer *peg_timer;
  double time_budget_seconds;
} PegDecompThreadArgs;

static void *peg_decomp_thread(void *arg) {
  PegDecompThreadArgs *a = (PegDecompThreadArgs *)arg;

  while (true) {
    if (peg_budget_exhausted(a->peg_timer, a->time_budget_seconds,
                             a->thread_control))
      break;
    int idx = atomic_fetch_add(a->next_item, 1);
    if (idx >= a->total_items)
      break;

    PegWorkItem *item = &a->work_items[idx];
    int ci = item->cand_idx;
    PegCandAccum *acc = &a->accums[ci];

    // Lazy-prepare the post-candidate game (first thread to touch wins).
    int expected = 0;
    if (atomic_compare_exchange_strong(&acc->prepared, &expected, 1)) {
      // We won: play candidate, update cross-sets.
      Game *g = game_duplicate(a->base_game);
      game_set_endgame_solving_mode(g);
      game_set_backup_mode(g, BACKUP_MODE_OFF);
      Move m;
      small_move_to_move(&m, &a->candidates[ci].move, game_get_board(g));
      play_move_without_drawing_tiles(&m, g);
      if (game_get_game_end_reason(g) != GAME_END_REASON_NONE) {
        Equity bonus = calculate_end_rack_points(
            player_get_rack(game_get_player(g, a->opp_idx)), game_get_ld(g));
        player_add_to_score(game_get_player(g, a->mover_idx), -bonus);
        game_set_game_end_reason(g, GAME_END_REASON_NONE);
      }
      acc->post_cand_game = g;
      atomic_store(&acc->prepared, 2);
    } else {
      // Wait for preparation to complete (or budget exhaustion).
      while (atomic_load(&acc->prepared) != 2) {
        if (peg_budget_exhausted(a->peg_timer, a->time_budget_seconds,
                                 a->thread_control))
          break;
      }
      if (atomic_load(&acc->prepared) != 2)
        break; // Budget exhausted before prep finished.
    }

    // Duplicate the post-candidate game and set racks for this scenario.
    Game *scenario = game_duplicate(acc->post_cand_game);
    Rack *opp_rack = player_get_rack(game_get_player(scenario, a->opp_idx));
    set_opp_rack_for_scenario(opp_rack, a->unseen, a->ld_size, item->tile);
    Rack *mover_rack =
        player_get_rack(game_get_player(scenario, a->mover_idx));
    rack_add_letter(mover_rack, item->tile);

    int32_t mover_lead =
        equity_to_int(
            player_get_score(game_get_player(scenario, a->mover_idx))) -
        equity_to_int(
            player_get_score(game_get_player(scenario, a->opp_idx)));

    EndgameArgs ea = {
        .thread_control = a->thread_control,
        .game = scenario,
        .plies = a->plies,
        .shared_tt = a->shared_tt,
        .initial_small_move_arena_size = DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
        .num_threads = 1,
        .use_heuristics = true,
        .num_top_moves = 1,
        .dual_lexicon_mode = a->dual_lexicon_mode,
        .skip_word_pruning = true,
        .thread_index_offset = a->thread_index,
    };

    ErrorStack *local_es = error_stack_create();
    endgame_solve(a->endgame_solver, &ea, a->endgame_results, local_es);
    error_stack_destroy(local_es);

    int eg_val =
        endgame_results_get_value(a->endgame_results, ENDGAME_RESULT_BEST);
    int32_t mover_total = mover_lead - eg_val;

    // Accumulate into per-candidate atomics.
    int win_contrib =
        (mover_total > 0) ? 2 * item->tile_count
                          : (mover_total == 0 ? item->tile_count : 0);
    atomic_fetch_add(&acc->wins_x2, win_contrib);
    atomic_fetch_add(&acc->spread_sum, (int)(mover_total * item->tile_count));
    atomic_fetch_add(&acc->weight_done, item->tile_count);

    game_destroy(scenario);
  }
  return NULL;
}

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
  for (int i = 0; i < top; i++) {
    moves[i] = sorted[i].move;
    values[i] = sorted[i].expected_value;
    win_pcts[i] = sorted[i].win_pct;
  }
  args->per_pass_callback(pass, num_evaluated, moves, values, win_pcts, top,
                          args->game, elapsed, stage_seconds,
                          args->per_pass_callback_data);
}

// ---------------------------------------------------------------------------
// Main solver
// ---------------------------------------------------------------------------

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

  // Compute unseen tiles.
  uint8_t unseen[MAX_ALPHABET_SIZE];
  int total_unseen = compute_unseen(args->game, mover_idx, unseen);

  if (total_unseen < 1) {
    error_stack_push(error_stack, ERROR_STATUS_ENDGAME_BAG_NOT_EMPTY,
                     get_formatted_string(
                         "peg_solve requires at least 1 unseen tile, "
                         "but found %d",
                         total_unseen));
    return;
  }
  if (total_unseen > RACK_SIZE + 1) {
    error_stack_push(error_stack, ERROR_STATUS_ENDGAME_BAG_NOT_EMPTY,
                     get_formatted_string(
                         "peg_solve: %d unseen tiles would give opponent %d "
                         "tiles, exceeding RACK_SIZE=%d",
                         total_unseen, total_unseen - 1, RACK_SIZE));
    return;
  }

  // Build a base game with an empty bag by drawing each tile and then
  // returning it from the rack (net effect: bag empty, rack unchanged).
  Game *base_game = game_duplicate(args->game);
  {
    Bag *base_bag = game_get_bag(base_game);
    Rack *mover_rack = player_get_rack(game_get_player(base_game, mover_idx));
    for (int ml = 0; ml < ld_size; ml++) {
      while (bag_get_letter(base_bag, ml) > 0) {
        bag_draw_letter(base_bag, (MachineLetter)ml, mover_idx);
        rack_take_letter(mover_rack, (MachineLetter)ml);
      }
    }
  }

  // Build pruned KWGs once at the PEG root position.
  // These will be reused for all greedy playouts and passed via
  // skip_word_pruning to endgame_solve so it doesn't rebuild them.
  bool shared_kwg =
      game_get_data_is_shared(args->game, PLAYERS_DATA_TYPE_KWG);
  dual_lexicon_mode_t dlm = args->dual_lexicon_mode;
  if (dlm == DUAL_LEXICON_MODE_INFORMED && shared_kwg) {
    dlm = DUAL_LEXICON_MODE_IGNORANT;
  }
  bool create_separate_kwgs =
      (dlm == DUAL_LEXICON_MODE_INFORMED) && !shared_kwg;

  Timer peg_timer;
  ctimer_start(&peg_timer);

  KWG *pruned_kwgs[2] = {NULL, NULL};
  for (int player_idx = 0; player_idx < (create_separate_kwgs ? 2 : 1);
       player_idx++) {
    const KWG *full_kwg =
        player_get_kwg(game_get_player(base_game, player_idx));
    DictionaryWordList *word_list = dictionary_word_list_create();
    generate_possible_words(base_game, full_kwg, word_list);
    pruned_kwgs[player_idx] = make_kwg_from_words_small(
        word_list, KWG_MAKER_OUTPUT_GADDAG, KWG_MAKER_MERGE_EXACT);
    dictionary_word_list_destroy(word_list);
  }

  // Set override KWGs on base_game so all duplicates inherit them.
  game_set_override_kwgs(base_game, pruned_kwgs[0], pruned_kwgs[1], dlm);
  game_gen_all_cross_sets(base_game);

  // Generate all candidate moves.
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
    kwg_destroy(pruned_kwgs[0]);
    kwg_destroy(pruned_kwgs[1]);
    game_destroy(base_game);
    error_stack_push(error_stack, ERROR_STATUS_ENDGAME_BAG_NOT_EMPTY,
                     get_formatted_string("peg_solve: no legal moves found"));
    return;
  }

  PegCandidate *candidates =
      malloc_or_die(num_candidates * sizeof(PegCandidate));
  {
    int j = 0;
    for (int i = 0; i < num_candidates; i++) {
      if (args->skip_pass && small_move_is_pass(initial_ml->small_moves[i]))
        continue;
      candidates[j].move = *initial_ml->small_moves[i];
      candidates[j].expected_value = 0.0;
      candidates[j].win_pct = 0.0;
      j++;
    }
    num_candidates = j;
  }
  small_move_list_destroy(initial_ml);

  // Sort candidates by score descending so the highest-scoring moves are
  // evaluated first. This matters especially for skip_greedy mode where
  // the time budget may only allow evaluating a few candidates.
  qsort(candidates, num_candidates, sizeof(PegCandidate),
        compare_peg_candidates_score_desc);

  if (num_candidates == 0) {
    free(candidates);
    kwg_destroy(pruned_kwgs[0]);
    kwg_destroy(pruned_kwgs[1]);
    game_destroy(base_game);
    error_stack_push(error_stack, ERROR_STATUS_ENDGAME_BAG_NOT_EMPTY,
                     get_formatted_string("peg_solve: no legal moves after "
                                          "filtering"));
    return;
  }

  double prev_elapsed = 0.0;
  int passes_completed = 0;
  int total_evaluated = 0;

  // =========================================================================
  // Pass 0: greedy evaluation for all candidates (unless skip_greedy)
  // =========================================================================

  if (!args->skip_greedy) {
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
          .tt_fraction_of_mem = 0,
          .initial_small_move_arena_size =
              DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
          .num_threads = 1,
          .use_heuristics = true,
          .dual_lexicon_mode = args->dual_lexicon_mode,
          .skip_word_pruning = true,
      };
      endgame_solver_reset(greedy_solvers[ti], &greedy_ea);
      greedy_workers[ti] = endgame_solver_create_worker(
          greedy_solvers[ti], ti, (uint64_t)ti * 54321 + 1);
    }

    int chunk = (num_candidates + num_threads - 1) / num_threads;
    PegGreedyThreadArgs *greedy_targs =
        malloc_or_die(num_threads * sizeof(PegGreedyThreadArgs));
    cpthread_t *greedy_threads =
        malloc_or_die(num_threads * sizeof(cpthread_t));

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
          .peg_timer = &peg_timer,
          .time_budget_seconds = args->time_budget_seconds,
          .thread_control = args->thread_control,
          .outer_args = args,
      };
      cpthread_create(&greedy_threads[ti], peg_greedy_thread,
                      &greedy_targs[ti]);
    }
    for (int ti = 0; ti < num_threads; ti++) {
      cpthread_join(greedy_threads[ti]);
    }

    for (int ti = 0; ti < num_threads; ti++) {
      endgame_solver_worker_destroy(greedy_workers[ti]);
      endgame_solver_destroy(greedy_solvers[ti]);
    }
    free(greedy_workers);
    free(greedy_solvers);
    free(greedy_targs);
    free(greedy_threads);

    qsort(candidates, num_candidates, sizeof(PegCandidate),
          compare_peg_candidates_desc);

    prev_elapsed = ctimer_elapsed_seconds(&peg_timer);
    invoke_per_pass_callback(args, 0, num_candidates, candidates,
                             num_candidates, args->pass_candidate_limits[0],
                             prev_elapsed, prev_elapsed);
  }
  // When skip_greedy, candidates stay in static score order (from movegen).

  // =========================================================================
  // Passes 1..num_passes: progressively deeper endgame search on top-K
  // =========================================================================

  bool tt_is_owned = false;
  TranspositionTable *shared_tt = args->shared_tt;
  if (!shared_tt && args->num_passes > 0 && args->tt_fraction_of_mem > 0) {
    shared_tt = transposition_table_create(args->tt_fraction_of_mem);
    tt_is_owned = true;
  }

  for (int pass = 0; pass < args->num_passes; pass++) {
    if (peg_budget_exhausted(&peg_timer, args->time_budget_seconds,
                             args->thread_control)) {
      break;
    }

    int plies = args->skip_greedy ? pass + 2 : pass + 1;
    int limit = args->pass_candidate_limits[pass];
    if (limit <= 0) {
      static const int kDefaultLimits[] = {
          PEG_DEFAULT_PASS0_LIMIT, PEG_DEFAULT_PASS1_LIMIT,
          PEG_DEFAULT_PASS2_LIMIT, PEG_DEFAULT_PASS3_LIMIT,
          PEG_DEFAULT_PASS4_LIMIT,
      };
      int ndefaults = (int)(sizeof(kDefaultLimits) / sizeof(kDefaultLimits[0]));
      limit =
          (pass < ndefaults) ? kDefaultLimits[pass] : PEG_DEFAULT_PASS4_LIMIT;
    }
    if (limit > num_candidates)
      limit = num_candidates;

    // Build flat work queue: (candidate, tile_type) pairs for all candidates.
    // Distinct tile types from the unseen array.
    int num_tile_types = 0;
    MachineLetter tile_types[MAX_ALPHABET_SIZE];
    int tile_counts[MAX_ALPHABET_SIZE];
    for (int ml = 0; ml < ld_size; ml++) {
      if (unseen[ml] > 0) {
        tile_types[num_tile_types] = (MachineLetter)ml;
        tile_counts[num_tile_types] = (int)unseen[ml];
        num_tile_types++;
      }
    }

    int total_items = limit * num_tile_types;
    PegWorkItem *work_items =
        malloc_or_die(total_items * sizeof(PegWorkItem));
    PegCandAccum *accums =
        calloc_or_die(limit, sizeof(PegCandAccum));

    for (int ci = 0; ci < limit; ci++) {
      int tw = 0;
      for (int ti = 0; ti < num_tile_types; ti++) {
        int idx = ci * num_tile_types + ti;
        work_items[idx].cand_idx = ci;
        work_items[idx].tile = tile_types[ti];
        work_items[idx].tile_count = tile_counts[ti];
        tw += tile_counts[ti];
      }
      accums[ci].total_weight = tw;
    }

    // Per-thread endgame solvers.
    EndgameSolver **eg_solvers =
        malloc_or_die(num_threads * sizeof(EndgameSolver *));
    EndgameResults **eg_results =
        malloc_or_die(num_threads * sizeof(EndgameResults *));
    for (int ti = 0; ti < num_threads; ti++) {
      eg_solvers[ti] = endgame_solver_create();
      eg_results[ti] = endgame_results_create();
    }

    atomic_int next_item;
    atomic_init(&next_item, 0);

    PegDecompThreadArgs *targs =
        malloc_or_die(num_threads * sizeof(PegDecompThreadArgs));
    cpthread_t *threads = malloc_or_die(num_threads * sizeof(cpthread_t));

    for (int ti = 0; ti < num_threads; ti++) {
      targs[ti] = (PegDecompThreadArgs){
          .work_items = work_items,
          .next_item = &next_item,
          .total_items = total_items,
          .accums = accums,
          .candidates = candidates,
          .base_game = base_game,
          .mover_idx = mover_idx,
          .opp_idx = opp_idx,
          .plies = plies,
          .thread_index = ti,
          .unseen = unseen,
          .ld_size = ld_size,
          .endgame_solver = eg_solvers[ti],
          .endgame_results = eg_results[ti],
          .thread_control = args->thread_control,
          .shared_tt = shared_tt,
          .dual_lexicon_mode = args->dual_lexicon_mode,
          .peg_timer = &peg_timer,
          .time_budget_seconds = args->time_budget_seconds,
      };
      cpthread_create(&threads[ti], peg_decomp_thread, &targs[ti]);
    }
    for (int ti = 0; ti < num_threads; ti++) {
      cpthread_join(threads[ti]);
    }

    // Harvest results from per-candidate accumulators.
    int evaluated_this_pass = 0;
    for (int ci = 0; ci < limit; ci++) {
      int w = atomic_load(&accums[ci].weight_done);
      if (w == 0)
        continue; // not touched (budget exhausted before reaching this cand)
      evaluated_this_pass++;
      double total_w = (double)accums[ci].total_weight;
      candidates[ci].win_pct =
          (double)atomic_load(&accums[ci].wins_x2) / (2.0 * total_w);
      candidates[ci].expected_value =
          (double)atomic_load(&accums[ci].spread_sum) / total_w;
      // Clean up post-candidate game.
      if (accums[ci].post_cand_game) {
        game_destroy(accums[ci].post_cand_game);
      }
    }
    total_evaluated += evaluated_this_pass;

    for (int ti = 0; ti < num_threads; ti++) {
      endgame_solver_destroy(eg_solvers[ti]);
      endgame_results_destroy(eg_results[ti]);
    }
    free(eg_solvers);
    free(eg_results);
    free(targs);
    free(threads);
    free(work_items);
    free(accums);

    qsort(candidates, evaluated_this_pass, sizeof(PegCandidate),
          compare_peg_candidates_desc);

    passes_completed++;
    num_candidates = evaluated_this_pass;
    double now = ctimer_elapsed_seconds(&peg_timer);
    invoke_per_pass_callback(args, pass + 1, evaluated_this_pass, candidates,
                             num_candidates, num_candidates, now,
                             now - prev_elapsed);
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
  result->candidates_evaluated = total_evaluated;

  free(candidates);
  kwg_destroy(pruned_kwgs[0]);
  kwg_destroy(pruned_kwgs[1]);
  game_destroy(base_game);
  if (tt_is_owned) {
    transposition_table_destroy(shared_tt);
  }
}
