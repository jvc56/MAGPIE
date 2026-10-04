#ifndef SIMMEDINF_BENCHMARK_H
#define SIMMEDINF_BENCHMARK_H

#include "../src/ent/game.h"
#include "../src/ent/inference_args.h"
#include "../src/ent/inference_results.h"
#include "../src/ent/move.h"
#include "../src/ent/rack.h"
#include "../src/ent/sim_results.h"
#include "../src/ent/thread_control.h"
#include "../src/ent/win_pct.h"
#include "../src/impl/simmer.h"
#include "../src/util/io_util.h"
#include <stdbool.h>

void test_simmedinf_benchmark(void);

// Exposed for reuse by other harnesses that need the same "run simmed
// inference on the opponent's previous move, then simulate with the
// precomputed distribution" wiring (see test/siminf_oracle_test.c).

// Timer thread: fires USER_INTERRUPT on tc after `seconds`, unless `done` is
// set first.
typedef struct {
  ThreadControl *tc;
  double seconds;
  volatile bool done;
} TimerArgs;

void *timer_thread_func(void *arg);

// Number of tiles unseen by the player currently on turn (bag plus the
// opponent's rack).
int tiles_unseen(const Game *game);

// Rack storage plus the InferenceArgs that reference it, for inferring the
// opponent's previous move. The setup must outlive any simmed_infer() or
// simulate() call that uses its args (the args hold pointers into it).
typedef struct {
  Rack target_played_tiles;
  Rack target_known_rack;
  Rack nontarget_known_rack;
  InferenceArgs args;
} PrevMoveInferSetup;

void fill_prev_move_infer_args(PrevMoveInferSetup *setup,
                               const Game *game_before_prev,
                               const Move *prev_move, int prev_player_index,
                               Equity equity_margin, ThreadControl *tc);

// Runs simmed_infer() on the opponent's previous move, populating
// inference_results with a weighted leave distribution. Returns whether it
// completed without error; *elapsed_out receives the wall-clock time spent.
bool run_simmed_inference(const Game *game_before_prev, WinPct *win_pcts,
                          ThreadControl *tc, const Move *prev_move,
                          int prev_player_index,
                          InferenceResults *inference_results,
                          ErrorStack *error_stack, double *elapsed_out);

// Generates moves, runs the outer sim for budget_s seconds, and plays the
// sim-best move (mutating `game`). Returns the move played (aliases into
// move_list -- copy it out before the next call reuses that storage). Three
// inference modes: inference_results == NULL is plain sim (uniform rack
// sampling); results_precomputed true means inference_results already holds
// a (simmed) leave distribution; static_infer_args non-NULL runs simulate()'s
// own static inference into inference_results.
const Move *play_sim_turn(Game *game, MoveList *move_list,
                          SimResults *sim_results, SimCtx **sim_ctx,
                          WinPct *win_pcts, ThreadControl *tc, int num_plies,
                          double budget_s, InferenceResults *inference_results,
                          bool results_precomputed,
                          const InferenceArgs *static_infer_args,
                          ErrorStack *error_stack);

#endif
