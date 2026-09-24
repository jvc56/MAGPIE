#include "pat_vol.h"

#include "../def/game_history_defs.h"
#include "../def/players_data_defs.h"
#include "../def/rack_defs.h"
#include "../ent/bag.h"
#include "../ent/board.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/move.h"
#include "../ent/pat.h"
#include "../ent/player.h"
#include "../ent/rack.h"
#include "../ent/sim_args.h"
#include "../ent/win_pct.h"
#include "../util/io_util.h"
#include "gameplay.h"
#include <math.h>
#include <stdbool.h>

enum {
  // Finite-difference half-width, in points, for the utility curvature;
  // wide enough to smooth the table's integer margin buckets.
  PAT_VOL_KAPPA_STEP = 15,
  // Tiles an average later ply draws, for the unseen count at the horizon.
  PAT_VOL_TILES_PER_PLY = 4,
  PAT_VOL_QUADRATURE_POINTS = 9,
};

// Nine-point Gauss-Hermite nodes and weights for E[f(Z)], Z ~ N(0, 1):
// E[f(Z)] ~ sum_i weight_i * f(sqrt(2) * node_i) / sqrt(pi).
static const double pat_vol_nodes[PAT_VOL_QUADRATURE_POINTS] = {
    -3.1909932017815276, -2.2665805845318431, -1.4685532892166679,
    -0.7235510187528376, 0.0,                 0.7235510187528376,
    1.4685532892166679,  2.2665805845318431,  3.1909932017815276};
static const double pat_vol_weights[PAT_VOL_QUADRATURE_POINTS] = {
    3.9606977263264381e-05, 4.9436242755369472e-03, 8.8474527394376573e-02,
    4.3265155900255575e-01, 7.2023521560605097e-01, 4.3265155900255575e-01,
    8.8474527394376573e-02, 4.9436242755369472e-03, 3.9606977263264381e-05};

// The default sim utility (win 1.0, spread 0.5, scale 100) of the mover at
// margin with unseen tiles left, from whichever side is on turn.
static double pat_vol_utility(const WinPct *win_pcts, double margin,
                              int unseen, bool mover_on_turn) {
  const int rounded = (int)lround(margin);
  const double win =
      mover_on_turn
          ? win_pct_get(win_pcts, rounded, (unsigned)unseen)
          : 1.0 - win_pct_get(win_pcts, -rounded, (unsigned)unseen);
  return sim_utility_blend(win, double_to_equity(margin), 1.0, 0.5, 100.0);
}

// -U''/U' of that utility at margin (opponent on turn), by central
// differences.
static double pat_vol_kappa(const WinPct *win_pcts, double margin,
                            int unseen) {
  const double step = PAT_VOL_KAPPA_STEP;
  const double lower =
      pat_vol_utility(win_pcts, margin - step, unseen, false);
  const double middle = pat_vol_utility(win_pcts, margin, unseen, false);
  const double upper =
      pat_vol_utility(win_pcts, margin + step, unseen, false);
  const double slope = (upper - lower) / (2.0 * step);
  const double curvature = (upper - 2.0 * middle + lower) / (step * step);
  return slope > 0.0 ? -curvature / slope : 0.0;
}

// E[U(mean + sigma Z)] at the horizon.
static double pat_vol_expected_utility(const WinPct *win_pcts, double mean,
                                       double sigma, int unseen,
                                       bool mover_on_turn) {
  double total = 0.0;
  for (int point_idx = 0; point_idx < PAT_VOL_QUADRATURE_POINTS;
       point_idx++) {
    total += pat_vol_weights[point_idx] *
             pat_vol_utility(win_pcts,
                             mean + M_SQRT2 * sigma * pat_vol_nodes[point_idx],
                             unseen, mover_on_turn);
  }
  return total / sqrt(M_PI);
}

