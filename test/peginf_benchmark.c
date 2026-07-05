// A/B benchmark for pre-endgame (PEG) inference. The end goal: from PEG
// positions, play the game out with Player A (uniform PEG) vs Player B
// (inference-weighted PEG), equal per-turn budget, and tally the win%/spread
// difference, logging per-turn wall time to confirm neither arm overruns.
//
// This is built incrementally. THIS increment lands the playout core: from a
// position, choose and play each move -- PEG (greedy seed) while the bag holds
// [PEG_MIN_BAG, PEG_MAX_BAG] tiles, a static best otherwise (a placeholder for
// the d25 time-limited endgame) -- until the game ends, logging per-turn time
// against the budget. The inference variant (Player B), position generation
// (static -> sim -> <=4, and PEG -> PEG), and the A/B aggregation build on top.

#include "../src/def/equity_defs.h"
#include "../src/def/move_defs.h"
#include "../src/def/peg_defs.h"
#include "../src/def/thread_control_defs.h"
#include "../src/compat/ctime.h"
#include "../src/ent/bag.h"
#include "../src/ent/endgame_results.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/thread_control.h"
#include "../src/impl/config.h"
#include "../src/impl/endgame.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"
#include "../src/impl/peg.h"
#include "../src/util/io_util.h"
#include "test_util.h"
#include <assert.h>
#include <stdio.h>
#include <time.h>

// A real 4-in-bag CSW24 pre-endgame: player 0 (ACEINOP) on turn, bag = 4.
#define PEG_BENCH_4BAG_CGP                                                    \
  "cgp 3V3W6L/1BEATY1U5GI/2XU3S4FEN/3TA2H4LOY/2GEN1DUCAT1AD1/2O1I1I2WRITE1/"  \
  "2V1M1ZOAEA4/3JAGER2DRILL/2BOtONE5O1/1FERER7Q1/4S8U1/12NaM/12ATE/13ST/14H " \
  "ACEINOP/DEIINOS 361/397 0 -lex CSW24"

#define PEG_BENCH_TURN_BUDGET_S 3.0
#define PEG_BENCH_MAX_CANDIDATES 20
#define PEG_BENCH_ENDGAME_PLIES 25

