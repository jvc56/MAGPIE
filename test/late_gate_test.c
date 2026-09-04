#include "late_gate_test.h"

#include "../src/compat/ctime.h"
#include "../src/def/board_defs.h"
#include "../src/def/equity_defs.h"
#include "../src/def/move_defs.h"
#include "../src/def/players_data_defs.h"
#include "../src/def/sim_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/board.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/klv.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/sim_args.h"
#include "../src/ent/sim_results.h"
#include "../src/ent/stats.h"
#include "../src/ent/tws_defense.h"
#include "../src/ent/win_pct.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"
#include "../src/impl/play_chooser.h"
#include "../src/str/move_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
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
// LATEGATE_ROOT_A (1), LATEGATE_ROOT_B (1) (whether each player's candidate
// pool is chosen with the defense term; 0 gives a player with no term
// anywhere), LATEGATE_DEFENSE_POOL_A (0), LATEGATE_DEFENSE_POOL_B (0) (a
// mixed pool: that many candidates by defensive static, the rest of the
// LATEGATE_CANDIDATES by plain static), LATEGATE_UWIN_PCT (100),
// LATEGATE_USPREAD_PCT (50), LATEGATE_USCALE (100) (the utility blend, in
// percent weights and spread points, that the sims rank by and that scores
// each game: the engine default weighs win% 1.0 and sigmoid spread 0.5),
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
  LATEGATE_NS_PER_SECOND = 1000000000,
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
  // Whether each player's candidate pool is chosen with the defense term.
  const bool root_a = lategate_env_long("LATEGATE_ROOT_A", 1) != 0;
  const bool root_b = lategate_env_long("LATEGATE_ROOT_B", 1) != 0;
  // Mixed pools: this many candidates by defensive static, the rest by
  // plain static (0 = a single ranking).
  const int defense_pool_a =
      (int)lategate_env_long("LATEGATE_DEFENSE_POOL_A", 0);
  const int defense_pool_b =
      (int)lategate_env_long("LATEGATE_DEFENSE_POOL_B", 0);
  // The objective, for the sims' ranking and for scoring the games: the
  // engine's utility blend of win% and sigmoid-normalized spread.
  const double utility_w_winpct =
      (double)lategate_env_long("LATEGATE_UWIN_PCT", 100) / 100.0;
  const double utility_w_spread =
      (double)lategate_env_long("LATEGATE_USPREAD_PCT", 50) / 100.0;
  const double utility_spread_scale =
      (double)lategate_env_long("LATEGATE_USCALE", 100);
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
      .utility_w_winpct = utility_w_winpct,
      .utility_w_spread = utility_w_spread,
      .utility_spread_scale = utility_spread_scale,
  };
  PlayChooserStrategy strategy_a = base_strategy;
  strategy_a.sim_max_iterations = (uint64_t)iters_a;
  strategy_a.twd_rollout_plies = (int)rollout_a;
  strategy_a.twd_leaf = leaf_a;
  strategy_a.disable_root_twd = !root_a;
  strategy_a.defense_pool_size = defense_pool_a;
  PlayChooserStrategy strategy_b = base_strategy;
  strategy_b.sim_max_iterations = (uint64_t)iters_b;
  strategy_b.twd_rollout_plies = (int)rollout_b;
  strategy_b.twd_leaf = leaf_b;
  strategy_b.disable_root_twd = !root_b;
  strategy_b.defense_pool_size = defense_pool_b;
  PlayChooser *chooser_a = play_chooser_create(&strategy_a);
  PlayChooser *chooser_b = play_chooser_create(&strategy_b);
  ErrorStack *error_stack = error_stack_create();

  FILE *out = fopen_or_die(out_path, "we");
  (void)fprintf(out, "pos,seed,bag,game,a_first,a_score,b_score,a_spread,"
                     "moves,a_sim_moves,b_sim_moves,a_seconds,b_seconds,"
                     "a_utility\n");
  Game *position = game_duplicate(game);
  long positions_done = 0;
  long games_played = 0;
  double a_points = 0.0;
  long a_spread_sum = 0;
  long pairs_a_both = 0;
  long pairs_split = 0;
  long pairs_b_both = 0;
  // Wall-clock spent inside each player's sim decisions, to check how well
  // the two sample counts approximate an equal time budget.
  int64_t a_total_ns = 0;
  int64_t b_total_ns = 0;
  long a_total_sim_moves = 0;
  long b_total_sim_moves = 0;
  // A's realized utility summed over games, and its per-pair edge (A's
  // utility in the pair less B's, which is 1 - A's in the same game).
  double a_utility_sum = 0.0;
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
      int64_t a_game_ns = 0;
      int64_t b_game_ns = 0;
      int a_game_sim_moves = 0;
      int b_game_sim_moves = 0;
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
        const int64_t start_ns = ctimer_monotonic_ns();
        play_chooser_choose_move(a_to_move ? chooser_a : chooser_b, game, &move,
                                 error_stack);
        const int64_t elapsed_ns = ctimer_monotonic_ns() - start_ns;
        if (a_to_move) {
          a_game_ns += elapsed_ns;
          a_game_sim_moves++;
        } else {
          b_game_ns += elapsed_ns;
          b_game_sim_moves++;
        }
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
      // Realized utility: the game's result as win%, blended with its
      // final spread exactly as the sims blend their rollouts.
      const double a_utility =
          sim_utility_blend(points, int_to_equity(a_spread), utility_w_winpct,
                            utility_w_spread, utility_spread_scale);
      a_utility_sum += a_utility;
      games_played++;
      a_total_ns += a_game_ns;
      b_total_ns += b_game_ns;
      a_total_sim_moves += a_game_sim_moves;
      b_total_sim_moves += b_game_sim_moves;
      (void)fprintf(out, "%ld,%llu,%d,%d,%d,%d,%d,%d,%d,%d,%d,%.3f,%.3f,%.6f\n",
                    positions_done, (unsigned long long)sample_seed,
                    bag_at_start, continuation, a_first ? 1 : 0, a_score,
                    b_score, a_spread, moves, a_game_sim_moves,
                    b_game_sim_moves,
                    (double)a_game_ns / LATEGATE_NS_PER_SECOND,
                    (double)b_game_ns / LATEGATE_NS_PER_SECOND, a_utility);
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
      printf(
          "lategate: %ld positions, A %.2f%% over %ld games, spread %+.2f "
          "(pairs A/split/B %ld/%ld/%ld); sim time A %.1fs B %.1fs "
          "(A/B %.3f), per sim move A %.3fs B %.3fs\n",
          positions_done, 100.0 * a_points / (double)games_played, games_played,
          (double)a_spread_sum / (double)games_played, pairs_a_both,
          pairs_split, pairs_b_both,
          (double)a_total_ns / LATEGATE_NS_PER_SECOND,
          (double)b_total_ns / LATEGATE_NS_PER_SECOND,
          b_total_ns > 0 ? (double)a_total_ns / (double)b_total_ns : 0.0,
          a_total_sim_moves > 0 ? (double)a_total_ns / LATEGATE_NS_PER_SECOND /
                                      (double)a_total_sim_moves
                                : 0.0,
          b_total_sim_moves > 0 ? (double)b_total_ns / LATEGATE_NS_PER_SECOND /
                                      (double)b_total_sim_moves
                                : 0.0);
      (void)fflush(stdout);
    }
  }
  const double a_win_rate = a_points / (double)games_played;
  const double sigma =
      sqrt(a_win_rate * (1.0 - a_win_rate) / (double)games_played);
  printf("lategate: DONE %ld positions, %ld games: A %.2f%% (+/- %.2f), "
         "spread %+.2f/game, utility %.4f (edge %+.4f), pairs "
         "A-both/split/B-both %ld/%ld/%ld; sim time A %.1fs over %ld moves, "
         "B %.1fs over %ld moves (A/B %.3f)\n",
         positions_done, games_played, 100.0 * a_win_rate, 100.0 * sigma,
         (double)a_spread_sum / (double)games_played,
         a_utility_sum / (double)games_played,
         a_utility_sum / (double)games_played - 0.5, pairs_a_both, pairs_split,
         pairs_b_both, (double)a_total_ns / LATEGATE_NS_PER_SECOND,
         a_total_sim_moves, (double)b_total_ns / LATEGATE_NS_PER_SECOND,
         b_total_sim_moves,
         b_total_ns > 0 ? (double)a_total_ns / (double)b_total_ns : 0.0);
  (void)fclose(out);
  game_destroy(position);
  error_stack_destroy(error_stack);
  play_chooser_destroy(chooser_a);
  play_chooser_destroy(chooser_b);
  config_destroy(config);
}

