// Throughput benchmark for MOVE_RECORD_BINGO_EXISTS. Draws fresh racks
// from a fixed mid-game position and times generate_moves over many
// iterations. Compares the RIT inline-bingo fast path against the WMP
// subrack enumeration fallback.
//
// Usage: ./bin/magpie_test bingosbench
// Env vars:
//   BBOB_RIT    "true" / "false" — RIT on or off (default true)
//   BBOB_ITERS  iteration count (default 200000)
//   BBOB_SEED   PRNG seed for the bag (default 42)
//   BBOB_CGP    "midgame" / "empty" — position fixture (default midgame)

#include "bingo_exists_test.h"

#include "../src/def/equity_defs.h"
#include "../src/def/game_history_defs.h"
#include "../src/def/move_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/rack.h"
#include "../src/impl/cgp.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"
#include "../src/str/move_string.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// A real mid-game position taken from the existing simbench corpus.
static const char *MIDGAME_CGP =
    "WAgTAILS5K1/7P4QI1/3NEUTRINO1UN1/JOTA3I3BEDU/I2EFF1N2ZAS1N/"
    "HM5G2AGO1R/AE4REP2G2E/D6SOOTY1TA/ID11EL/1AR10L1/1WO7UNBOX/"
    "1TE10M1/1E11I1/1D10ICY/15 AILORRV/ 303/458 0";

// Empty-board opening: anchors only at the center; very different
// shadow/anchor topology from a mid-game position.
static const char *EMPTY_CGP =
    "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 AILORRV/ 0/0 0";

void test_bingo_exists_bench(void) {
  const char *rit_env = getenv("BBOB_RIT");
  const char *rit = (rit_env != NULL) ? rit_env : "true";
  const char *iters_env = getenv("BBOB_ITERS");
  const int iters =
      (iters_env != NULL) ? (int)strtol(iters_env, NULL, 10) : 200000;
  const char *seed_env = getenv("BBOB_SEED");
  const uint64_t seed =
      (seed_env != NULL) ? (uint64_t)strtoull(seed_env, NULL, 10) : 42ULL;
  const char *cgp_env = getenv("BBOB_CGP");
  const char *cgp_label = (cgp_env != NULL) ? cgp_env : "midgame";
  const char *cgp_string = (cgp_env != NULL && strcmp(cgp_env, "empty") == 0)
                               ? EMPTY_CGP
                               : MIDGAME_CGP;

  char cmd[256];
  (void)snprintf(cmd, sizeof(cmd),
                 "set -lex CSW21 -wmp true -rit %s -s1 score -s2 score "
                 "-r1 all -r2 all -numplays 100",
                 rit);
  Config *config = config_create_or_die(cmd);
  Game *game = config_game_create(config);
  load_cgp_or_die(game, cgp_string);

  // Seed the bag's PRNG so rack draws are deterministic and identical
  // across the rit=true / rit=false runs.
  bag_seed(game_get_bag(game), seed);

  MoveList *move_list = move_list_create(1);

  const MoveGenArgs args = {
      .game = game,
      .move_list = move_list,
      .move_record_type = MOVE_RECORD_BINGO_EXISTS,
      .move_sort_type = MOVE_SORT_SCORE,
      .thread_index = 0,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
  };

  // Warmup: get the per-thread caches into a representative state.
  return_rack_to_bag(game, 0);
  draw_to_full_rack(game, 0);
  (void)bingo_exists(&args);

  struct timespec start;
  struct timespec end;
  clock_gettime(CLOCK_MONOTONIC, &start); // NOLINT(misc-include-cleaner)

  uint64_t racks_with_bingo = 0;
  for (int i = 0; i < iters; i++) {
    return_rack_to_bag(game, 0);
    draw_to_full_rack(game, 0);
    if (bingo_exists(&args)) {
      racks_with_bingo++;
    }
  }

  clock_gettime(CLOCK_MONOTONIC, &end);
  const double elapsed = (double)(end.tv_sec - start.tv_sec) +
                         (double)(end.tv_nsec - start.tv_nsec) / 1e9;
  const double calls_per_sec = (double)iters / elapsed;
  const double bingo_rate = (double)racks_with_bingo / (double)iters;

  printf("cgp=%s rit=%s iters=%d: %.3fs  %.0f calls/sec  bingo_rate=%.3f\n",
         cgp_label, rit, iters, elapsed, calls_per_sec, bingo_rate);

  move_list_destroy(move_list);
  game_destroy(game);
  config_destroy(config);
}

