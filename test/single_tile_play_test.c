// Tests for single_tile_play (scan + features).
//
// Two correctness checks:
// 1. The per-letter best_score from single_tile_scan agrees with what
//    MAGPIE's existing move generator finds when run with a one-letter
//    rack (filtered to single-tile plays).
// 2. The per-rack features match a brute-force enumeration over all
//    possible draws for a small pool, verifying the order-statistic
//    formulas (E[max], E[2nd_max]) and the linearity formula for
//    fraction playable.

#include "single_tile_play_test.h"

#include "../src/def/equity_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/move_defs.h"
#include "../src/def/rack_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"
#include "../src/impl/single_tile_play.h"
#include "test_util.h"
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// A real mid-game position with plenty of tiles on the board, so most
// letters have multiple potential single-tile plays with different scores.
static const char *MIDGAME_CGP =
    "WAgTAILS5K1/7P4QI1/3NEUTRINO1UN1/JOTA3I3BEDU/I2EFF1N2ZAS1N/"
    "HM5G2AGO1R/AE4REP2G2E/D6SOOTY1TA/ID11EL/1AR10L1/1WO7UNBOX/"
    "1TE10M1/1E11I1/1D10ICY/15 AILORRV/ 303/458 0";

static void compute_brute_force_per_letter_max(Game *game,
                                               Equity *expected_best) {
  const LetterDistribution *ld = game_get_ld(game);
  const int ld_size = ld_get_size(ld);
  const int player_idx = game_get_player_on_turn_index(game);
  Player *player = game_get_player(game, player_idx);
  Rack *player_rack = player_get_rack(player);
  MoveList *move_list = move_list_create(1024);

  for (int ml = 0; ml < ld_size; ml++) {
    expected_best[ml] = 0;
    rack_reset(player_rack);
    rack_set_dist_size(player_rack, ld_size);
    rack_add_letter(player_rack, (MachineLetter)ml);

    const MoveGenArgs args = {
        .game = game,
        .move_list = move_list,
        .move_record_type = MOVE_RECORD_ALL,
        .move_sort_type = MOVE_SORT_SCORE,
        .thread_index = 0,
        .eq_margin_movegen = 0,
        .target_equity = EQUITY_MAX_VALUE,
        .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
    };
    generate_moves(&args);

    const int n = move_list_get_count(move_list);
    for (int i = 0; i < n; i++) {
      const Move *m = move_list_get_move(move_list, i);
      if (move_get_tiles_played(m) != 1) {
        continue;
      }
      const Equity score = move_get_score(m);
      if (score > expected_best[ml]) {
        expected_best[ml] = score;
      }
    }
  }
  move_list_destroy(move_list);
}

static void test_scan_matches_movegen(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 1");
  Game *game = config_game_create(config);
  load_cgp_or_die(game, MIDGAME_CGP);

  SingleTileScan scan;
  single_tile_scan(game, &scan);

  Equity expected[MAX_ALPHABET_SIZE] = {0};
  compute_brute_force_per_letter_max(game, expected);

  const LetterDistribution *ld = game_get_ld(game);
  const int ld_size = ld_get_size(ld);
  for (int ml = 0; ml < ld_size; ml++) {
    if (scan.best_score[ml] != expected[ml]) {
      printf("ml=%d expected=%d scan=%d\n", ml, equity_to_int(expected[ml]),
             equity_to_int(scan.best_score[ml]));
    }
    assert(scan.best_score[ml] == expected[ml]);

    // playable_set bit must be set iff best_score > 0 OR (blank-only case).
    // For non-blank ml, best_score > 0 iff playable somewhere with non-zero
    // face value. Blank may have best_score == 0 even when playable (if
    // it can only play on a square with no scoring contribution), so handle
    // it loosely.
    if (ml != 0 && expected[ml] > 0) {
      assert(((scan.playable_set >> ml) & 1) != 0);
    }
  }

  game_destroy(game);
  config_destroy(config);
}

// Brute-force enumeration of expected max/2nd-max/frac for a known
// leave + small pool + small draw. Iterates every multiset draw, weights
// by multinomial counts.
typedef struct {
  double e_max;
  double e_2nd_max;
  double e_frac;
} BruteFeats;

static uint64_t binom(int n, int k) {
  if (k < 0 || k > n) {
    return 0;
  }
  if (k > n - k) {
    k = n - k;
  }
  uint64_t r = 1;
  for (int i = 0; i < k; i++) {
    r = r * (uint64_t)(n - i) / (uint64_t)(i + 1);
  }
  return r;
}