// Late-game candidate pool collection (on-demand test "latepool").
//
// For each sampled late-game position, the candidate pool is the union of
// the top LATEPOOL_TOP plays by static equity without the defense term and
// the top LATEPOOL_TOP with it, and every candidate is simulated for a flat
// LATEPOOL_SAMPLES rollouts (round robin, no adaptive sampling, no stopping
// condition) with rollouts running to the end of the game. Each candidate's
// row records its rank in both rankings, its static components, and its
// simulated win% and spread, so selection rules can be evaluated afterwards
// against the sim's own verdict on the whole union.
//
// Environment: LATEPOOL_OUT (latepool.csv), LATEPOOL_POSITIONS (10000),
// LATEPOOL_SAMPLES (1000), LATEPOOL_TOP (15), LATEPOOL_PLIES (MAX_PLIES),
// LATEPOOL_ROLLOUT (MAX_PLIES, rollout plies steered by the defense term),
// LATEPOOL_THREADS (16), LATEPOOL_MIN_BAG (5), LATEPOOL_MAX_BAG (9),
// LATEPOOL_SEED (1), LATEPOOL_TWD (gs050_105), LATEPOOL_PATH (./data),
// LATEPOOL_LEX (CSW24), LATEPOOL_WMP (true), LATEPOOL_WINPCT (winpct),
// LATEPOOL_SIM_SEED (7, the rollouts' seed; a second run on the same
// positions with another value gives an independent replicate),
// LATEPOOL_SKIP_TOP (0; see the extension note above), LATEPOOL_MAX_TURNS (0;
// when > 0 positions are taken at a random turn in 0..max_turns and kept if
// the bag is in range, for a sample spread across the game).

enum {
  LATEPOOL_DEFAULT_POSITIONS = 10000,
  LATEPOOL_DEFAULT_SAMPLES = 1000,
  LATEPOOL_DEFAULT_TOP = 15,
  LATEPOOL_PROGRESS_EVERY = 100,
};

// 1-based rank of the move in a sorted list, or 0 if absent.
static int latepool_rank(const MoveList *sorted, const Move *move) {
  for (int move_idx = 0; move_idx < move_list_get_count(sorted); move_idx++) {
    // The two rankings value the same move differently, so equity is
    // left out of the comparison.
    if (compare_moves_without_equity(move_list_get_move(sorted, move_idx), move,
                                     true) == -1) {
      return move_idx + 1;
    }
  }
  return 0;
}

static void latepool_generate(Game *game, MoveList *move_list,
                              bool disable_twd) {
  move_list_reset(move_list);
  const MoveGenArgs gen_args = {
      .game = game,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_EQUITY,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
      .move_list = move_list,
      .disable_twd = disable_twd,
  };
  generate_moves(&gen_args);
  move_list_sort_moves(move_list);
}

// Plays a random number of static turns (0..max_turns, from the seed) and
// accepts the position if the mover's bag count is within range, so the
// sample spreads over the phases of the game rather than taking the first
// turn that qualifies.
static bool latepool_sample_random_turn(Game *game, uint64_t seed,
                                        int max_turns, int min_bag,
                                        int max_bag) {
  game_reset(game);
  game_seed(game, seed);
  draw_starting_racks(game);
  const int turns =
      (int)((seed * 0x9E3779B97F4A7C15ULL >> 33) % (uint64_t)(max_turns + 1));
  for (int turn_idx = 0; turn_idx < turns; turn_idx++) {
    if (game_over(game)) {
      return false;
    }
    play_top_n_equity_move(game, 0);
  }
  if (game_over(game)) {
    return false;
  }
  const int bag_count = bag_get_letters(game_get_bag(game));
  return bag_count >= min_bag && bag_count <= max_bag;
}

