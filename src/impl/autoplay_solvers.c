#include "autoplay_solvers.h"

#include "../def/equity_defs.h"
#include "../def/game_defs.h"
#include "../def/game_history_defs.h"
#include "../def/move_defs.h"
#include "../def/thread_control_defs.h"
#include "../ent/autoplay_results.h"
#include "../ent/autoplay_solver_settings.h"
#include "../ent/bag.h"
#include "../ent/endgame_results.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/move.h"
#include "../ent/player.h"
#include "../ent/thread_control.h"
#include "../ent/transposition_table.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "endgame.h"
#include "move_gen.h"
#include "peg.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

enum {
  // Capacity of the list an endgame move's static equity is looked up in. An
  // endgame position has far fewer plays than this; a play not found keeps
  // its score as its equity.
  AUTOPLAY_SOLVER_EQUITY_LIST_CAP = 16384,
};

// The most threads a solve has been given since the last reset; see
// autoplay_solver_get_max_num_threads.
static atomic_int max_solve_num_threads = 0;

int autoplay_solver_get_max_num_threads(void) {
  return atomic_load(&max_solve_num_threads);
}

void autoplay_solver_reset_max_num_threads(void) {
  atomic_store(&max_solve_num_threads, 0);
}

static void autoplay_solver_note_num_threads(int num_threads) {
  int seen = atomic_load(&max_solve_num_threads);
  while (num_threads > seen &&
         !atomic_compare_exchange_weak(&max_solve_num_threads, &seen,
                                       num_threads)) {
  }
}

// Whether the run the solve belongs to was asked to stop: by the user, or by
// a contribute task's time limit.
static bool autoplay_solver_stopped(ThreadControl *run_thread_control) {
  return run_thread_control != NULL &&
         thread_control_get_status(run_thread_control) ==
             THREAD_CONTROL_STATUS_USER_INTERRUPT;
}

// The solve's own thread control, which the solver sets and reads as it
// likes, linked to the run's so a stop of the run stops the solve.
static ThreadControl *
autoplay_solver_thread_control_create(ThreadControl *run_thread_control) {
  ThreadControl *thread_control = thread_control_create();
  thread_control_set_parent(thread_control, run_thread_control);
  thread_control_set_status(thread_control, THREAD_CONTROL_STATUS_STARTED);
  return thread_control;
}

struct AutoplaySolverCtx {
  EndgameCtx *endgame_ctx;
  EndgameResults *endgame_results;
  // Scratch for looking up an endgame move's static equity; created on first
  // use, since only a recorded endgame turn needs it.
  MoveList *equity_list;
  Move move;
  SolverRankedPlay *plays;
  int plays_capacity;
  SolverAnalysis analysis;
};

AutoplaySolverCtx *autoplay_solver_ctx_create(void) {
  AutoplaySolverCtx *ctx = calloc_or_die(1, sizeof(AutoplaySolverCtx));
  ctx->endgame_ctx = endgame_ctx_create();
  ctx->endgame_results = endgame_results_create();
  return ctx;
}

void autoplay_solver_ctx_destroy(AutoplaySolverCtx *ctx) {
  if (!ctx) {
    return;
  }
  endgame_ctx_destroy(ctx->endgame_ctx);
  endgame_results_destroy(ctx->endgame_results);
  move_list_destroy(ctx->equity_list);
  free(ctx->plays);
  free(ctx);
}

bool autoplay_solver_applies(const AutoplaySolverSettings *settings,
                             const Game *game) {
  if (!autoplay_solver_settings_solves(settings)) {
    return false;
  }
  const int bag = bag_get_letters(game_get_bag(game));
  return bag == 0 || autoplay_solver_settings_runs_peg(settings, bag);
}

uint64_t autoplay_solver_seed(uint64_t game_seed, int turn_number,
                              int player_index) {
  // splitmix64 over the three, so neighbouring turns and games get unrelated
  // seeds.
  uint64_t z = game_seed + 0x9E3779B97F4A7C15ULL * (uint64_t)(turn_number + 1) +
               0xBF58476D1CE4E5B9ULL * (uint64_t)(player_index + 1);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  z ^= z >> 31;
  return z != 0 ? z : 1;
}

