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
                                   int ld_size, int total_weight,
                                   double cutoff_win_pct,
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

    // Early cutoff: if even winning all remaining scenarios can't beat the
    // current best, stop evaluating this candidate.
    int remaining = total_weight - weight;
    if (remaining > 0 && (wins + remaining) / total_weight <= cutoff_win_pct) {
      break;
    }
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

static double peg_endgame_eval_candidate(EndgameSolver *endgame_solver,
                                         EndgameResults *results,
                                         const Game *base_game,
                                         const SmallMove *move, int mover_idx,
                                         int opp_idx, int plies,
                                         const uint8_t unseen[MAX_ALPHABET_SIZE],
                                         int ld_size, int total_weight,
                                         double cutoff_win_pct,
                                         ThreadControl *tc,
                                         TranspositionTable *shared_tt,
                                         dual_lexicon_mode_t dual_lexicon_mode,
                                         int thread_index_base,
                                         double *win_pct_out) {
  double total = 0.0;
  double wins = 0.0;
  int weight = 0;

  for (int t = 0; t < ld_size; t++) {
    int cnt = (int)unseen[t];
    if (cnt == 0)
      continue;
    Game *scenario =
        setup_endgame_scenario(base_game, move, mover_idx, opp_idx,
                               (MachineLetter)t, unseen, ld_size);

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
        .thread_index_base = thread_index_base,
    };

    ErrorStack *local_es = error_stack_create();
    endgame_solve(endgame_solver, &ea, results, local_es);
    error_stack_destroy(local_es);

    int endgame_val =
        endgame_results_get_value(results, ENDGAME_RESULT_BEST);
    int32_t mover_total = mover_lead - endgame_val;
    total += (double)mover_total * cnt;
    wins += ((mover_total > 0) ? 1.0 : (mover_total == 0 ? 0.5 : 0.0)) * cnt;
    weight += cnt;

    game_destroy(scenario);

    // Early cutoff: if even winning all remaining scenarios can't beat the
    // current best, stop evaluating this candidate.
    int remaining = total_weight - weight;
    if (remaining > 0 && (wins + remaining) / total_weight <= cutoff_win_pct) {
      break;
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
// Recursive pass evaluation -- per-tile worker
// ---------------------------------------------------------------------------

typedef struct PassTileResult {
  double spread;
  double win;
  int weight;
} PassTileResult;

typedef struct PassTileArgs {
  const PegArgs *outer_args;
  int opp_idx;
  const uint8_t *unseen;
  int ld_size;
  int plies;
  TranspositionTable *shared_tt;
  int tile;
  int cnt;
  int thread_index_offset;
  PassTileResult result;
} PassTileArgs;

static void *pass_tile_worker(void *arg) {
  PassTileArgs *a = (PassTileArgs *)arg;
  const PegArgs *outer_args = a->outer_args;
  int opp_idx = a->opp_idx;
  int mover_idx = 1 - opp_idx;
  int t = a->tile;
  int cnt = a->cnt;
  const LetterDistribution *ld = game_get_ld(outer_args->game);

  Game *inner_game = game_duplicate(outer_args->game);
  Rack *opp_rack = player_get_rack(game_get_player(inner_game, opp_idx));
  set_opp_rack_for_scenario(opp_rack, a->unseen, a->ld_size,
                            (MachineLetter)t);
  game_set_player_on_turn_index(inner_game, opp_idx);

  int ms = equity_to_int(
      player_get_score(game_get_player(inner_game, mover_idx)));
  int os = equity_to_int(
      player_get_score(game_get_player(inner_game, opp_idx)));
  int mp = equity_to_int(rack_get_score(
      ld, player_get_rack(game_get_player(inner_game, mover_idx))));
  int op = equity_to_int(rack_get_score(
      ld, player_get_rack(game_get_player(inner_game, opp_idx))));

  int both_pass_opp_spread = (os - op) - (ms - mp);
  double opp_pass_expected_win =
      (both_pass_opp_spread > 0)
          ? 1.0
          : (both_pass_opp_spread == 0 ? 0.5 : 0.0);
  double opp_pass_expected_spread = (double)both_pass_opp_spread;

  int inner_passes = a->plies > 0 ? a->plies - 1 : 0;
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
      .shared_tt = a->shared_tt,
      .thread_index_offset = a->thread_index_offset,
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

  {
    Bag *ibag = game_get_bag(inner_game);
    for (int ml = 0; ml < a->ld_size; ml++) {
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
      for (int ml = 0; ml < a->ld_size; ml++) {
        while (bag_get_letter(tbag, ml) > 0)
          bag_draw_letter(tbag, (MachineLetter)ml, opp_idx);
      }
      compute_unseen(tmp, opp_idx, inner_unseen);
      game_destroy(tmp);
    }

    Game *scenario = setup_endgame_scenario(
        inner_game, &inner_result.best_move, opp_idx, mover_idx,
        (MachineLetter)t, inner_unseen, a->ld_size);

    int32_t opp_lead =
        equity_to_int(
            player_get_score(game_get_player(scenario, opp_idx))) -
        equity_to_int(
            player_get_score(game_get_player(scenario, mover_idx)));

    int solve_plies = a->plies > 0 ? a->plies : 1;
    EndgameSolver *eg_solver = endgame_solver_create();
    EndgameResults *eg_results = endgame_results_create();
    EndgameArgs ea = {
        .thread_control = outer_args->thread_control,
        .game = scenario,
        .plies = solve_plies,
        .shared_tt = a->shared_tt,
        .initial_small_move_arena_size =
            DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
        .num_threads = 1,
        .use_heuristics = true,
        .num_top_moves = 1,
        .dual_lexicon_mode = outer_args->dual_lexicon_mode,
        .skip_word_pruning = true,
        .thread_index_base = a->thread_index_offset,
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
  a->result.spread = -opp_spread * cnt;
  a->result.win = mover_win * cnt;
  a->result.weight = cnt;

  game_destroy(inner_game);
  return NULL;
}

static void peg_eval_pass_recursive(const PegArgs *outer_args, int opp_idx,
                                    const uint8_t unseen[MAX_ALPHABET_SIZE],
                                    int ld_size, int plies,
                                    TranspositionTable *shared_tt,
                                    double *win_pct_out,
                                    double *expected_value_out) {
  // Count non-zero unseen tiles to determine number of parallel tasks.
  int num_tiles = 0;
  for (int t = 0; t < ld_size; t++) {
    if (unseen[t] > 0)
      num_tiles++;
  }
  if (num_tiles == 0) {
    *win_pct_out = 0.0;
    *expected_value_out = 0.0;
    return;
  }

  int base_offset =
      outer_args->thread_index_offset + outer_args->num_threads;

  PassTileArgs *tile_args = malloc_or_die(num_tiles * sizeof(PassTileArgs));
  cpthread_t *tile_threads = malloc_or_die(num_tiles * sizeof(cpthread_t));
  int ti = 0;
  for (int t = 0; t < ld_size; t++) {
    int cnt = (int)unseen[t];
    if (cnt == 0)
      continue;
    tile_args[ti] = (PassTileArgs){
        .outer_args = outer_args,
        .opp_idx = opp_idx,
        .unseen = unseen,
        .ld_size = ld_size,
        .plies = plies,
        .shared_tt = shared_tt,
        .tile = t,
        .cnt = cnt,
        .thread_index_offset = base_offset + ti,
    };
    cpthread_create(&tile_threads[ti], pass_tile_worker, &tile_args[ti]);
    ti++;
  }

  double total = 0.0;
  double wins = 0.0;
  int weight = 0;
  for (int i = 0; i < num_tiles; i++) {
    cpthread_join(tile_threads[i]);
    total += tile_args[i].result.spread;
    wins += tile_args[i].result.win;
    weight += tile_args[i].result.weight;
  }
  free(tile_args);
  free(tile_threads);

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

// Shared atomic best win percentage (stored as millipcts, i.e. win_pct * 1000).
// Threads update this as they find better candidates, enabling early cutoff
// of weaker candidates across all threads.
static void update_best_win_millipct(atomic_int *best, int new_val) {
  int old = atomic_load(best);
  while (new_val > old) {
    if (atomic_compare_exchange_weak(best, &old, new_val))
      break;
  }
}

typedef struct PegGreedyThreadArgs {
  PegCandidate *candidates;
  atomic_int *next_candidate;
  int num_candidates;
  EndgameSolverWorker *worker;
  int mover_idx;
  int opp_idx;
  const uint8_t *unseen;
  int ld_size;
  int total_weight;
  atomic_int *best_win_millipct;
  // For recursive pass evaluation.
  const PegArgs *outer_args;
} PegGreedyThreadArgs;

static void *peg_greedy_thread(void *arg) {
  PegGreedyThreadArgs *a = (PegGreedyThreadArgs *)arg;
  while (true) {
    int i = atomic_fetch_add(a->next_candidate, 1);
    if (i >= a->num_candidates)
      break;
    PegCandidate *c = &a->candidates[i];
    if (small_move_is_pass(&c->move)) {
      peg_eval_pass_recursive(a->outer_args, a->opp_idx,
                              a->unseen, a->ld_size, 0, NULL,
                              &c->win_pct, &c->expected_value);
    } else {
      // No early cutoff for greedy: evaluations are cheap and aggressive
      // cutoff can misrank candidates needed by deeper passes.
      c->expected_value = peg_greedy_eval_play(
          a->worker, &c->move, a->mover_idx, a->opp_idx,
          a->unseen, a->ld_size, a->total_weight, -1.0,
          &c->win_pct);
    }
  }
  return NULL;
}

// ---------------------------------------------------------------------------
// Thread arguments and worker function -- endgame passes
// ---------------------------------------------------------------------------

typedef struct PegEndgameThreadArgs {
  PegCandidate *candidates;
  atomic_int *next_candidate;
  int num_candidates;
  EndgameSolver *endgame_solver;
  EndgameResults *endgame_results;
  const Game *base_game;
  int mover_idx;
  int opp_idx;
  int plies;
  const uint8_t *unseen;
  int ld_size;
  int total_weight;
  atomic_int *best_win_millipct;
  ThreadControl *thread_control;
  TranspositionTable *shared_tt;
  dual_lexicon_mode_t dual_lexicon_mode;
  const PegArgs *outer_args;
  int thread_index_base;
} PegEndgameThreadArgs;

static void *peg_endgame_thread(void *arg) {
  PegEndgameThreadArgs *a = (PegEndgameThreadArgs *)arg;
  while (true) {
    int idx = atomic_fetch_add(a->next_candidate, 1);
    if (idx >= a->num_candidates)
      break;
    PegCandidate *c = &a->candidates[idx];
    double cutoff = (double)atomic_load(a->best_win_millipct) / 1000.0;
    if (small_move_is_pass(&c->move)) {
      peg_eval_pass_recursive(a->outer_args, a->opp_idx,
                              a->unseen, a->ld_size, a->plies,
                              a->shared_tt,
                              &c->win_pct, &c->expected_value);
    } else {
      c->expected_value = peg_endgame_eval_candidate(
          a->endgame_solver, a->endgame_results, a->base_game, &c->move,
          a->mover_idx, a->opp_idx, a->plies, a->unseen, a->ld_size,
          a->total_weight, cutoff,
          a->thread_control, a->shared_tt, a->dual_lexicon_mode,
          a->thread_index_base, &c->win_pct);
    }
    update_best_win_millipct(a->best_win_millipct,
                             (int)(c->win_pct * 1000.0));
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
  int ti_offset = args->thread_index_offset;

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

  // Build a base game with an empty bag.
  Game *base_game = game_duplicate(args->game);
  {
    Bag *base_bag = game_get_bag(base_game);
    for (int ml = 0; ml < ld_size; ml++) {
      while (bag_get_letter(base_bag, ml) > 0) {
        bag_draw_letter(base_bag, (MachineLetter)ml, mover_idx);
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

  Timer peg_timer;
  ctimer_start(&peg_timer);

  // Generate all candidate moves.
  MoveList *initial_ml = move_list_create_small(PEG_MOVELIST_CAPACITY);
  {
    const MoveGenArgs gen_args = {
        .game = base_game,
        .move_list = initial_ml,
        .move_record_type = MOVE_RECORD_ALL_SMALL,
        .move_sort_type = MOVE_SORT_SCORE,
        .override_kwg = NULL,
        .thread_index = ti_offset,
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
    // Move the pass candidate (if any) to index 0 so the work-stealing
    // greedy loop picks it up first. The pass candidate triggers a
    // recursive peg_solve and is much more expensive than tile moves;
    // evaluating it first lets other threads process tile moves in parallel.
    for (int i = 1; i < num_candidates; i++) {
      if (small_move_is_pass(&candidates[i].move)) {
        PegCandidate tmp = candidates[0];
        candidates[0] = candidates[i];
        candidates[i] = tmp;
        break;
      }
    }
  }
  small_move_list_destroy(initial_ml);

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

  // =========================================================================
  // Pass 0: greedy evaluation for all candidates
  // =========================================================================

  // Set up per-thread EndgameSolverWorkers for greedy evaluation.
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
        .initial_small_move_arena_size = DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
        .num_threads = 1,
        .use_heuristics = true,
        .dual_lexicon_mode = args->dual_lexicon_mode,
        .skip_word_pruning = true,
    };
    endgame_solver_reset(greedy_solvers[ti], &greedy_ea);
    greedy_workers[ti] = endgame_solver_create_worker(
        greedy_solvers[ti], ti_offset + ti, (uint64_t)ti * 54321 + 1);
  }

  atomic_int greedy_next;
  atomic_init(&greedy_next, 0);
  atomic_int greedy_best_win;
  atomic_init(&greedy_best_win, 0);
  PegGreedyThreadArgs *greedy_targs =
      malloc_or_die(num_threads * sizeof(PegGreedyThreadArgs));
  cpthread_t *greedy_threads = malloc_or_die(num_threads * sizeof(cpthread_t));

  for (int ti = 0; ti < num_threads; ti++) {
    greedy_targs[ti] = (PegGreedyThreadArgs){
        .candidates = candidates,
        .next_candidate = &greedy_next,
        .num_candidates = num_candidates,
        .worker = greedy_workers[ti],
        .mover_idx = mover_idx,
        .opp_idx = opp_idx,
        .unseen = unseen,
        .ld_size = ld_size,
        .total_weight = total_unseen,
        .best_win_millipct = &greedy_best_win,
        .outer_args = args,
    };
    cpthread_create(&greedy_threads[ti], peg_greedy_thread, &greedy_targs[ti]);
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

  double prev_elapsed = ctimer_elapsed_seconds(&peg_timer);
  invoke_per_pass_callback(args, 0, num_candidates, candidates, num_candidates,
                           args->pass_candidate_limits[0], prev_elapsed,
                           prev_elapsed);

  int passes_completed = 0;

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
    if (args->time_budget_seconds > 0.0 &&
        ctimer_elapsed_seconds(&peg_timer) >= args->time_budget_seconds) {
      break;
    }

    int plies = pass + 1;
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

    EndgameSolver **eg_solvers =
        malloc_or_die(num_threads * sizeof(EndgameSolver *));
    EndgameResults **eg_results =
        malloc_or_die(num_threads * sizeof(EndgameResults *));
    for (int ti = 0; ti < num_threads; ti++) {
      eg_solvers[ti] = endgame_solver_create();
      eg_results[ti] = endgame_results_create();
    }

    atomic_int next_candidate;
    atomic_init(&next_candidate, 0);
    atomic_int eg_best_win;
    atomic_init(&eg_best_win, 0);

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
          .total_weight = total_unseen,
          .best_win_millipct = &eg_best_win,
          .thread_control = args->thread_control,
          .shared_tt = shared_tt,
          .dual_lexicon_mode = args->dual_lexicon_mode,
          .outer_args = args,
          .thread_index_base = ti_offset + ti,
      };
      cpthread_create(&eg_threads[ti], peg_endgame_thread, &eg_targs[ti]);
    }
    for (int ti = 0; ti < num_threads; ti++) {
      cpthread_join(eg_threads[ti]);
    }

    for (int ti = 0; ti < num_threads; ti++) {
      endgame_solver_destroy(eg_solvers[ti]);
      endgame_results_destroy(eg_results[ti]);
    }
    free(eg_solvers);
    free(eg_results);
    free(eg_targs);
    free(eg_threads);

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
  kwg_destroy(pruned_kwgs[0]);
  kwg_destroy(pruned_kwgs[1]);
  game_destroy(base_game);
  if (tt_is_owned) {
    transposition_table_destroy(shared_tt);
  }
}