void test_late_pool(void) {
  const char *out_path = lategate_env_string("LATEPOOL_OUT", "latepool.csv");
  const long num_positions =
      lategate_env_long("LATEPOOL_POSITIONS", LATEPOOL_DEFAULT_POSITIONS);
  const long samples =
      lategate_env_long("LATEPOOL_SAMPLES", LATEPOOL_DEFAULT_SAMPLES);
  const long top = lategate_env_long("LATEPOOL_TOP", LATEPOOL_DEFAULT_TOP);
  // Extension mode: candidates within this rank of either ranking were
  // simulated by an earlier run on the same seed and are skipped; the plain
  // top play is re-simulated as a reference that must reproduce its earlier
  // value, since every play's rollouts are seeded alike.
  const int skip_top = (int)lategate_env_long("LATEPOOL_SKIP_TOP", 0);
  // When > 0, sample at a random turn in 0..max_turns instead of the first
  // turn whose bag count is in range.
  const int max_turns = (int)lategate_env_long("LATEPOOL_MAX_TURNS", 0);
  const long plies = lategate_env_long("LATEPOOL_PLIES", MAX_PLIES);
  const long rollout = lategate_env_long("LATEPOOL_ROLLOUT", MAX_PLIES);
  const long threads =
      lategate_env_long("LATEPOOL_THREADS", LATEGATE_DEFAULT_THREADS);
  const int min_bag =
      (int)lategate_env_long("LATEPOOL_MIN_BAG", LATEGATE_DEFAULT_MIN_BAG);
  const int max_bag =
      (int)lategate_env_long("LATEPOOL_MAX_BAG", LATEGATE_DEFAULT_MAX_BAG);
  const long base_seed =
      lategate_env_long("LATEPOOL_SEED", LATEGATE_DEFAULT_SEED);
  const char *twd_name = lategate_env_string("LATEPOOL_TWD", "gs050_105");
  const char *data_path = lategate_env_string("LATEPOOL_PATH", "./data");
  const char *lexicon = lategate_env_string("LATEPOOL_LEX", "CSW24");
  const char *use_wmp = lategate_env_string("LATEPOOL_WMP", "true");
  const char *win_pct_name = lategate_env_string("LATEPOOL_WINPCT", "winpct");
  // The rollouts' seed, separate from the position seed so the same
  // positions can be re-simulated independently.
  const long sim_seed = lategate_env_long("LATEPOOL_SIM_SEED", 7);

  char cmd[LATEGATE_CMD_SIZE];
  snprintf(cmd, sizeof(cmd),
           "set -lex %s -wmp %s -s1 equity -s2 equity -r1 all -r2 all "
           "-numplays %ld -plies %ld -threads %ld -iter %ld -minp %ld -sr rr "
           "-threshold none -scond none -cutoff 0 -seed %ld -savesettings "
           "false -path %s -twd %s -winpct %s -twdrollout %ld",
           lexicon, use_wmp, 2 * top, plies, threads, 2 * top * samples,
           samples, sim_seed, data_path, twd_name, win_pct_name, rollout);
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
  const LetterDistribution *ld = config_get_ld(config);
  // The config creates its move list on the first generate command.
  MoveList *pool = NULL;
  SimResults *sim_results = config_get_sim_results(config);
  MoveList *with_term = move_list_create((int)top);
  MoveList *without_term = move_list_create((int)top);

  FILE *out = fopen_or_die(out_path, "we");
  (void)fprintf(out, "pos,seed,bag,cand,rank_new,rank_old,type,tiles_played,"
                     "score,leave,twd,equity_old,equity_new,win_pct,"
                     "win_pct_sd,sim_spread,sim_spread_sd,samples,move\n");
  StringBuilder *move_sb = string_builder_create();
  long positions_done = 0;
  uint64_t sample_seed = (uint64_t)base_seed * 1000003ULL;
  while (positions_done < num_positions) {
    sample_seed++;
    const bool sampled =
        max_turns > 0
            ? latepool_sample_random_turn(game, sample_seed, max_turns, min_bag,
                                          max_bag)
            : lategate_sample_position(game, sample_seed, min_bag, max_bag);
    if (!sampled) {
      continue;
    }
    const int player_index = game_get_player_on_turn_index(game);
    const Player *player = game_get_player(game, player_index);
    const KLV *klv = player_get_klv(player);
    const Board *board = game_get_board(game);
    const int bag_count = bag_get_letters(game_get_bag(game));

    if (pool == NULL) {
      load_and_exec_config_or_die(config, "gen");
      pool = config_get_move_list(config);
    }
    latepool_generate(game, with_term, false);
    latepool_generate(game, without_term, true);
    move_list_reset(pool);
    if (skip_top > 0 && move_list_get_count(without_term) > 0) {
      move_list_add_move(pool, move_list_get_move(without_term, 0));
    }
    for (int move_idx = 0; move_idx < move_list_get_count(with_term);
         move_idx++) {
      const Move *move = move_list_get_move(with_term, move_idx);
      const int rank_old = latepool_rank(without_term, move);
      if (skip_top > 0 &&
          (move_idx < skip_top || (rank_old > 0 && rank_old <= skip_top))) {
        continue;
      }
      move_list_add_move(pool, move);
    }
    for (int move_idx = 0; move_idx < move_list_get_count(without_term);
         move_idx++) {
      const Move *move = move_list_get_move(without_term, move_idx);
      if (latepool_rank(with_term, move) != 0) {
        continue;
      }
      if (skip_top > 0 && move_idx < skip_top) {
        continue;
      }
      move_list_add_move(pool, move);
    }
    const int pool_size = move_list_get_count(pool);
    if (pool_size < 2) {
      // Nothing beyond the earlier run's pool (or a one-move position); in
      // extension mode still count the position so numbering stays aligned.
      if (skip_top > 0) {
        positions_done++;
      }
      continue;
    }
    snprintf(cmd, sizeof(cmd), "set -iter %ld", (long)pool_size * samples);
    load_and_exec_config_or_die(config, cmd);
    const error_code_t status =
        config_simulate_and_return_status(config, NULL, NULL, sim_results);
    if (status != ERROR_STATUS_SUCCESS) {
      log_fatal("latepool: simulation failed with status %d", status);
    }
    const int num_plays = sim_results_get_number_of_plays(sim_results);

    TWDEvalContext twd_eval_ctx;
    twd_eval_context_disable(&twd_eval_ctx);
    const TWDWeights *twd = player_get_twd(player);
    if (twd != NULL) {
      twd_eval_context_load(
          &twd_eval_ctx, twd,
          board_get_readonly_lanes(
              board, board_get_cross_set_index(
                         game_get_data_is_shared(game, PLAYERS_DATA_TYPE_KWG),
                         player_index)),
          ld, player_get_rack(player));
    }
    for (int play_idx = 0; play_idx < num_plays; play_idx++) {
      const SimmedPlay *simmed_play =
          sim_results_get_simmed_play(sim_results, play_idx);
      const Move *move = simmed_play_get_move(simmed_play);
      const int rank_new = latepool_rank(with_term, move);
      const int rank_old = latepool_rank(without_term, move);
      Rack leave_rack;
      rack_copy(&leave_rack, player_get_rack(player));
      const Equity leave_value =
          get_leave_value_for_move(klv, move, &leave_rack);
      const Equity twd_penalty = twd_eval_move_penalty(&twd_eval_ctx, move);
      // The term is additive on top of the plain static equity, so either
      // ranking's recorded equity gives both.
      const Equity equity_old =
          rank_old > 0
              ? move_get_equity(move_list_get_move(without_term, rank_old - 1))
              : move_get_equity(move_list_get_move(with_term, rank_new - 1)) -
                    twd_penalty;
      const Equity equity_new = equity_old + twd_penalty;
      const Stat *win_pct_stat = simmed_play_get_win_pct_stat(simmed_play);
      const Stat *equity_stat = simmed_play_get_equity_stat(simmed_play);
      string_builder_clear(move_sb);
      string_builder_add_move(move_sb, board, move, ld, false);
      (void)fprintf(
          out,
          "%ld,%llu,%d,%d,%d,%d,%d,%d,%.3f,%.3f,%.3f,%.3f,%.3f,"
          "%.6f,%.6f,%.3f,%.3f,%llu,%s\n",
          positions_done, (unsigned long long)sample_seed, bag_count, play_idx,
          rank_new, rank_old, (int)move_get_type(move),
          move_get_tiles_played(move), equity_to_double(move_get_score(move)),
          equity_to_double(leave_value), equity_to_double(twd_penalty),
          equity_to_double(equity_old), equity_to_double(equity_new),
          stat_get_mean(win_pct_stat), stat_get_stdev(win_pct_stat),
          stat_get_mean(equity_stat), stat_get_stdev(equity_stat),
          (unsigned long long)stat_get_num_samples(win_pct_stat),
          string_builder_peek(move_sb));
    }
    if (skip_top > 0) {
      // Rank-only rows for the candidates the earlier run simulated, so the
      // merged data carries every move's rank in both deeper rankings.
      for (int list_idx = 0; list_idx < 2; list_idx++) {
        const MoveList *list = list_idx == 0 ? with_term : without_term;
        for (int move_idx = 0; move_idx < move_list_get_count(list);
             move_idx++) {
          const Move *move = move_list_get_move(list, move_idx);
          const int rank_new = latepool_rank(with_term, move);
          const int rank_old = latepool_rank(without_term, move);
          const bool simmed_before = (rank_new > 0 && rank_new <= skip_top) ||
                                     (rank_old > 0 && rank_old <= skip_top);
          // Each move once: from the defensive list, or from the plain list
          // when it is absent from the defensive one.
          if (!simmed_before || (list_idx == 1 && rank_new > 0)) {
            continue;
          }
          string_builder_clear(move_sb);
          string_builder_add_move(move_sb, board, move, ld, false);
          (void)fprintf(out, "%ld,%llu,%d,-1,%d,%d,%d,%d,,,,,,,,,,0,%s\n",
                        positions_done, (unsigned long long)sample_seed,
                        bag_count, rank_new, rank_old, (int)move_get_type(move),
                        move_get_tiles_played(move),
                        string_builder_peek(move_sb));
        }
      }
    }
    (void)fflush(out);
    positions_done++;
    if (positions_done % LATEPOOL_PROGRESS_EVERY == 0) {
      printf("latepool: %ld positions\n", positions_done);
      (void)fflush(stdout);
    }
  }
  printf("latepool: DONE %ld positions to %s\n", positions_done, out_path);
  string_builder_destroy(move_sb);
  (void)fclose(out);
  move_list_destroy(with_term);
  move_list_destroy(without_term);
  config_destroy(config);
}