static void autoplay_solver_reserve_plays(AutoplaySolverCtx *ctx, int needed) {
  if (needed <= ctx->plays_capacity) {
    return;
  }
  int capacity = ctx->plays_capacity > 0 ? ctx->plays_capacity : 64;
  while (capacity < needed) {
    capacity *= 2;
  }
  ctx->plays =
      realloc_or_die(ctx->plays, sizeof(SolverRankedPlay) * (size_t)capacity);
  ctx->plays_capacity = capacity;
}

static bool moves_are_the_same_play(const Move *a, const Move *b) {
  if (move_get_type(a) != move_get_type(b)) {
    return false;
  }
  if (move_get_type(a) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
    return true;
  }
  if (move_get_row_start(a) != move_get_row_start(b) ||
      move_get_col_start(a) != move_get_col_start(b) ||
      move_get_dir(a) != move_get_dir(b) ||
      move_get_tiles_length(a) != move_get_tiles_length(b)) {
    return false;
  }
  for (int i = 0; i < move_get_tiles_length(a); i++) {
    if (move_get_tile(a, i) != move_get_tile(b, i)) {
      return false;
    }
  }
  return true;
}

// Every play the player on turn has, in ctx->equity_list, ranked by static
// equity: what a PEG solve ranks as its candidates, and where an endgame
// play's static equity is looked up.
static int autoplay_solver_generate_all(AutoplaySolverCtx *ctx, Game *game) {
  if (!ctx->equity_list) {
    ctx->equity_list = move_list_create(AUTOPLAY_SOLVER_EQUITY_LIST_CAP);
  }
  const MoveGenArgs args = {
      .game = game,
      .move_list = ctx->equity_list,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
  };
  generate_moves(&args);
  return move_list_get_count(ctx->equity_list);
}

// An endgame solve hands back a play without its static equity. Looked up in
// a full move generation for the same position, so a recorded endgame play's
// equity means what every other recorded play's does.
static void autoplay_solver_set_static_equity(AutoplaySolverCtx *ctx,
                                              Game *game, Move *move) {
  move_set_equity(move, move_get_score(move));
  if (move_get_type(move) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
    return;
  }
  const int count = autoplay_solver_generate_all(ctx, game);
  for (int i = 0; i < count; i++) {
    const Move *candidate = move_list_get_move(ctx->equity_list, i);
    if (moves_are_the_same_play(candidate, move)) {
      move_set_equity(move, move_get_equity(candidate));
      return;
    }
  }
}

// The mover's spread before this turn, in points.
static double autoplay_solver_current_spread(const Game *game) {
  const int on_turn = game_get_player_on_turn_index(game);
  return equity_to_double(player_get_score(game_get_player(game, on_turn))) -
         equity_to_double(player_get_score(game_get_player(game, 1 - on_turn)));
}