// Mode "eu": the win table already averages over an ordinary position's
// future swings, so only the variance a move adds beyond its least
// volatile sibling is integrated, on the current row (opponent on turn,
// the same unseen count for every candidate so levels compare).
static int pat_vol_choose_expected_utility(
    const Game *game, const MoveList *move_list, const WinPct *win_pcts,
    const PATEvalContext *ctx, double window, double margin_before,
    int bag) {
  const int num_moves = move_list_get_count(move_list);
  double *vol_terms = malloc_or_die(sizeof(double) * (size_t)num_moves);
  int num_candidates = 0;
  double best_equity = 0.0;
  double max_vol_term = -HUGE_VAL;
  for (int move_idx = 0; move_idx < num_moves; move_idx++) {
    const Move *move = move_list_get_move(move_list, move_idx);
    vol_terms[move_idx] = 0.0;
    if (move_get_type(move) == GAME_EVENT_PASS) {
      continue;
    }
    const double equity = equity_to_double(move_get_equity(move));
    if (num_candidates == 0) {
      best_equity = equity;
    }
    if (best_equity - equity > window) {
      break;
    }
    num_candidates = move_idx + 1;
    Rack leave;
    get_leave_for_move(move, game, &leave);
    vol_terms[move_idx] =
        equity_to_double(pat_eval_move_penalty(ctx, move, &leave));
    if (vol_terms[move_idx] > max_vol_term) {
      max_vol_term = vol_terms[move_idx];
    }
  }
  // The largest term is the smallest second moment (the term is minus it).
  const int unseen =
      (bag > PAT_VOL_TILES_PER_PLY ? bag - PAT_VOL_TILES_PER_PLY : 0) +
      RACK_SIZE;
  int chosen = 0;
  bool have_choice = false;
  double best_value = 0.0;
  for (int move_idx = 0; move_idx < num_candidates; move_idx++) {
    const Move *move = move_list_get_move(move_list, move_idx);
    if (move_get_type(move) == GAME_EVENT_PASS) {
      continue;
    }
    const double extra_variance = max_vol_term - vol_terms[move_idx];
    const double value = pat_vol_expected_utility(
        win_pcts, margin_before + equity_to_double(move_get_equity(move)),
        sqrt(extra_variance), unseen, false);
    if (!have_choice || value > best_value) {
      have_choice = true;
      best_value = value;
      chosen = move_idx;
    }
  }
  free(vol_terms);
  return chosen;
}

// Mode "u_direct": each candidate's value is the default win/spread
// utility read off the win table at margin + equity on the row its own
// draw leads to (opponent on turn): the table already averages over an
// ordinary position's future, so nothing is integrated. The table is
// averaged over margins within smooth points to damp its bucket noise.
static int pat_vol_choose_u_direct(const Game *game,
                                   const MoveList *move_list,
                                   const WinPct *win_pcts, double window,
                                   double margin_before, int bag,
                                   int smooth) {
  const int num_moves = move_list_get_count(move_list);
  int chosen = 0;
  bool have_choice = false;
  double best_equity = 0.0;
  double best_value = 0.0;
  for (int move_idx = 0; move_idx < num_moves; move_idx++) {
    const Move *move = move_list_get_move(move_list, move_idx);
    if (move_get_type(move) == GAME_EVENT_PASS) {
      continue;
    }
    const double equity = equity_to_double(move_get_equity(move));
    if (!have_choice) {
      best_equity = equity;
    }
    if (best_equity - equity > window) {
      break;
    }
    const int drawn = move_get_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE
                          ? move_get_tiles_played(move)
                          : 0;
    const int unseen = (bag > drawn ? bag - drawn : 0) + RACK_SIZE;
    double value = 0.0;
    for (int offset = -smooth; offset <= smooth; offset++) {
      value += pat_vol_utility(win_pcts, margin_before + equity + offset,
                               unseen, false);
    }
    value /= (double)(2 * smooth + 1);
    if (!have_choice || value > best_value) {
      have_choice = true;
      best_value = value;
      chosen = move_idx;
    }
  }
  (void)game;
  return chosen;
}