// Full-game equal-time gate (on-demand test "fullgate").
//
// Two play choosers play game pairs from the opening with the same per-move
// time budget, the same pre-endgame and endgame solvers, and the same tile
// order, roles swapped between the games of a pair. Player A is the test
// configuration; player B is the stock configuration: plain candidate pool,
// no defense term anywhere, 2-ply sims. Each game is scored by result, spread
// and the utility blend, and each player's wall-clock inside its decisions
// is logged.
//
// Environment: FULLGATE_OUT (fullgate.csv), FULLGATE_PAIRS (100000),
// FULLGATE_SECONDS (0 = no time limit on the whole run), FULLGATE_TL_MS (300,
// per-move budget), FULLGATE_THREADS (16), FULLGATE_CANDIDATES (15),
// FULLGATE_SEED (1), FULLGATE_TWD (gs050_105), FULLGATE_PATH, FULLGATE_LEX,
// FULLGATE_WMP, FULLGATE_WINPCT, the utility blend as in lategate
// (FULLGATE_UWIN_PCT 100, FULLGATE_USPREAD_PCT 50, FULLGATE_USCALE 100), and
// A's knobs: FULLGATE_A_ROLLOUT (MAX_PLIES, defense rollout plies),
// FULLGATE_A_ROOT (0, defense term in A's candidate ranking),
// FULLGATE_A_TURNOVER (3), FULLGATE_A_TURNOVER_MIN_BAG (6),
// FULLGATE_A_TURNOVER_MAX_BAG (12), FULLGATE_A_DEEP_MAX_BAG (12),
// FULLGATE_A_DEEP_PLIES (MAX_PLIES).

