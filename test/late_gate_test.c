#include "late_gate_test.h"

#include "../src/def/board_defs.h"
#include "../src/def/sim_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/win_pct.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/play_chooser.h"
#include "../src/util/io_util.h"
#include "test_util.h"
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

// Late-game policy gate (on-demand test "lategate").
//
// Does a defense-aware rollout policy earn its keep once a simulation has
// enough samples for its noise to fall below the policy's bias? Positions
// with LATEGATE_MIN_BAG..LATEGATE_MAX_BAG tiles in the bag are sampled from
// static self-play, and from each one two continuation games are played
// with the roles swapped: player A sims with the defense term steering its
// rollouts for LATEGATE_ROLLOUT_A plies at LATEGATE_ITERS_A rollouts per
// move, player B with LATEGATE_ROLLOUT_B plies at LATEGATE_ITERS_B. Both
// players carry the same weights at the root, so their candidate pools are
// identical and only the rollout policy and sample count differ. This is a
// test of the sim policy alone, so no solver takes part: both players sim
// while the bag holds at least LATEGATE_SIM_MIN_BAG tiles (every move with
// tiles in the bag, by default) and play plain static equity below that and
// through the endgame, so the games differ only through the sim decisions.
// The default horizon, the simmer's ceiling of MAX_PLIES, lets every rollout
// run to the end of the game, so its value is the game's actual result
// rather than a win-table estimate. Both continuations of a position draw the
// same tiles in the same order.
//
// Environment: LATEGATE_OUT (lategate.csv), LATEGATE_POSITIONS (200),
// LATEGATE_ITERS_A (160000), LATEGATE_ITERS_B (200000), LATEGATE_ROLLOUT_A
// (2), LATEGATE_ROLLOUT_B (0), LATEGATE_LEAF_A (0), LATEGATE_LEAF_B (0),
// LATEGATE_PLIES (MAX_PLIES), LATEGATE_CANDIDATES (15), LATEGATE_THREADS
// (16), LATEGATE_WINPCT (winpct),
// LATEGATE_MIN_BAG (5), LATEGATE_MAX_BAG (9), LATEGATE_SIM_MIN_BAG (1),
// LATEGATE_SEED (1), LATEGATE_TWD (gs050_105),
// LATEGATE_PATH (./data), LATEGATE_LEX (CSW24), LATEGATE_WMP (true).

enum {
  LATEGATE_DEFAULT_POSITIONS = 200,
  LATEGATE_DEFAULT_ITERS_A = 160000,
  LATEGATE_DEFAULT_ITERS_B = 200000,
  LATEGATE_DEFAULT_ROLLOUT_A = 2,
  LATEGATE_DEFAULT_ROLLOUT_B = 0,
  // The simmer's ceiling; from a bag of at most nine tiles it reaches the
  // end of the game.
  LATEGATE_DEFAULT_PLIES = MAX_PLIES,
  LATEGATE_DEFAULT_SIM_MIN_BAG = 1,
  LATEGATE_DEFAULT_CANDIDATES = 15,
  LATEGATE_DEFAULT_THREADS = 16,
  LATEGATE_DEFAULT_MIN_BAG = 5,
  LATEGATE_DEFAULT_MAX_BAG = 9,
  LATEGATE_DEFAULT_SEED = 1,
  LATEGATE_MAX_SAMPLE_TURNS = 80,
  LATEGATE_MAX_CONTINUATION_MOVES = 80,
  LATEGATE_CMD_SIZE = 1024,
};

static long lategate_env_long(const char *name, long default_value) {
  const char *value = getenv(name);
  if (value == NULL || value[0] == '\0') {
    return default_value;
  }
  return strtol(value, NULL, 10);
}

static const char *lategate_env_string(const char *name,
                                       const char *default_value) {
  const char *value = getenv(name);
  if (value == NULL || value[0] == '\0') {
    return default_value;
  }
  return value;
}

// Plays static self-play from a fresh game until the mover's bag count
// falls inside [min_bag, max_bag] at the start of a turn. Returns false if
// the game ends or the bag skips past the range.
static bool lategate_sample_position(Game *game, uint64_t seed, int min_bag,
                                     int max_bag) {
  game_reset(game);
  game_seed(game, seed);
  draw_starting_racks(game);
  for (int turn_idx = 0; turn_idx < LATEGATE_MAX_SAMPLE_TURNS; turn_idx++) {
    if (game_over(game)) {
      return false;
    }
    const int bag_count = bag_get_letters(game_get_bag(game));
    if (bag_count >= min_bag && bag_count <= max_bag) {
      return true;
    }
    if (bag_count < min_bag) {
      return false;
    }
    play_top_n_equity_move(game, 0);
  }
  return false;
}