static double peg_bench_now_s(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

// Solve and play the endgame (bag empty), capped to the same per-turn budget as
// the pre-endgame turns: a depth-25 solve with soft/hard IDS limits and a hard
// wall-clock deadline all set to budget_s. Returns true if a move was played.
static bool peg_bench_play_endgame(Game *game, ThreadControl *thread_control,
                                   double budget_s, ErrorStack *error_stack) {
  EndgameArgs endgame_args = {0};
  endgame_args.thread_control = thread_control;
  endgame_args.game = game;
  endgame_args.plies = PEG_BENCH_ENDGAME_PLIES;
  endgame_args.tt_fraction_of_mem = 0.05;
  endgame_args.initial_small_move_arena_size =
      DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE;
  endgame_args.num_threads = 1;
  endgame_args.use_heuristics = true;
  endgame_args.forced_pass_bypass = true;
  endgame_args.enable_pv_display = true;
  endgame_args.num_top_moves = 1;
  endgame_args.seed = 0;
  // Same time budget as a pre-endgame turn: stop IDS at the soft limit, don't
  // start a depth that would blow the hard limit, and bail mid-search at the
  // wall-clock deadline.
  endgame_args.soft_time_limit = budget_s;
  endgame_args.hard_time_limit = budget_s;
  endgame_args.external_deadline_ns =
      ctimer_monotonic_ns() + (int64_t)(budget_s * 1.0e9);

  EndgameCtx *ctx = endgame_ctx_create();
  EndgameResults *results = endgame_results_create();
  thread_control_set_status(thread_control, THREAD_CONTROL_STATUS_STARTED);
  endgame_solve(&ctx, &endgame_args, results, error_stack);

  bool played = false;
  if (error_stack_is_empty(error_stack)) {
    const PVLine *pv = endgame_results_get_pvline(results, ENDGAME_RESULT_BEST);
    if (pv != NULL && pv->num_moves > 0) {
      Move move;
      small_move_to_move(&move, &pv->moves[0], game_get_board(game));
      play_move(&move, game, NULL);
      played = true;
    }
  }
  endgame_results_destroy(results);
  endgame_ctx_destroy(ctx);
  return played;
}

// Choose and play one move for the player on turn. While the bag is in the PEG
// range, solve it with the greedy pre-endgame seed within budget_s; at bag 0
// solve the endgame under the same budget. *elapsed_out is the wall time spent.
static void peg_bench_play_turn(Game *game, MoveList *move_list,
                                ThreadControl *thread_control, double budget_s,
                                double *elapsed_out, ErrorStack *error_stack) {
  const double t0 = peg_bench_now_s();
  const int bag = bag_get_letters(game_get_bag(game));
  bool played = false;
  if (bag == 0) {
    played = peg_bench_play_endgame(game, thread_control, budget_s, error_stack);
  } else if (bag >= PEG_MIN_BAG && bag <= PEG_MAX_BAG) {
    // Supply the candidate field explicitly: peg_solve's own root move
    // generation is unused by every caller (all pass only_moves) and currently
    // returns just a pass here. Top-N by static equity is a sound field, and
    // capping it keeps each greedy solve fast.
    move_list_reset(move_list);
    const MoveGenArgs gen_args = {
        .game = game,
        .move_list = move_list,
        .move_record_type = MOVE_RECORD_ALL,
        .move_sort_type = MOVE_SORT_EQUITY,
        .override_kwg = NULL,
        .eq_margin_movegen = 0,
        .target_equity = EQUITY_MAX_VALUE,
        .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
        .tiles_played_bv = NULL,
    };
    generate_moves(&gen_args);
    move_list_sort_moves(move_list);
    const int total = move_list_get_count(move_list);
    const int n_cand = total < PEG_BENCH_MAX_CANDIDATES ? total
                                                        : PEG_BENCH_MAX_CANDIDATES;
    const Move *cands[PEG_BENCH_MAX_CANDIDATES];
    for (int i = 0; i < n_cand; i++) {
      cands[i] = move_list_get_move(move_list, i);
    }
    if (n_cand > 0) {
      PegArgs peg_args = {0};
      peg_args.game = game;
      peg_args.thread_control = thread_control;
      peg_args.num_threads = 1;
      peg_args.greedy_seed_only = true;
      peg_args.time_budget_seconds = budget_s;
      peg_args.only_moves = cands;
      peg_args.n_only_moves = n_cand;
      thread_control_set_status(thread_control, THREAD_CONTROL_STATUS_STARTED);
      PegResult peg_result = {0};
      peg_solve(&peg_args, &peg_result, error_stack);
      if (error_stack_is_empty(error_stack) && peg_result.n_top_cands > 0 &&
          peg_result.best_win >= 0.0) {
        play_move(&peg_result.best_move, game, NULL);
        played = true;
      }
      peg_result_destroy(&peg_result);
    }
  }
  if (!played) {
    // A solve produced nothing (e.g. a severe budget, or a bag outside the PEG
    // range that should not occur mid-playout): fall back to the static best.
    const Move *move = get_top_equity_move(game, move_list);
    play_move(move, game, NULL);
  }
  *elapsed_out = peg_bench_now_s() - t0;
}

// Play the game to the end from its current state, logging per-turn wall time
// against the budget. Returns player 0's final spread (score0 - score1).
static int peg_bench_play_out(Game *game, MoveList *move_list,
                              ThreadControl *thread_control,
                              ErrorStack *error_stack) {
  int turn = 0;
  double total = 0.0;
  double worst = 0.0;
  while (!game_over(game)) {
    const int player_idx = game_get_player_on_turn_index(game);
    const int bag = bag_get_letters(game_get_bag(game));
    double elapsed = 0.0;
    peg_bench_play_turn(game, move_list, thread_control, PEG_BENCH_TURN_BUDGET_S,
                        &elapsed, error_stack);
    total += elapsed;
    if (elapsed > worst) {
      worst = elapsed;
    }
    printf("    turn %2d  p%d  bag=%d  %.2fs%s\n", ++turn, player_idx, bag,
           elapsed,
           elapsed > PEG_BENCH_TURN_BUDGET_S * 1.5 ? "  *** OVER BUDGET" : "");
    if (!error_stack_is_empty(error_stack)) {
      printf("    ERROR on turn %d: ", turn);
      error_stack_print_and_reset(error_stack);
      break;
    }
  }
  printf("    (%d turns, %.2fs total, worst turn %.2fs vs %.2fs budget)\n", turn,
         total, worst, PEG_BENCH_TURN_BUDGET_S);
  return equity_to_int(player_get_score(game_get_player(game, 0))) -
         equity_to_int(player_get_score(game_get_player(game, 1)));
}

void test_peginf_benchmark(void) {
  Config *config = config_create_or_die(
      "set -lex CSW24 -s1 equity -s2 equity -r1 all -r2 all -numplays 15 "
      "-threads 1");
  load_and_exec_config_or_die(config, PEG_BENCH_4BAG_CGP ";");
  Game *game = config_get_game(config);
  ThreadControl *thread_control = config_get_thread_control(config);
  MoveList *move_list = move_list_create(64);
  ErrorStack *error_stack = error_stack_create();

  printf("  PEG benchmark playout (4-in-bag CSW24, budget %.1fs/turn):\n",
         PEG_BENCH_TURN_BUDGET_S);
  const int spread =
      peg_bench_play_out(game, move_list, thread_control, error_stack);
  printf("  final spread (p0 - p1): %+d\n", spread);

  assert(game_over(game));
  assert(error_stack_is_empty(error_stack));

  move_list_destroy(move_list);
  error_stack_destroy(error_stack);
  config_destroy(config);
}