enum {
  FULLGATE_DEFAULT_PAIRS = 100000,
  FULLGATE_DEFAULT_TL_MS = 300,
  FULLGATE_MAX_MOVES = 120,
  FULLGATE_PROGRESS_EVERY = 10,
};

void test_full_gate(void) {
  const char *out_path = lategate_env_string("FULLGATE_OUT", "fullgate.csv");
  const long num_pairs =
      lategate_env_long("FULLGATE_PAIRS", FULLGATE_DEFAULT_PAIRS);
  const long run_seconds = lategate_env_long("FULLGATE_SECONDS", 0);
  const long tl_ms =
      lategate_env_long("FULLGATE_TL_MS", FULLGATE_DEFAULT_TL_MS);
  const long threads =
      lategate_env_long("FULLGATE_THREADS", LATEGATE_DEFAULT_THREADS);
  const long candidates =
      lategate_env_long("FULLGATE_CANDIDATES", LATEGATE_DEFAULT_CANDIDATES);
  const long base_seed =
      lategate_env_long("FULLGATE_SEED", LATEGATE_DEFAULT_SEED);
  const char *twd_name = lategate_env_string("FULLGATE_TWD", "gs050_105");
  const char *data_path = lategate_env_string("FULLGATE_PATH", "./data");
  const char *lexicon = lategate_env_string("FULLGATE_LEX", "CSW24");
  const char *use_wmp = lategate_env_string("FULLGATE_WMP", "true");
  const char *win_pct_name = lategate_env_string("FULLGATE_WINPCT", "winpct");
  const double utility_w_winpct =
      (double)lategate_env_long("FULLGATE_UWIN_PCT", 100) / 100.0;
  const double utility_w_spread =
      (double)lategate_env_long("FULLGATE_USPREAD_PCT", 50) / 100.0;
  const double utility_spread_scale =
      (double)lategate_env_long("FULLGATE_USCALE", 100);
  const int a_rollout = (int)lategate_env_long("FULLGATE_A_ROLLOUT", MAX_PLIES);
  const bool a_root = lategate_env_long("FULLGATE_A_ROOT", 0) != 0;
  const int a_turnover = (int)lategate_env_long("FULLGATE_A_TURNOVER", 3);
  const int a_turnover_min_bag =
      (int)lategate_env_long("FULLGATE_A_TURNOVER_MIN_BAG", 6);
  const int a_turnover_max_bag =
      (int)lategate_env_long("FULLGATE_A_TURNOVER_MAX_BAG", 12);
  const int a_deep_max_bag =
      (int)lategate_env_long("FULLGATE_A_DEEP_MAX_BAG", 12);
  const int a_deep_plies =
      (int)lategate_env_long("FULLGATE_A_DEEP_PLIES", MAX_PLIES);

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
    log_fatal("fullgate needs a win percentage table");
  }

  const PlayChooserStrategy base_strategy = {
      .pre_endgame_eval = PLAY_CHOOSER_EVAL_PEG,
      .endgame_eval = PLAY_CHOOSER_EVAL_ENDGAME,
      .sim_plies = 2,
      .sim_max_candidates = (int)candidates,
      .fixed_seconds_per_move = (double)tl_ms / 1000.0,
      .win_pcts = win_pcts,
      .num_threads = (int)threads,
      .seed = (uint64_t)base_seed,
      .set_twd_rollout_plies = true,
      .utility_w_winpct = utility_w_winpct,
      .utility_w_spread = utility_w_spread,
      .utility_spread_scale = utility_spread_scale,
  };
  PlayChooserStrategy strategy_a = base_strategy;
  strategy_a.twd_rollout_plies = a_rollout;
  strategy_a.disable_root_twd = !a_root;
  strategy_a.turnover_pool_size = a_turnover;
  strategy_a.turnover_min_bag = a_turnover_min_bag;
  strategy_a.turnover_max_bag = a_turnover_max_bag;
  strategy_a.deep_sim_max_bag = a_deep_max_bag;
  strategy_a.deep_sim_plies = a_deep_plies;
  // B is stock: plain pool, no term in rollouts, 2-ply everywhere.
  PlayChooserStrategy strategy_b = base_strategy;
  strategy_b.twd_rollout_plies = 0;
  strategy_b.disable_root_twd = true;
  PlayChooser *chooser_a = play_chooser_create(&strategy_a);
  PlayChooser *chooser_b = play_chooser_create(&strategy_b);
  ErrorStack *error_stack = error_stack_create();

  FILE *out = fopen_or_die(out_path, "we");
  (void)fprintf(out, "pair,seed,game,a_first,a_score,b_score,a_spread,"
                     "a_utility,moves,a_seconds,b_seconds\n");
  const int64_t run_start_ns = ctimer_monotonic_ns();
  long pairs_done = 0;
  long games_played = 0;
  double a_points = 0.0;
  long a_spread_sum = 0;
  double a_utility_sum = 0.0;
  long pairs_a_both = 0;
  long pairs_split = 0;
  long pairs_b_both = 0;
  int64_t a_total_ns = 0;
  int64_t b_total_ns = 0;
  while (pairs_done < num_pairs) {
    if (run_seconds > 0 && ctimer_monotonic_ns() - run_start_ns >
                               (int64_t)run_seconds * LATEGATE_NS_PER_SECOND) {
      break;
    }
    const uint64_t pair_seed =
        (uint64_t)base_seed * 1000003ULL + (uint64_t)pairs_done + 1;
    double pair_points = 0.0;
    for (int game_idx = 0; game_idx < 2; game_idx++) {
      game_reset(game);
      game_seed(game, pair_seed);
      draw_starting_racks(game);
      const int a_player = game_idx == 0
                               ? game_get_player_on_turn_index(game)
                               : 1 - game_get_player_on_turn_index(game);
      int64_t a_game_ns = 0;
      int64_t b_game_ns = 0;
      int moves = 0;
      while (!game_over(game) && moves < FULLGATE_MAX_MOVES) {
        const bool a_to_move = game_get_player_on_turn_index(game) == a_player;
        Move move;
        const int64_t start_ns = ctimer_monotonic_ns();
        play_chooser_choose_move(a_to_move ? chooser_a : chooser_b, game, &move,
                                 error_stack);
        const int64_t elapsed_ns = ctimer_monotonic_ns() - start_ns;
        if (a_to_move) {
          a_game_ns += elapsed_ns;
        } else {
          b_game_ns += elapsed_ns;
        }
        if (!error_stack_is_empty(error_stack)) {
          error_stack_print_and_reset(error_stack);
          log_fatal("fullgate: play chooser failed in pair %ld", pairs_done);
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
      const double a_utility =
          sim_utility_blend(points, int_to_equity(a_spread), utility_w_winpct,
                            utility_w_spread, utility_spread_scale);
      a_points += points;
      pair_points += points;
      a_spread_sum += a_spread;
      a_utility_sum += a_utility;
      a_total_ns += a_game_ns;
      b_total_ns += b_game_ns;
      games_played++;
      (void)fprintf(out, "%ld,%llu,%d,%d,%d,%d,%d,%.6f,%d,%.3f,%.3f\n",
                    pairs_done, (unsigned long long)pair_seed, game_idx,
                    game_idx == 0 ? 1 : 0, a_score, b_score, a_spread,
                    a_utility, moves,
                    (double)a_game_ns / LATEGATE_NS_PER_SECOND,
                    (double)b_game_ns / LATEGATE_NS_PER_SECOND);
      (void)fflush(out);
    }
    if (pair_points > 1.5) {
      pairs_a_both++;
    } else if (pair_points < 0.5) {
      pairs_b_both++;
    } else {
      pairs_split++;
    }
    pairs_done++;
    if (pairs_done % FULLGATE_PROGRESS_EVERY == 0) {
      printf("fullgate: %ld pairs (%.0fs), A %.2f%% over %ld games, spread "
             "%+.2f, utility edge %+.4f (pairs A/split/B %ld/%ld/%ld), time "
             "A/B %.3f\n",
             pairs_done,
             (double)(ctimer_monotonic_ns() - run_start_ns) /
                 LATEGATE_NS_PER_SECOND,
             100.0 * a_points / (double)games_played, games_played,
             (double)a_spread_sum / (double)games_played,
             a_utility_sum / (double)games_played - 0.5, pairs_a_both,
             pairs_split, pairs_b_both,
             b_total_ns > 0 ? (double)a_total_ns / (double)b_total_ns : 0.0);
      (void)fflush(stdout);
    }
  }
  const double a_win_rate =
      games_played > 0 ? a_points / (double)games_played : 0.5;
  const double sigma =
      games_played > 0
          ? sqrt(a_win_rate * (1.0 - a_win_rate) / (double)games_played)
          : 0.0;
  printf("fullgate: DONE %ld pairs, %ld games: A %.2f%% (+/- %.2f), spread "
         "%+.2f/game, utility %.4f (edge %+.4f), pairs A-both/split/B-both "
         "%ld/%ld/%ld; decision time A %.1fs B %.1fs (A/B %.3f)\n",
         pairs_done, games_played, 100.0 * a_win_rate, 100.0 * sigma,
         games_played > 0 ? (double)a_spread_sum / (double)games_played : 0.0,
         games_played > 0 ? a_utility_sum / (double)games_played : 0.5,
         games_played > 0 ? a_utility_sum / (double)games_played - 0.5 : 0.0,
         pairs_a_both, pairs_split, pairs_b_both,
         (double)a_total_ns / LATEGATE_NS_PER_SECOND,
         (double)b_total_ns / LATEGATE_NS_PER_SECOND,
         b_total_ns > 0 ? (double)a_total_ns / (double)b_total_ns : 0.0);
  (void)fclose(out);
  error_stack_destroy(error_stack);
  play_chooser_destroy(chooser_a);
  play_chooser_destroy(chooser_b);
  config_destroy(config);
}

// Oracle pool collection (on-demand test "oraclepool").
//
// Like latepool, but across the whole game, with a wider union pool (plain
// top ORACLE_TOP, defensive top ORACLE_TOP, and the ORACLE_TURNOVER
// highest-turnover plays), many flat samples per candidate, and the
// simmer's sample log switched on, so every candidate's running estimate is
// written out at a fixed list of sample counts. A pool rule's pick at a
// budget of k samples per candidate is then the candidate with the best
// estimate at checkpoint k, and its regret against the oracle (the full
// sample) can be computed offline for any rule and budget. The sim horizon
// follows the test player: ORACLE_PLIES plies normally, ORACLE_DEEP_PLIES
// while the bag holds at most ORACLE_DEEP_MAX_BAG tiles.
//
// Environment: ORACLE_OUT (oracle.csv), ORACLE_POSITIONS (10000),
// ORACLE_SAMPLES (2000), ORACLE_TOP (30), ORACLE_TURNOVER (5), ORACLE_PLIES
// (2), ORACLE_DEEP_MAX_BAG (12), ORACLE_DEEP_PLIES (MAX_PLIES),
// ORACLE_ROLLOUT (MAX_PLIES), ORACLE_MIN_BAG (5), ORACLE_MAX_BAG (86),
// ORACLE_MAX_TURNS (24), ORACLE_THREADS (16), ORACLE_SEED (1),
// ORACLE_SIM_SEED (7), ORACLE_TWD, ORACLE_PATH, ORACLE_LEX, ORACLE_WMP,
// ORACLE_WINPCT, ORACLE_UWIN_PCT (100), ORACLE_USPREAD_PCT (50),
// ORACLE_USCALE (100).

enum {
  ORACLE_DEFAULT_POSITIONS = 10000,
  ORACLE_DEFAULT_SAMPLES = 2000,
  ORACLE_DEFAULT_TOP = 30,
  ORACLE_DEFAULT_TURNOVER = 5,
  ORACLE_SCAN_CAPACITY = 60,
  ORACLE_NUM_CHECKPOINTS = 15,
};

static const int ORACLE_CHECKPOINTS[ORACLE_NUM_CHECKPOINTS] = {
    10, 20, 30, 50, 75, 100, 150, 200, 300, 400, 500, 750, 1000, 1500, 2000};

void test_oracle_pool(void) {
  const char *out_path = lategate_env_string("ORACLE_OUT", "oracle.csv");
  const long num_positions =
      lategate_env_long("ORACLE_POSITIONS", ORACLE_DEFAULT_POSITIONS);
  const long samples =
      lategate_env_long("ORACLE_SAMPLES", ORACLE_DEFAULT_SAMPLES);
  const long top = lategate_env_long("ORACLE_TOP", ORACLE_DEFAULT_TOP);
  const long turnover =
      lategate_env_long("ORACLE_TURNOVER", ORACLE_DEFAULT_TURNOVER);
  const long plies = lategate_env_long("ORACLE_PLIES", 2);
  const int deep_max_bag = (int)lategate_env_long("ORACLE_DEEP_MAX_BAG", 12);
  const long deep_plies = lategate_env_long("ORACLE_DEEP_PLIES", MAX_PLIES);
  const long rollout = lategate_env_long("ORACLE_ROLLOUT", MAX_PLIES);
  const int min_bag = (int)lategate_env_long("ORACLE_MIN_BAG", 5);
  const int max_bag = (int)lategate_env_long("ORACLE_MAX_BAG", 86);
  const int max_turns = (int)lategate_env_long("ORACLE_MAX_TURNS", 24);
  const long threads =
      lategate_env_long("ORACLE_THREADS", LATEGATE_DEFAULT_THREADS);
  const long base_seed =
      lategate_env_long("ORACLE_SEED", LATEGATE_DEFAULT_SEED);
  const long sim_seed = lategate_env_long("ORACLE_SIM_SEED", 7);
  const char *twd_name = lategate_env_string("ORACLE_TWD", "gs050_105");
  const char *data_path = lategate_env_string("ORACLE_PATH", "./data");
  const char *lexicon = lategate_env_string("ORACLE_LEX", "CSW24");
  const char *use_wmp = lategate_env_string("ORACLE_WMP", "true");
  const char *win_pct_name = lategate_env_string("ORACLE_WINPCT", "winpct");
  const double utility_w_winpct =
      (double)lategate_env_long("ORACLE_UWIN_PCT", 100) / 100.0;
  const double utility_w_spread =
      (double)lategate_env_long("ORACLE_USPREAD_PCT", 50) / 100.0;
  const double utility_spread_scale =
      (double)lategate_env_long("ORACLE_USCALE", 100);

  char cmd[LATEGATE_CMD_SIZE];
  snprintf(cmd, sizeof(cmd),
           "set -lex %s -wmp %s -s1 equity -s2 equity -r1 all -r2 all "
           "-numplays %ld -plies %ld -threads %ld -iter %ld -minp %ld -sr rr "
           "-threshold none -scond none -cutoff 0 -seed %ld -savesettings "
           "false -path %s -twd %s -winpct %s -twdrollout %ld",
           lexicon, use_wmp, 2 * top + turnover, plies, threads,
           (2 * top + turnover) * samples, samples, sim_seed, data_path,
           twd_name, win_pct_name, rollout);
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
  const LetterDistribution *ld = config_get_ld(config);
  MoveList *pool = NULL;
  SimResults *sim_results = config_get_sim_results(config);
  sim_results_set_sample_log_capacity(sim_results, (int)samples);
  MoveList *with_term = move_list_create((int)top);
  MoveList *plain_scan = move_list_create(ORACLE_SCAN_CAPACITY);

  FILE *out = fopen_or_die(out_path, "we");
  (void)fprintf(out, "pos,seed,bag,plies,cand,rank_new,rank_old,rank_tiles,"
                     "type,tiles_played,score,leave,twd,equity_old,equity_new,"
                     "samples");
  for (int cp = 0; cp < ORACLE_NUM_CHECKPOINTS; cp++) {
    (void)fprintf(out, ",win_%d,spread_%d,util_%d", ORACLE_CHECKPOINTS[cp],
                  ORACLE_CHECKPOINTS[cp], ORACLE_CHECKPOINTS[cp]);
  }
  (void)fprintf(out, ",move\n");
  StringBuilder *move_sb = string_builder_create();
  long positions_done = 0;
  uint64_t sample_seed = (uint64_t)base_seed * 1000003ULL;
  while (positions_done < num_positions) {
    sample_seed++;
    if (!latepool_sample_random_turn(game, sample_seed, max_turns, min_bag,
                                     max_bag)) {
      continue;
    }
    if (pool == NULL) {
      load_and_exec_config_or_die(config, "gen");
      pool = config_get_move_list(config);
    }
    const int player_index = game_get_player_on_turn_index(game);
    const Player *player = game_get_player(game, player_index);
    const KLV *klv = player_get_klv(player);
    const Board *board = game_get_board(game);
    const int bag_count = bag_get_letters(game_get_bag(game));

    latepool_generate(game, with_term, false);
    latepool_generate(game, plain_scan, true);
    move_list_reset(pool);
    for (int move_idx = 0; move_idx < move_list_get_count(with_term);
         move_idx++) {
      move_list_add_move(pool, move_list_get_move(with_term, move_idx));
    }
    for (int move_idx = 0;
         move_idx < move_list_get_count(plain_scan) && move_idx < top;
         move_idx++) {
      const Move *move = move_list_get_move(plain_scan, move_idx);
      if (latepool_rank(with_term, move) == 0) {
        move_list_add_move(pool, move);
      }
    }
    // The highest-turnover plays, best equity first within a tile count.
    int turnover_added = 0;
    for (int tiles = RACK_SIZE; tiles >= 1 && turnover_added < turnover;
         tiles--) {
      for (int scan_idx = 0; scan_idx < move_list_get_count(plain_scan) &&
                             turnover_added < turnover;
           scan_idx++) {
        const Move *move = move_list_get_move(plain_scan, scan_idx);
        if (move_get_tiles_played(move) != tiles ||
            move_get_type(move) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
          continue;
        }
        bool in_pool = false;
        for (int pool_idx = 0; pool_idx < move_list_get_count(pool);
             pool_idx++) {
          if (compare_moves_without_equity(move_list_get_move(pool, pool_idx),
                                           move, true) == -1) {
            in_pool = true;
            break;
          }
        }
        if (!in_pool) {
          move_list_add_move(pool, move);
          turnover_added++;
        }
      }
    }
    const int pool_size = move_list_get_count(pool);
    if (pool_size < 2) {
      continue;
    }
    const long position_plies = bag_count <= deep_max_bag ? deep_plies : plies;
    snprintf(cmd, sizeof(cmd), "set -iter %ld -plies %ld",
             (long)pool_size * samples, position_plies);
    load_and_exec_config_or_die(config, cmd);
    const error_code_t status =
        config_simulate_and_return_status(config, NULL, NULL, sim_results);
    if (status != ERROR_STATUS_SUCCESS) {
      log_fatal("oraclepool: simulation failed with status %d", status);
    }
    const int num_plays = sim_results_get_number_of_plays(sim_results);

    TWDEvalContext twd_eval_ctx;
    twd_eval_context_disable(&twd_eval_ctx);
    const TWDWeights *twd = player_get_twd(player);
    if (twd != NULL) {
      twd_eval_context_load(
          &twd_eval_ctx, twd,
          board_get_readonly_lanes(
              board, board_get_cross_set_index(
                         game_get_data_is_shared(game, PLAYERS_DATA_TYPE_KWG),
                         player_index)),
          ld, player_get_rack(player));
    }
    for (int play_idx = 0; play_idx < num_plays; play_idx++) {
      const SimmedPlay *simmed_play =
          sim_results_get_simmed_play(sim_results, play_idx);
      const Move *move = simmed_play_get_move(simmed_play);
      const int rank_new = latepool_rank(with_term, move);
      const int rank_old = latepool_rank(plain_scan, move);
      // Rank among the turnover picks: 1-based by tiles then equity, 0 if
      // the move was not one of them.
      int rank_tiles = 0;
      {
        int seen = 0;
        for (int tiles = RACK_SIZE; tiles >= 1 && rank_tiles == 0; tiles--) {
          for (int scan_idx = 0;
               scan_idx < move_list_get_count(plain_scan) && rank_tiles == 0;
               scan_idx++) {
            const Move *scan_move = move_list_get_move(plain_scan, scan_idx);
            if (move_get_tiles_played(scan_move) != tiles ||
                move_get_type(scan_move) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
              continue;
            }
            seen++;
            if (compare_moves_without_equity(scan_move, move, true) == -1) {
              rank_tiles = seen;
            }
          }
        }
      }
      Rack leave_rack;
      rack_copy(&leave_rack, player_get_rack(player));
      const Equity leave_value =
          get_leave_value_for_move(klv, move, &leave_rack);
      const Equity twd_penalty = twd_eval_move_penalty(&twd_eval_ctx, move);
      const Equity equity_old =
          rank_old > 0
              ? move_get_equity(move_list_get_move(plain_scan, rank_old - 1))
              : move_get_equity(move_list_get_move(with_term, rank_new - 1)) -
                    twd_penalty;
      const Equity equity_new = equity_old + twd_penalty;
      const int num_logged = simmed_play_get_num_logged_samples(simmed_play);
      string_builder_clear(move_sb);
      string_builder_add_move(move_sb, board, move, ld, false);
      (void)fprintf(out,
                    "%ld,%llu,%d,%ld,%d,%d,%d,%d,%d,%d,%.3f,%.3f,%.3f,"
                    "%.3f,%.3f,%d",
                    positions_done, (unsigned long long)sample_seed, bag_count,
                    position_plies, play_idx, rank_new, rank_old, rank_tiles,
                    (int)move_get_type(move), move_get_tiles_played(move),
                    equity_to_double(move_get_score(move)),
                    equity_to_double(leave_value),
                    equity_to_double(twd_penalty), equity_to_double(equity_old),
                    equity_to_double(equity_new), num_logged);
      // Running means at each checkpoint, in seed order.
      double win_sum = 0.0;
      double spread_sum = 0.0;
      double util_sum = 0.0;
      int next_cp = 0;
      for (int sample_idx = 0;
           sample_idx < num_logged && next_cp < ORACLE_NUM_CHECKPOINTS;
           sample_idx++) {
        const double win =
            simmed_play_get_logged_win_pct(simmed_play, sample_idx);
        const double spread =
            simmed_play_get_logged_equity(simmed_play, sample_idx);
        win_sum += win;
        spread_sum += spread;
        util_sum +=
            sim_utility_blend(win, double_to_equity(spread), utility_w_winpct,
                              utility_w_spread, utility_spread_scale);
        const int count = sample_idx + 1;
        while (next_cp < ORACLE_NUM_CHECKPOINTS &&
               count == ORACLE_CHECKPOINTS[next_cp]) {
          (void)fprintf(out, ",%.6f,%.3f,%.6f", win_sum / count,
                        spread_sum / count, util_sum / count);
          next_cp++;
        }
      }
      for (; next_cp < ORACLE_NUM_CHECKPOINTS; next_cp++) {
        (void)fprintf(out, ",,,");
      }
      (void)fprintf(out, ",%s\n", string_builder_peek(move_sb));
    }
    (void)fflush(out);
    positions_done++;
    if (positions_done % LATEPOOL_PROGRESS_EVERY == 0) {
      printf("oraclepool: %ld positions\n", positions_done);
      (void)fflush(stdout);
    }
  }
  printf("oraclepool: DONE %ld positions to %s\n", positions_done, out_path);
  string_builder_destroy(move_sb);
  (void)fclose(out);
  move_list_destroy(with_term);
  move_list_destroy(plain_scan);
  config_destroy(config);
}