static const Move *autoplay_solver_solve_endgame(
    AutoplaySolverCtx *ctx, const AutoplaySolverSettings *settings, Game *game,
    TranspositionTable *shared_tt, int num_threads, uint64_t seed,
    ThreadControl *run_thread_control, bool record, ErrorStack *error_stack) {
  ThreadControl *thread_control =
      autoplay_solver_thread_control_create(run_thread_control);
  EndgameArgs endgame_args;
  endgame_args_fill(
      thread_control, game, /*tt_fraction_of_mem=*/0.0, settings->endgame_plies,
      DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE, num_threads,
      /*use_heuristics=*/true, /*num_top_moves=*/1,
      /*per_ply_callback=*/NULL, /*per_ply_callback_data=*/NULL,
      /*before_search_callback=*/NULL, /*before_search_callback_data=*/NULL,
      /*per_root_move_callback=*/NULL, /*per_root_move_callback_data=*/NULL,
      DUAL_LEXICON_MODE_IGNORANT, /*forced_pass_bypass=*/false,
      /*incremental_movegen=*/true, /*enable_pv_display=*/false,
      // No time limit: the depth bounds the work (see
      // AutoplaySolverSettings).
      /*soft_time_limit=*/0.0, /*hard_time_limit=*/0.0, seed,
      /*skip_word_pruning=*/false, shared_tt, /*max_workers=*/0,
      /*first_win=*/false, /*first_win_fallback_moves=*/0,
      /*use_initial_window=*/false, /*initial_alpha=*/0, /*initial_beta=*/0,
      /*external_deadline_ns=*/0, /*actual_move=*/NULL, &endgame_args);
  endgame_solve(&ctx->endgame_ctx, &endgame_args, ctx->endgame_results,
                error_stack);
  thread_control_destroy(thread_control);
  if (!error_stack_is_empty(error_stack) ||
      autoplay_solver_stopped(run_thread_control)) {
    return NULL;
  }
  const PVLine *pv_line =
      endgame_results_get_pvline(ctx->endgame_results, ENDGAME_RESULT_BEST);
  if (pv_line->num_moves == 0) {
    error_stack_push(error_stack, ERROR_STATUS_AUTOPLAY_SOLVER_NO_MOVE,
                     string_duplicate("the endgame solve chose no move"));
    return NULL;
  }
  small_move_to_move(&ctx->move, &pv_line->moves[0], game_get_board(game));
  if (record) {
    autoplay_solver_set_static_equity(ctx, game, &ctx->move);
    int depth = 0;
    int root_moves_completed = 0;
    int root_moves_total = 0;
    int ply2_completed = 0;
    int ply2_total = 0;
    endgame_ctx_get_progress(ctx->endgame_ctx, &depth, &root_moves_completed,
                             &root_moves_total, &ply2_completed, &ply2_total);
    autoplay_solver_reserve_plays(ctx, 1);
    SolverRankedPlay *play = &ctx->plays[0];
    play->move = ctx->move;
    play->has_win_percentage = false;
    play->win_percentage = 0.0;
    // The solve's value is the spread the rest of the game nets the mover, so
    // the projected final spread is that on top of the spread now.
    play->mean_spread = autoplay_solver_current_spread(game) +
                        (double)endgame_results_get_value(ctx->endgame_results,
                                                          ENDGAME_RESULT_BEST);
    play->fidelity_plies =
        endgame_results_get_depth(ctx->endgame_results, ENDGAME_RESULT_BEST);
    ctx->analysis = (SolverAnalysis){
        .type = POSITION_ANALYSIS_ENDGAME,
        .num_moves = root_moves_total > 0 ? root_moves_total : 1,
        .plays = ctx->plays,
        .num_plays = 1,
    };
  }
  return &ctx->move;
}

static void autoplay_solver_add_peg_play(AutoplaySolverCtx *ctx, int index,
                                         const PegRankedCand *cand,
                                         int fidelity) {
  SolverRankedPlay *play = &ctx->plays[index];
  play->move = cand->move;
  play->has_win_percentage = true;
  play->win_percentage = cand->win_pct * 100.0;
  play->mean_spread = cand->mean_spread;
  play->fidelity_plies = fidelity;
}

// PEG's graded ranking lists its tiers shallowest first, each best first, so
// the best-first ranking is the tiers in reverse with each kept in order. It
// holds only the plays a halving stage kept; the plays ranked at all are every
// play the position has, which is what PEG's greedy seed scores.
static void autoplay_solver_record_peg(AutoplaySolverCtx *ctx, Game *game,
                                       const PegResult *result) {
  int num_plays = 0;
  if (result->n_graded > 0) {
    autoplay_solver_reserve_plays(ctx, result->n_graded);
    int tier_end = result->n_graded;
    while (tier_end > 0) {
      const int fidelity = result->graded_fidelity[tier_end - 1];
      int tier_start = tier_end - 1;
      while (tier_start > 0 &&
             result->graded_fidelity[tier_start - 1] == fidelity) {
        tier_start--;
      }
      for (int i = tier_start; i < tier_end; i++) {
        autoplay_solver_add_peg_play(ctx, num_plays++, &result->graded_cands[i],
                                     fidelity);
      }
      tier_end = tier_start;
    }
  } else {
    autoplay_solver_reserve_plays(ctx, result->n_top_cands);
    const int fidelity =
        result->last_completed_stage >= 0 && result->n_stage_history > 0
            ? result->stage_history[result->n_stage_history - 1].fidelity_plies
            : 0;
    for (int i = 0; i < result->n_top_cands; i++) {
      autoplay_solver_add_peg_play(ctx, num_plays++, &result->top_cands[i],
                                   fidelity);
    }
  }
  const int num_moves = autoplay_solver_generate_all(ctx, game);
  ctx->analysis = (SolverAnalysis){
      .type = POSITION_ANALYSIS_PEG,
      .num_moves = num_moves > num_plays ? num_moves : num_plays,
      .plays = ctx->plays,
      .num_plays = num_plays,
  };
}