// Correctness check: for many random racks at a fixed mid-game position,
// generate via MOVE_RECORD_BINGO_EXISTS and via MOVE_RECORD_ALL (filtering
// for full-rack tile placements). The per-rack bingo counts must agree
// exactly. Aborts on the first divergence (after logging up to 5).
//
// Usage: ./bin/magpie_test bingoscorrect
// Env vars:
//   BBOC_ITERS  iteration count (default 5000)
//   BBOC_SEED   bag PRNG seed (default 42)
//   BBOC_CGP    "midgame" / "empty" (default midgame)
void test_bingo_exists_correctness(void) {
  const char *iters_env = getenv("BBOC_ITERS");
  const int iters =
      (iters_env != NULL) ? (int)strtol(iters_env, NULL, 10) : 5000;
  const char *seed_env = getenv("BBOC_SEED");
  const uint64_t seed =
      (seed_env != NULL) ? (uint64_t)strtoull(seed_env, NULL, 10) : 42ULL;
  const char *cgp_env = getenv("BBOC_CGP");
  const char *cgp_label = (cgp_env != NULL) ? cgp_env : "midgame";
  const char *cgp_string = (cgp_env != NULL && strcmp(cgp_env, "empty") == 0)
                               ? EMPTY_CGP
                               : MIDGAME_CGP;

  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp true -rit true -s1 score -s2 score "
      "-r1 all -r2 all -numplays 5000");
  Game *game = config_game_create(config);
  load_cgp_or_die(game, cgp_string);
  bag_seed(game_get_bag(game), seed);

  MoveList *bingo_exists_list = move_list_create(1);
  MoveList *all_list = move_list_create(10000);

  const MoveGenArgs bingo_exists_args = {
      .game = game,
      .move_list = bingo_exists_list,
      .move_record_type = MOVE_RECORD_BINGO_EXISTS,
      .move_sort_type = MOVE_SORT_SCORE,
      .thread_index = 0,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
  };
  const MoveGenArgs all_args = {
      .game = game,
      .move_list = all_list,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_SCORE,
      .thread_index = 0,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
  };

  int divergences = 0;
  uint64_t bingo_exists_count = 0;
  uint64_t total_all_bingos = 0;
  for (int i = 0; i < iters; i++) {
    return_rack_to_bag(game, 0);
    draw_to_full_rack(game, 0);

    const bool bingo_exists_says_yes = bingo_exists(&bingo_exists_args);

    generate_moves(&all_args);
    bool all_says_yes = false;
    int all_bingo_count = 0;
    const int all_count = move_list_get_count(all_list);
    for (int j = 0; j < all_count; j++) {
      const Move *m = move_list_get_move(all_list, j);
      if (move_get_type(m) == GAME_EVENT_TILE_PLACEMENT_MOVE &&
          move_get_tiles_played(m) == RACK_SIZE) {
        all_says_yes = true;
        all_bingo_count++;
      }
    }

    if (bingo_exists_says_yes) {
      bingo_exists_count++;
    }
    total_all_bingos += (uint64_t)all_bingo_count;

    if (bingo_exists_says_yes != all_says_yes) {
      divergences++;
      if (divergences <= 5) {
        printf("DIVERGENCE iter=%d: BINGO_EXISTS=%s, ALL_filtered_count=%d\n",
               i, bingo_exists_says_yes ? "yes" : "no", all_bingo_count);
      }
    }
  }

  printf("correctness cgp=%s iters=%d: bingo_exists_yes=%llu "
         "all_bingos_total_plays=%llu divergences=%d\n",
         cgp_label, iters, (unsigned long long)bingo_exists_count,
         (unsigned long long)total_all_bingos, divergences);
  assert(divergences == 0);

  move_list_destroy(bingo_exists_list);
  move_list_destroy(all_list);
  game_destroy(game);
  config_destroy(config);
}