void test_late_gate(void) {
  const char *out_path = lategate_env_string("LATEGATE_OUT", "lategate.csv");
  const long num_positions =
      lategate_env_long("LATEGATE_POSITIONS", LATEGATE_DEFAULT_POSITIONS);
  const long iters_a =
      lategate_env_long("LATEGATE_ITERS_A", LATEGATE_DEFAULT_ITERS_A);
  const long iters_b =
      lategate_env_long("LATEGATE_ITERS_B", LATEGATE_DEFAULT_ITERS_B);
  const long rollout_a =
      lategate_env_long("LATEGATE_ROLLOUT_A", LATEGATE_DEFAULT_ROLLOUT_A);
  const long rollout_b =
      lategate_env_long("LATEGATE_ROLLOUT_B", LATEGATE_DEFAULT_ROLLOUT_B);
  const bool leaf_a = lategate_env_long("LATEGATE_LEAF_A", 0) != 0;
  const bool leaf_b = lategate_env_long("LATEGATE_LEAF_B", 0) != 0;
  const long plies =
      lategate_env_long("LATEGATE_PLIES", LATEGATE_DEFAULT_PLIES);
  const long candidates =
      lategate_env_long("LATEGATE_CANDIDATES", LATEGATE_DEFAULT_CANDIDATES);
  const long threads =
      lategate_env_long("LATEGATE_THREADS", LATEGATE_DEFAULT_THREADS);
  const int min_bag =
      (int)lategate_env_long("LATEGATE_MIN_BAG", LATEGATE_DEFAULT_MIN_BAG);
  const int sim_min_bag = (int)lategate_env_long("LATEGATE_SIM_MIN_BAG",
                                                 LATEGATE_DEFAULT_SIM_MIN_BAG);
  const int max_bag =
      (int)lategate_env_long("LATEGATE_MAX_BAG", LATEGATE_DEFAULT_MAX_BAG);
  const long base_seed =
      lategate_env_long("LATEGATE_SEED", LATEGATE_DEFAULT_SEED);
  const char *twd_name = lategate_env_string("LATEGATE_TWD", "gs050_105");
  const char *data_path = lategate_env_string("LATEGATE_PATH", "./data");
  const char *lexicon = lategate_env_string("LATEGATE_LEX", "CSW24");
  const char *use_wmp = lategate_env_string("LATEGATE_WMP", "true");
  const char *win_pct_name = lategate_env_string("LATEGATE_WINPCT", "winpct");
  const bool verbose = lategate_env_long("LATEGATE_VERBOSE", 0) != 0;

  char cmd[LATEGATE_CMD_SIZE];
  snprintf(cmd, sizeof(cmd),
           "set -lex %s -wmp %s -s1 equity -s2 equity -r1 all -r2 all "
           "-numplays %ld -threads %ld -seed 7 -savesettings false -path %s "
           "-twd %s -winpct %s",
           lexicon, use_wmp, candidates, threads, data_path, twd_name,
           win_pct_name);
  Config *config = config_create_or_die(cmd);
  char empty_cgp[LATEGATE_CMD_SIZE];
  int cgp_len = snprintf(empty_cgp, sizeof(empty_cgp), "cgp ");
  for (int row = 0; row < BOARD_DIM; row++) {
    cgp_len +=
        snprintf(empty_cgp + cgp_len, sizeof(empty_cgp) - (size_t)cgp_len,
                 "%d%s", BOARD_DIM, row + 1 < BOARD_DIM ? "/" : "");
  }
  snprintf(empty_cgp + cgp_len, sizeof(empty_cgp) - (size_t)cgp_len,
           " / 0/0 0");
  load_and_exec_config_or_die(config, empty_cgp);
  Game *game = config_get_game(config);
  WinPct *win_pcts = config_get_win_pcts(config);
  if (win_pcts == NULL) {
    log_fatal("lategate needs a win percentage table");
  }

  const PlayChooserStrategy base_strategy = {
      .pre_endgame_eval = PLAY_CHOOSER_EVAL_SIM,
      .endgame_eval = PLAY_CHOOSER_EVAL_STATIC,
      .sim_plies = (int)plies,
      .sim_max_candidates = (int)candidates,
      .win_pcts = win_pcts,
      .num_threads = (int)threads,
      .seed = (uint64_t)base_seed,
      .set_twd_rollout_plies = true,
  };
  PlayChooserStrategy strategy_a = base_strategy;
  strategy_a.sim_max_iterations = (uint64_t)iters_a;
  strategy_a.twd_rollout_plies = (int)rollout_a;
  strategy_a.twd_leaf = leaf_a;
  PlayChooserStrategy strategy_b = base_strategy;
  strategy_b.sim_max_iterations = (uint64_t)iters_b;
  strategy_b.twd_rollout_plies = (int)rollout_b;
  strategy_b.twd_leaf = leaf_b;
  PlayChooser *chooser_a = play_chooser_create(&strategy_a);
  PlayChooser *chooser_b = play_chooser_create(&strategy_b);
  ErrorStack *error_stack = error_stack_create();

  FILE *out = fopen_or_die(out_path, "we");
  (void)fprintf(out, "pos,seed,bag,game,a_first,a_score,b_score,a_spread,"
                     "moves\n");
  Game *position = game_duplicate(game);
  long positions_done = 0;
  long games_played = 0;
  double a_points = 0.0;
  long a_spread_sum = 0;
  long pairs_a_both = 0;
  long pairs_split = 0;
  long pairs_b_both = 0;
  uint64_t sample_seed = (uint64_t)base_seed * 1000003ULL;
  while (positions_done < num_positions) {
    sample_seed++;
    if (!lategate_sample_position(game, sample_seed, min_bag, max_bag)) {
      continue;
    }
    game_copy(position, game);
    const int start_player = game_get_player_on_turn_index(position);
    const int bag_at_start = bag_get_letters(game_get_bag(position));
    double pair_points = 0.0;
    for (int continuation = 0; continuation < 2; continuation++) {
      const bool a_first = (continuation == 0);
      game_copy(game, position);
      // Both continuations of a position draw the same tiles in the same
      // order (the game-pair idiom), so the pair differs only in play.
      game_seed(game, sample_seed * 7919ULL + 17ULL);
      const int a_player = a_first ? start_player : 1 - start_player;
      int moves = 0;
      while (!game_over(game) && moves < LATEGATE_MAX_CONTINUATION_MOVES) {
        if (verbose) {
          printf("lategate: pos %ld game %d move %d: bag %d, on turn %d, %s\n",
                 positions_done, continuation, moves,
                 bag_get_letters(game_get_bag(game)),
                 game_get_player_on_turn_index(game),
                 bag_get_letters(game_get_bag(game)) < sim_min_bag ? "static"
                                                                   : "sim");
          (void)fflush(stdout);
        }
        if (bag_get_letters(game_get_bag(game)) < sim_min_bag) {
          // Below the sim zone, and through the endgame, both sides play
          // plain static equity: no solver, no sim.
          play_top_n_equity_move(game, 0);
          moves++;
          continue;
        }
        const bool a_to_move = game_get_player_on_turn_index(game) == a_player;
        Move move;
        play_chooser_choose_move(a_to_move ? chooser_a : chooser_b, game, &move,
                                 error_stack);
        if (!error_stack_is_empty(error_stack)) {
          error_stack_print_and_reset(error_stack);
          log_fatal("lategate: play chooser failed at position %ld",
                    positions_done);
        }
        play_move(&move, game, NULL);
        moves++;
      }
      const int a_score =
          equity_to_int(player_get_score(game_get_player(game, a_player)));
      const int b_score =
          equity_to_int(player_get_score(game_get_player(game, 1 - a_player)));
      const int a_spread = a_score - b_score;
      double points = 0.0;
      if (a_spread > 0) {
        points = 1.0;
      } else if (a_spread == 0) {
        points = 0.5;
      }
      a_points += points;
      pair_points += points;
      a_spread_sum += a_spread;
      games_played++;
      (void)fprintf(out, "%ld,%llu,%d,%d,%d,%d,%d,%d,%d\n", positions_done,
                    (unsigned long long)sample_seed, bag_at_start, continuation,
                    a_first ? 1 : 0, a_score, b_score, a_spread, moves);
      (void)fflush(out);
    }
    if (pair_points > 1.5) {
      pairs_a_both++;
    } else if (pair_points < 0.5) {
      pairs_b_both++;
    } else {
      pairs_split++;
    }
    positions_done++;
    if (positions_done % 10 == 0) {
      printf("lategate: %ld positions, A %.2f%% over %ld games, spread %+.2f "
             "(pairs A/split/B %ld/%ld/%ld)\n",
             positions_done, 100.0 * a_points / (double)games_played,
             games_played, (double)a_spread_sum / (double)games_played,
             pairs_a_both, pairs_split, pairs_b_both);
      (void)fflush(stdout);
    }
  }
  const double a_win_rate = a_points / (double)games_played;
  const double sigma =
      sqrt(a_win_rate * (1.0 - a_win_rate) / (double)games_played);
  printf("lategate: DONE %ld positions, %ld games: A %.2f%% (+/- %.2f), "
         "spread %+.2f/game, pairs A-both/split/B-both %ld/%ld/%ld\n",
         positions_done, games_played, 100.0 * a_win_rate, 100.0 * sigma,
         (double)a_spread_sum / (double)games_played, pairs_a_both, pairs_split,
         pairs_b_both);
  (void)fclose(out);
  game_destroy(position);
  error_stack_destroy(error_stack);
  play_chooser_destroy(chooser_a);
  play_chooser_destroy(chooser_b);
  config_destroy(config);
}