static const Move *autoplay_solver_solve_peg(
    AutoplaySolverCtx *ctx, const AutoplaySolverSettings *settings, Game *game,
    TranspositionTable *shared_tt, int num_threads,
    ThreadControl *run_thread_control, bool record, ErrorStack *error_stack) {
  ThreadControl *thread_control =
      autoplay_solver_thread_control_create(run_thread_control);
  PegArgs peg_args;
  peg_args_fill(
      game, thread_control, num_threads,
      // No time limit: the schedule bounds the work.
      /*time_budget_seconds=*/0.0, /*max_stage=*/0, /*greedy_seed_only=*/false,
      settings->peg_stage_top_k, settings->peg_num_stages, /*inner_top_k=*/0,
      settings->peg_pessimistic ? PEG_OPP_PESSIMISTIC : PEG_OPP_RATIONAL,
      settings->peg_scenario_stride, settings->peg_nested,
      /*nested_cand_cap=*/0,
      settings->peg_nested ? settings->peg_nested_cand_caps : NULL,
      settings->peg_nested ? settings->peg_nested_num_cand_caps : 0,
      /*nested_stride=*/0,
      settings->peg_nested ? settings->peg_nested_strides : NULL,
      settings->peg_nested ? AUTOPLAY_SOLVER_NUM_NESTED_STRIDES : 0,
      /*nested_emptier_ply_cap=*/0,
      settings->peg_nested ? settings->peg_nested_max_depth : 0,
      /*eval_bag_order=*/NULL, /*eval_bag_order_len=*/0, /*only_moves=*/NULL,
      /*n_only_moves=*/0, /*protect_moves=*/NULL, /*n_protect_moves=*/0,
      /*include_per_scenario=*/false, /*on_stage_start=*/NULL,
      /*on_cand_done=*/NULL, /*on_scenario_done=*/NULL, /*user_data=*/NULL,
      /*poll=*/NULL, shared_tt, &peg_args);
  PegResult result = {0};
  peg_solve(&peg_args, &result, error_stack);
  thread_control_destroy(thread_control);
  if (error_stack_is_empty(error_stack) &&
      autoplay_solver_stopped(run_thread_control)) {
    peg_result_destroy(&result);
    return NULL;
  }
  if (error_stack_is_empty(error_stack) &&
      (result.n_top_cands == 0 || result.best_win < 0.0)) {
    error_stack_push(error_stack, ERROR_STATUS_AUTOPLAY_SOLVER_NO_MOVE,
                     string_duplicate("the pre-endgame solve chose no move"));
  }
  if (!error_stack_is_empty(error_stack)) {
    peg_result_destroy(&result);
    return NULL;
  }
  ctx->move = result.best_move;
  if (record) {
    autoplay_solver_record_peg(ctx, game, &result);
  }
  peg_result_destroy(&result);
  return &ctx->move;
}

const Move *autoplay_solver_solve(AutoplaySolverCtx *ctx,
                                  const AutoplaySolverSettings *settings,
                                  Game *game, TranspositionTable *shared_tt,
                                  int num_threads, uint64_t seed,
                                  ThreadControl *run_thread_control,
                                  bool record, ErrorStack *error_stack) {
  ctx->analysis = (SolverAnalysis){0};
  if (num_threads < 1) {
    num_threads = 1;
  }
  if (autoplay_solver_stopped(run_thread_control)) {
    return NULL;
  }
  autoplay_solver_note_num_threads(num_threads);
  if (bag_get_letters(game_get_bag(game)) == 0) {
    return autoplay_solver_solve_endgame(ctx, settings, game, shared_tt,
                                         num_threads, seed, run_thread_control,
                                         record, error_stack);
  }
  return autoplay_solver_solve_peg(ctx, settings, game, shared_tt, num_threads,
                                   run_thread_control, record, error_stack);
}

const SolverAnalysis *
autoplay_solver_get_analysis(const AutoplaySolverCtx *ctx) {
  return &ctx->analysis;
}