// Plays out N games using static-eval move selection (s1=score, r1=best
// over the equity-sorted full movegen), and at every position also runs
// bingo_exists. Reports total time spent in each so the per-position
// cost ratio is visible. Hypothesis: bingo_exists is much faster than
// the full static-eval movegen because it skips non-bingo anchors and
// short-circuits on the first valid placement.
//
// Usage: ./bin/magpie_test bingosvsstatic
// Env vars:
//   BVS_GAMES   number of games to play (default 200)
//   BVS_SEED    bag PRNG seed (default 42)
//   BVS_RIT     "true" / "false" (default true)
void test_bingo_exists_vs_static_bench(void) {
  const char *games_env = getenv("BVS_GAMES");
  const int games =
      (games_env != NULL) ? (int)strtol(games_env, NULL, 10) : 200;
  const char *seed_env = getenv("BVS_SEED");
  const uint64_t seed =
      (seed_env != NULL) ? (uint64_t)strtoull(seed_env, NULL, 10) : 42ULL;
  const char *rit_env = getenv("BVS_RIT");
  const char *rit = (rit_env != NULL) ? rit_env : "true";

  char cmd[256];
  (void)snprintf(cmd, sizeof(cmd),
                 "set -lex CSW21 -wmp true -rit %s -s1 equity -s2 equity "
                 "-r1 best -r2 best -numplays 1",
                 rit);
  Config *config = config_create_or_die(cmd);
  Game *game = config_game_create(config);

  MoveList *move_list = move_list_create(1);
  Rack leave;

  const MoveGenArgs args = {
      .game = game,
      .move_list = move_list,
      .move_record_type = MOVE_RECORD_BEST,
      .move_sort_type = MOVE_SORT_EQUITY,
      .thread_index = 0,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
  };

  uint64_t total_static_ns = 0;
  uint64_t total_bingo_ns = 0;
  uint64_t total_positions = 0;
  uint64_t bingo_yes_count = 0;

  for (int g = 0; g < games; g++) {
    // Seed per game so each game is independent and the same trajectory
    // is reproduced across rit=true / rit=false runs (game_reset does not
    // reseed the bag PRNG, so without per-game seeding any RIT-induced
    // tiebreak difference would cascade into every later game).
    bag_seed(game_get_bag(game), seed + (uint64_t)g);
    game_reset(game);
    draw_to_full_rack(game, 0);
    draw_to_full_rack(game, 1);

    while (!game_over(game)) {
      const int player_idx = game_get_player_on_turn_index(game);
      rack_set_dist_size(&leave, ld_get_size(game_get_ld(game)));

      // Time the full static-eval movegen (best move via equity sort).
      struct timespec t0;
      struct timespec t1;
      clock_gettime(CLOCK_MONOTONIC, &t0); // NOLINT(misc-include-cleaner)
      generate_moves(&args);
      clock_gettime(CLOCK_MONOTONIC, &t1);
      total_static_ns += (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000000ULL +
                         (uint64_t)(t1.tv_nsec - t0.tv_nsec);

      const Move *best_move = move_list_get_move(move_list, 0);

      // Time bingo_exists on the same position.
      clock_gettime(CLOCK_MONOTONIC, &t0);
      const bool found = bingo_exists(&args);
      clock_gettime(CLOCK_MONOTONIC, &t1);
      total_bingo_ns += (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000000ULL +
                        (uint64_t)(t1.tv_nsec - t0.tv_nsec);
      if (found) {
        bingo_yes_count++;
      }
      total_positions++;

      play_move(best_move, game, &leave);
      (void)player_idx;
    }
  }

  const double static_ms = (double)total_static_ns / 1e6;
  const double bingo_ms = (double)total_bingo_ns / 1e6;
  const double static_us_per_pos =
      (double)total_static_ns / (double)total_positions / 1000.0;
  const double bingo_us_per_pos =
      (double)total_bingo_ns / (double)total_positions / 1000.0;
  const double ratio = (double)total_static_ns / (double)total_bingo_ns;

  printf("rit=%s games=%d positions=%llu bingo_yes=%llu (%.1f%%)\n", rit, games,
         (unsigned long long)total_positions,
         (unsigned long long)bingo_yes_count,
         100.0 * (double)bingo_yes_count / (double)total_positions);
  printf("  static_eval: total=%.1fms  per_position=%.2fus\n", static_ms,
         static_us_per_pos);
  printf("  bingo_exists: total=%.1fms  per_position=%.2fus\n", bingo_ms,
         bingo_us_per_pos);
  printf("  static / bingo time ratio: %.2fx\n", ratio);

  move_list_destroy(move_list);
  game_destroy(game);
  config_destroy(config);
}

// Plays out N games using static-eval move selection, and at every
// position runs both bingo_exists (exact) and bingo_exists_approx
// (skips word_length > 8 anchors). Reports the time spent in each and
// the count/rate of "misses" — positions where exact says yes but
// approx says no (i.e., a real multi-playthrough bingo the approx
// mode dropped). Also asserts approx never returns yes when exact
// says no (which would be a bug).
//
// Usage: ./bin/magpie_test bingosapprox
// Env vars:
//   BAM_GAMES   number of games (default 200)
//   BAM_SEED    bag PRNG seed (default 42)
//   BAM_RIT     "true" / "false" (default true)
void test_bingo_exists_approx_miss_rate(void) {
  const char *games_env = getenv("BAM_GAMES");
  const int games =
      (games_env != NULL) ? (int)strtol(games_env, NULL, 10) : 200;
  const char *seed_env = getenv("BAM_SEED");
  const uint64_t seed =
      (seed_env != NULL) ? (uint64_t)strtoull(seed_env, NULL, 10) : 42ULL;
  const char *rit_env = getenv("BAM_RIT");
  const char *rit = (rit_env != NULL) ? rit_env : "true";

  char cmd[256];
  (void)snprintf(cmd, sizeof(cmd),
                 "set -lex CSW21 -wmp true -rit %s -s1 equity -s2 equity "
                 "-r1 best -r2 best -numplays 1",
                 rit);
  Config *config = config_create_or_die(cmd);
  Game *game = config_game_create(config);

  MoveList *move_list = move_list_create(1);
  Rack leave;

  const MoveGenArgs args = {
      .game = game,
      .move_list = move_list,
      .move_record_type = MOVE_RECORD_BEST,
      .move_sort_type = MOVE_SORT_EQUITY,
      .thread_index = 0,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
  };

  uint64_t total_exact_ns = 0;
  uint64_t total_approx_ns = 0;
  uint64_t total_positions = 0;
  uint64_t exact_yes = 0;
  uint64_t approx_yes = 0;
  uint64_t misses = 0;          // exact=yes, approx=no
  uint64_t false_positives = 0; // approx=yes, exact=no (should always be 0)

  for (int g = 0; g < games; g++) {
    bag_seed(game_get_bag(game), seed + (uint64_t)g);
    game_reset(game);
    draw_to_full_rack(game, 0);
    draw_to_full_rack(game, 1);

    while (!game_over(game)) {
      rack_set_dist_size(&leave, ld_get_size(game_get_ld(game)));

      // Need the chosen move for the playthrough; use static eval.
      generate_moves(&args);
      const Move *best_move = move_list_get_move(move_list, 0);

      struct timespec t0;
      struct timespec t1;
      clock_gettime(CLOCK_MONOTONIC, &t0); // NOLINT(misc-include-cleaner)
      const bool exact = bingo_exists(&args);
      clock_gettime(CLOCK_MONOTONIC, &t1);
      total_exact_ns += (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000000ULL +
                        (uint64_t)(t1.tv_nsec - t0.tv_nsec);

      clock_gettime(CLOCK_MONOTONIC, &t0);
      const bool approx = bingo_exists_approx(&args);
      clock_gettime(CLOCK_MONOTONIC, &t1);
      total_approx_ns += (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000000ULL +
                         (uint64_t)(t1.tv_nsec - t0.tv_nsec);

      if (exact) {
        exact_yes++;
      }
      if (approx) {
        approx_yes++;
      }
      if (exact && !approx) {
        misses++;
      }
      if (!exact && approx) {
        false_positives++;
      }
      total_positions++;

      play_move(best_move, game, &leave);
    }
  }

  const double exact_us =
      (double)total_exact_ns / (double)total_positions / 1000.0;
  const double approx_us =
      (double)total_approx_ns / (double)total_positions / 1000.0;
  const double speedup = (double)total_exact_ns / (double)total_approx_ns;
  const double miss_rate_overall =
      100.0 * (double)misses / (double)total_positions;
  const double miss_rate_of_yes =
      exact_yes > 0 ? 100.0 * (double)misses / (double)exact_yes : 0.0;

  printf("rit=%s games=%d positions=%llu\n", rit, games,
         (unsigned long long)total_positions);
  printf("  exact:  yes=%llu (%.1f%%) per_pos=%.2fus\n",
         (unsigned long long)exact_yes,
         100.0 * (double)exact_yes / (double)total_positions, exact_us);
  printf("  approx: yes=%llu (%.1f%%) per_pos=%.2fus\n",
         (unsigned long long)approx_yes,
         100.0 * (double)approx_yes / (double)total_positions, approx_us);
  printf("  misses (exact=yes, approx=no): %llu (%.2f%% of all positions, "
         "%.2f%% of exact-yes positions)\n",
         (unsigned long long)misses, miss_rate_overall, miss_rate_of_yes);
  printf("  false_positives (approx=yes, exact=no): %llu (must be 0)\n",
         (unsigned long long)false_positives);
  printf("  exact / approx time ratio: %.2fx\n", speedup);
  assert(false_positives == 0);

  move_list_destroy(move_list);
  game_destroy(game);
  config_destroy(config);
}