static void brute_recurse(const SingleTileScan *scan,
                          const uint8_t *leave_counts,
                          const uint8_t *pool_counts, int draw_size,
                          uint8_t *current_draw, int ml, uint64_t weight,
                          int rack_size, int ld_size, double total_denom,
                          BruteFeats *out, double *check_total_weight) {
  if (draw_size == 0) {
    // Compute features for rack = leave + current_draw
    int sorted[64];
    int n_sorted = 0;
    int playable_in_rack = 0;
    for (int j = 0; j < ld_size; j++) {
      const int n = leave_counts[j] + current_draw[j];
      for (int k = 0; k < n; k++) {
        sorted[n_sorted++] = (int)scan->best_score[j];
        if (((scan->playable_set >> j) & 1) != 0) {
          playable_in_rack++;
        }
      }
    }
    // Insertion sort descending
    for (int i = 1; i < n_sorted; i++) {
      int tmp = sorted[i];
      int j = i - 1;
      while (j >= 0 && sorted[j] < tmp) {
        sorted[j + 1] = sorted[j];
        j--;
      }
      sorted[j + 1] = tmp;
    }
    const double w = (double)weight / total_denom;
    out->e_max += w * (double)sorted[0];
    out->e_2nd_max += w * (n_sorted >= 2 ? (double)sorted[1] : 0.0);
    out->e_frac += w * ((double)playable_in_rack / (double)rack_size);
    *check_total_weight += w;
    return;
  }
  if (ml >= ld_size) {
    return;
  }
  const int max_take =
      (pool_counts[ml] < draw_size) ? pool_counts[ml] : draw_size;
  for (int take = 0; take <= max_take; take++) {
    current_draw[ml] = (uint8_t)take;
    const uint64_t w = weight * binom(pool_counts[ml], take);
    brute_recurse(scan, leave_counts, pool_counts, draw_size - take,
                  current_draw, ml + 1, w, rack_size, ld_size, total_denom, out,
                  check_total_weight);
  }
  current_draw[ml] = 0;
}

static void test_features_against_enumeration(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 1");
  Game *game = config_game_create(config);
  load_cgp_or_die(game, MIDGAME_CGP);

  SingleTileScan scan;
  single_tile_scan(game, &scan);

  const LetterDistribution *ld = game_get_ld(game);
  const int ld_size = ld_get_size(ld);

  // Construct a small pool with diverse letters (so different scores) and
  // a fixed leave with 3 tiles. Draw 4 from a pool of ~12 tiles -> small
  // enough for full enumeration.
  uint8_t leave_counts[MAX_ALPHABET_SIZE] = {0};
  uint8_t pool_counts[MAX_ALPHABET_SIZE] = {0};
  // Use the actual on-turn rack as the leave (first 3 tiles).
  const Player *player =
      game_get_player(game, game_get_player_on_turn_index(game));
  const Rack *real_rack = player_get_rack(player);
  int placed = 0;
  for (int ml = 0; ml < ld_size && placed < 3; ml++) {
    const int c = rack_get_letter(real_rack, (MachineLetter)ml);
    for (int i = 0; i < c && placed < 3; i++) {
      leave_counts[ml]++;
      placed++;
    }
  }
  const int leave_size = 3;

  // Build pool: pick 12 tiles, mix of low- and high-scoring letters.
  // Use the first 12 letters with non-zero score that aren't entirely in
  // the leave already.
  int pool_total = 0;
  for (int ml = 1; ml < ld_size && pool_total < 12; ml++) {
    pool_counts[ml]++;
    pool_total++;
  }
  const int draw_size = 4;
  const int rack_size = leave_size + draw_size;

  SingleTileFeatures feats;
  single_tile_features(&scan, leave_counts, pool_counts, pool_total, draw_size,
                       rack_size, &feats);

  // Brute force
  BruteFeats brute = {0};
  uint8_t current_draw[MAX_ALPHABET_SIZE] = {0};
  const double total_denom = (double)binom(pool_total, draw_size);
  double check = 0.0;
  brute_recurse(&scan, leave_counts, pool_counts, draw_size, current_draw, 0, 1,
                rack_size, ld_size, total_denom, &brute, &check);

  // The total weight should sum to 1.0 (within fp noise).
  assert(fabs(check - 1.0) < 1e-9);

  // Compare to formula (allow small fp tolerance).
  if (fabs(feats.e_max_score - brute.e_max) > 1e-6) {
    printf("e_max formula=%f brute=%f\n", feats.e_max_score, brute.e_max);
  }
  if (fabs(feats.e_2nd_max_score - brute.e_2nd_max) > 1e-6) {
    printf("e_2nd_max formula=%f brute=%f\n", feats.e_2nd_max_score,
           brute.e_2nd_max);
  }
  if (fabs(feats.frac_playable - brute.e_frac) > 1e-9) {
    printf("frac_playable formula=%f brute=%f\n", feats.frac_playable,
           brute.e_frac);
  }
  assert(fabs(feats.e_max_score - brute.e_max) < 1e-6);
  assert(fabs(feats.e_2nd_max_score - brute.e_2nd_max) < 1e-6);
  assert(fabs(feats.frac_playable - brute.e_frac) < 1e-9);

  game_destroy(game);
  config_destroy(config);
}

