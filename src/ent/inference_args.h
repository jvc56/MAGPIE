#ifndef INFERENCE_ARGS_H
#define INFERENCE_ARGS_H

#include "../def/inference_defs.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/game_history.h"
#include "../ent/inference_results.h"
#include "../ent/move.h"
#include "../ent/rack.h"
#include "../ent/thread_control.h"
#include "../ent/win_pct.h"
#include "../util/io_util.h"

typedef struct InferenceArgs {
  bool use_game_history;
  bool use_inference_cutoff_optimization;
  GameHistory *game_history;
  int target_index;
  Equity target_score;
  int target_num_exch;
  int leave_list_capacity;
  Equity equity_margin;
  Rack *target_played_tiles;
  Rack *target_known_rack;
  Rack *nontarget_known_rack;
  const Game *game;
  int num_threads;
  int parent_worker_thread_index;
  int print_interval;
  ThreadControl *thread_control;
  // Win-probability inference mode fields (unused in equity mode).
  inference_mode_t mode;
  double tau;
  inference_sample_mode_t sample_mode;
  int mini_sim_plies;
  int mini_sim_max_plays;
  uint64_t mc_max_iters;
  int mc_time_limit_secs;
  int max_enum_hypotheses;
  double min_weight_eps;
  const WinPct *win_pcts;
  // Full target move (set from game history; NULL when only score+tiles are
  // known, e.g. direct command invocation).  When non-NULL,
  // candidate_matches_target uses exact position matching instead of
  // score+tile-multiset.
  const Move *target_move;
} InferenceArgs;

static inline void
infer_args_fill(InferenceArgs *args, int leave_list_capacity, Equity eq_margin,
                GameHistory *game_history, const Game *game, int num_threads,
                int parent_worker_thread_index, int print_interval,
                ThreadControl *thread_control, bool use_game_history,
                bool use_inference_cutoff_optimization, int target_index,
                Equity target_score, int target_num_exch,
                Rack *target_played_tiles, Rack *target_known_rack,
                Rack *nontarget_known_rack) {
  args->target_index = target_index;
  args->target_score = target_score;
  args->target_num_exch = target_num_exch;
  args->leave_list_capacity = leave_list_capacity;
  args->equity_margin = eq_margin;
  args->target_played_tiles = target_played_tiles;
  args->target_known_rack = target_known_rack;
  args->nontarget_known_rack = nontarget_known_rack;
  args->use_game_history = use_game_history;
  args->use_inference_cutoff_optimization = use_inference_cutoff_optimization;
  args->game_history = game_history;
  args->game = game;
  args->num_threads = num_threads;
  args->parent_worker_thread_index = parent_worker_thread_index;
  args->print_interval = print_interval;
  args->thread_control = thread_control;
  args->mode = INFERENCE_MODE_EQUITY;
  args->tau = INFERENCE_WINPCT_DEFAULT_TAU;
  args->sample_mode = INFERENCE_SAMPLE_MODE_AUTO;
  args->mini_sim_plies = INFERENCE_WINPCT_DEFAULT_SIM_PLIES;
  args->mini_sim_max_plays = INFERENCE_WINPCT_DEFAULT_SIM_MAX_PLAYS;
  args->mc_max_iters = INFERENCE_WINPCT_DEFAULT_MC_MAX_ITERS;
  args->mc_time_limit_secs = INFERENCE_WINPCT_DEFAULT_TIME_LIMIT_SECS;
  args->max_enum_hypotheses = INFERENCE_WINPCT_DEFAULT_MAX_ENUM_HYPOTHESES;
  args->min_weight_eps = INFERENCE_WINPCT_DEFAULT_MIN_WEIGHT_EPS;
  args->win_pcts = NULL;
  args->target_move = NULL;
}

#endif