int pat_vol_choose(const Game *game, const MoveList *move_list,
                   const PATWeights *policy, const WinPct *win_pcts,
                   PATEvalContext *ctx, double window) {
  const int mover_index = game_get_player_on_turn_index(game);
  const Player *mover = game_get_player(game, mover_index);
  const Player *opponent = game_get_player(game, 1 - mover_index);
  const PATWeights *vol_model = pat_get_vol_model(policy);
  const bool expected_utility = pat_get_vol_expected_utility(policy);
  const double vol_scale = pat_get_vol_scale(policy);
  const double vol_cap = pat_get_vol_cap(policy);
  const int csi = board_get_cross_set_index(
      game_get_data_is_shared(game, PLAYERS_DATA_TYPE_KWG), mover_index);
  pat_eval_context_load(ctx, vol_model,
                        board_get_readonly_lanes(game_get_board(game), csi),
                        game_get_ld(game), player_get_rack(mover),
                        PAT_CLASS_MASK_ALL,
                        rack_get_total_letters(player_get_rack(opponent)));
  pat_eval_context_set_kwg(ctx, player_get_kwg(mover));
  const double margin_before = equity_to_double(player_get_score(mover) -
                                                player_get_score(opponent));
  const int bag = bag_get_letters(game_get_bag(game));
  if (pat_get_vol_u_direct(policy)) {
    return pat_vol_choose_u_direct(game, move_list, win_pcts, window,
                                   margin_before, bag,
                                   pat_get_vol_smooth(policy));
  }
  if (expected_utility) {
    return pat_vol_choose_expected_utility(game, move_list, win_pcts, ctx,
                                           window, margin_before, bag);
  }
  const bool relative = pat_get_vol_ce_relative(policy);
  const bool row_fixed = pat_get_vol_row_fixed(policy);
  double vol_const = 0.0;
  double vol_const_slope = 0.0;
  const bool use_const =
      pat_get_vol_const(policy, &vol_const, &vol_const_slope);
  const bool equity_center = pat_get_vol_ce_equity_center(policy);
  // A pass carries a sentinel equity, so the window is anchored on the
  // first real move; a list with only the pass keeps it. The first pass
  // gathers each candidate's term (and, for ce_rel, the largest, which is
  // the least volatile candidate's since the term is minus the moment).
  const int num_moves = move_list_get_count(move_list);
  double *vol_terms = malloc_or_die(sizeof(double) * (size_t)(num_moves + 1));
  int num_candidates = 0;
  double best_equity = 0.0;
  double max_vol_term = -HUGE_VAL;
  for (int move_idx = 0; move_idx < num_moves; move_idx++) {
    const Move *move = move_list_get_move(move_list, move_idx);
    vol_terms[move_idx] = 0.0;
    if (move_get_type(move) == GAME_EVENT_PASS) {
      continue;
    }
    const double equity = equity_to_double(move_get_equity(move));
    if (num_candidates == 0) {
      best_equity = equity;
    }
    if (best_equity - equity > window) {
      break;
    }
    num_candidates = move_idx + 1;
    if (use_const) {
      vol_terms[move_idx] = vol_const + vol_const_slope * bag;
    } else {
      Rack leave;
      get_leave_for_move(move, game, &leave);
      vol_terms[move_idx] =
          equity_to_double(pat_eval_move_penalty(ctx, move, &leave));
    }
    if (vol_terms[move_idx] > max_vol_term) {
      max_vol_term = vol_terms[move_idx];
    }
  }
  int chosen = 0;
  bool have_choice = false;
  double best_value = 0.0;
  for (int move_idx = 0; move_idx < num_candidates; move_idx++) {
    const Move *move = move_list_get_move(move_list, move_idx);
    if (move_get_type(move) == GAME_EVENT_PASS) {
      continue;
    }
    const double equity = equity_to_double(move_get_equity(move));
    const double vol_term =
        relative ? vol_terms[move_idx] - max_vol_term : vol_terms[move_idx];
    const int drawn = move_get_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE
                          ? move_get_tiles_played(move)
                          : 0;
    const int unseen =
        row_fixed ? (bag > PAT_VOL_TILES_PER_PLY ? bag - PAT_VOL_TILES_PER_PLY
                                                 : 0) +
                        RACK_SIZE
                  : (bag > drawn ? bag - drawn : 0) + RACK_SIZE;
    const double margin =
        margin_before +
        (equity_center ? equity : equity_to_double(move_get_score(move)));
    double adjustment = vol_scale * 0.5 *
                        pat_vol_kappa(win_pcts, margin, unseen) * vol_term;
    if (adjustment > vol_cap) {
      adjustment = vol_cap;
    }
    const double value = equity + adjustment;
    if (!have_choice || value > best_value) {
      have_choice = true;
      best_value = value;
      chosen = move_idx;
    }
  }
  free(vol_terms);
  return chosen;
}