// Sanity check: when draw_size == 0, the features are deterministic in
// the leave only and equal max / 2nd-max of leave letters' best_scores.
static void test_features_deterministic(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 1");
  Game *game = config_game_create(config);
  load_cgp_or_die(game, MIDGAME_CGP);

  SingleTileScan scan;
  single_tile_scan(game, &scan);

  const LetterDistribution *ld = game_get_ld(game);
  const int ld_size = ld_get_size(ld);
  const Player *player =
      game_get_player(game, game_get_player_on_turn_index(game));
  const Rack *real_rack = player_get_rack(player);

  uint8_t leave_counts[MAX_ALPHABET_SIZE] = {0};
  int leave_size = 0;
  int playable = 0;
  int sorted[16];
  int n_sorted = 0;
  for (int ml = 0; ml < ld_size; ml++) {
    const int c = rack_get_letter(real_rack, (MachineLetter)ml);
    leave_counts[ml] = (uint8_t)c;
    leave_size += c;
    for (int i = 0; i < c; i++) {
      sorted[n_sorted++] = (int)scan.best_score[ml];
      if (((scan.playable_set >> ml) & 1) != 0) {
        playable++;
      }
    }
  }
  for (int i = 1; i < n_sorted; i++) {
    int tmp = sorted[i];
    int j = i - 1;
    while (j >= 0 && sorted[j] < tmp) {
      sorted[j + 1] = sorted[j];
      j--;
    }
    sorted[j + 1] = tmp;
  }

  uint8_t pool_counts[MAX_ALPHABET_SIZE] = {0};
  SingleTileFeatures feats;
  single_tile_features(&scan, leave_counts, pool_counts, 0, 0, leave_size,
                       &feats);

  assert(fabs(feats.e_max_score - (double)sorted[0]) < 1e-9);
  assert(fabs(feats.e_2nd_max_score -
              (n_sorted >= 2 ? (double)sorted[1] : 0.0)) < 1e-9);
  assert(fabs(feats.frac_playable - ((double)playable / (double)leave_size)) <
         1e-9);

  game_destroy(game);
  config_destroy(config);
}

void test_single_tile_play(void) {
  test_scan_matches_movegen();
  test_features_deterministic();
  test_features_against_enumeration();
}

// Throughput benchmark for single_tile_scan + single_tile_features.
// Per iteration: redraw a fresh "us" rack from a fixed mid-game position,
// build the unseen pool from the bag + opp's rack, then time
//   1× single_tile_scan + 2× single_tile_features (us + opp).
// "us" rack is treated as a leave with random size 1..6 (so there's a
// non-trivial draw), while "opp" is a 7-tile draw with empty leave.
//
// Usage: ./bin/magpie_test singletilebench
// Env vars:
//   STBENCH_ITERS  iteration count (default 200000)
//   STBENCH_SEED   bag PRNG seed (default 42)
//   STBENCH_CGP    "midgame" / "empty" (default midgame)
static const char *EMPTY_CGP =
    "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 AILORRV/ 0/0 0";

void test_single_tile_bench(void) {
  const char *iters_env = getenv("STBENCH_ITERS");
  const int iters =
      (iters_env != NULL) ? (int)strtol(iters_env, NULL, 10) : 200000;
  const char *seed_env = getenv("STBENCH_SEED");
  const uint64_t seed =
      (seed_env != NULL) ? (uint64_t)strtoull(seed_env, NULL, 10) : 42ULL;
  const char *cgp_env = getenv("STBENCH_CGP");
  const char *cgp_label = (cgp_env != NULL) ? cgp_env : "midgame";
  const char *cgp_string = (cgp_env != NULL && strcmp(cgp_env, "empty") == 0)
                               ? EMPTY_CGP
                               : MIDGAME_CGP;

  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp true -s1 score -s2 score -r1 all -r2 all "
      "-numplays 1");
  Game *game = config_game_create(config);
  load_cgp_or_die(game, cgp_string);
  bag_seed(game_get_bag(game), seed);

  const LetterDistribution *ld = game_get_ld(game);
  const int ld_size = ld_get_size(ld);
  const int us_idx = game_get_player_on_turn_index(game);
  const int opp_idx = 1 - us_idx;
  Player *us = game_get_player(game, us_idx);
  Player *opp = game_get_player(game, opp_idx);
  Rack *us_rack = player_get_rack(us);
  Rack *opp_rack = player_get_rack(opp);
  Bag *bag = game_get_bag(game);

  uint8_t leave_us[MAX_ALPHABET_SIZE];
  uint8_t leave_opp[MAX_ALPHABET_SIZE];
  uint8_t pool_us[MAX_ALPHABET_SIZE];
  uint8_t pool_opp[MAX_ALPHABET_SIZE];

  // Warmup
  return_rack_to_bag(game, us_idx);
  draw_to_full_rack(game, us_idx);
  SingleTileScan warmup_scan;
  single_tile_scan(game, &warmup_scan);

  struct timespec start;
  struct timespec end;
  clock_gettime(CLOCK_MONOTONIC, &start); // NOLINT(misc-include-cleaner)

  double sink = 0.0;
  for (int i = 0; i < iters; i++) {
    // Re-draw "us" rack so the leave/draw composition varies. We also
    // simulate a partial leave by leaving only the first (i % 6) + 1
    // letters of the rack on the rack and returning the rest.
    return_rack_to_bag(game, us_idx);
    draw_to_full_rack(game, us_idx);
    const int leave_size = (i % 6) + 1;
    int dropped = 0;
    for (int ml = 0; ml < ld_size && dropped < RACK_SIZE - leave_size; ml++) {
      while (rack_get_letter(us_rack, (MachineLetter)ml) > 0 &&
             dropped < RACK_SIZE - leave_size) {
        rack_take_letter(us_rack, (MachineLetter)ml);
        bag_add_letter(bag, (MachineLetter)ml, us_idx);
        dropped++;
      }
    }

    // Build per-letter counts.
    memset(leave_us, 0, sizeof(leave_us));
    memset(leave_opp, 0, sizeof(leave_opp));
    memset(pool_us, 0, sizeof(pool_us));
    memset(pool_opp, 0, sizeof(pool_opp));
    int pool_us_size = 0;
    int pool_opp_size = 0;
    for (int ml = 0; ml < ld_size; ml++) {
      const int n_us = rack_get_letter(us_rack, (MachineLetter)ml);
      const int n_opp = rack_get_letter(opp_rack, (MachineLetter)ml);
      const int n_bag = bag_get_letter(bag, (MachineLetter)ml);
      leave_us[ml] = (uint8_t)n_us;
      // "Us" pool: bag + opp's rack (everything we don't see).
      pool_us[ml] = (uint8_t)(n_bag + n_opp);
      pool_us_size += n_bag + n_opp;
      // "Opp" pool: bag + opp's rack + our leave (everything not on board).
      pool_opp[ml] = (uint8_t)(n_bag + n_opp + n_us);
      pool_opp_size += n_bag + n_opp + n_us;
    }

    SingleTileScan scan;
    single_tile_scan(game, &scan);

    SingleTileFeatures feats_us;
    single_tile_features(&scan, leave_us, pool_us, pool_us_size,
                         RACK_SIZE - leave_size, RACK_SIZE, &feats_us);

    SingleTileFeatures feats_opp;
    single_tile_features(&scan, leave_opp, pool_opp, pool_opp_size, RACK_SIZE,
                         RACK_SIZE, &feats_opp);

    sink += feats_us.e_max_score + feats_opp.e_max_score +
            feats_us.frac_playable + feats_opp.frac_playable;
  }

  clock_gettime(CLOCK_MONOTONIC, &end);
  const double elapsed = (double)(end.tv_sec - start.tv_sec) +
                         (double)(end.tv_nsec - start.tv_nsec) / 1e9;
  const double calls_per_sec = (double)iters / elapsed;
  const double us_per_call = elapsed * 1e6 / (double)iters;

  printf("singletile bench cgp=%s iters=%d: %.3fs  %.0f calls/sec  "
         "%.3f us/call  (sink=%.6f)\n",
         cgp_label, iters, elapsed, calls_per_sec, us_per_call, sink);

  game_destroy(game);
  config_destroy(config);
}
