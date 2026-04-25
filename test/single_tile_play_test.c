// Tests for single_tile_play (per-square scan + closed-form + Monte Carlo).
//
// Correctness checks:
// 1. Per-(square, letter) score from the scan agrees with MAGPIE's existing
//    movegen for every single-tile play.
// 2. PRETZEL/SSSS?? deterministic case: top1=57, top2=10. Demonstrates
//    that per-square top-2 differs from naive per-letter top-2.
// 3. PRETZEL with random draw: closed-form vs Monte Carlo. Documents
//    the Jensen bias.
// 4. Features against full multinomial enumeration: validates both
//    closed-form and MC estimators.

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

static const char *MIDGAME_CGP =
    "WAgTAILS5K1/7P4QI1/3NEUTRINO1UN1/JOTA3I3BEDU/I2EFF1N2ZAS1N/"
    "HM5G2AGO1R/AE4REP2G2E/D6SOOTY1TA/ID11EL/1AR10L1/1WO7UNBOX/"
    "1TE10M1/1E11I1/1D10ICY/15 AILORRV/ 303/458 0";

// The trigraph "?\?/" avoids the "??/" trigraph warning while still
// parsing as the literal string "SSSS??/" (the rack with two blanks).
static const char *PRETZEL_CGP =
    "15/15/15/15/15/15/15/7PRETZEL1/15/15/15/15/15/15/15 SSSS?\?/ 0/0 0";

// Compute per-letter best score across the whole board by max-reducing
// the per-square scores from the scan.
static void scan_per_letter_best(const SingleTileScan *scan,
                                 const LetterDistribution *ld,
                                 Equity *out_best) {
  memset(out_best, 0, MAX_ALPHABET_SIZE * sizeof(Equity));
  for (int i = 0; i < scan->num_squares; i++) {
    const SingleTileSquare *sq = &scan->squares[i];
    uint64_t bits = sq->playable_letters;
    while (bits != 0) {
      const int ml = __builtin_ctzll(bits);
      bits &= bits - 1;
      const Equity face = ld_get_score(ld, (MachineLetter)ml);
      const Equity score = sq->base + face * sq->coef;
      if (score > out_best[ml]) {
        out_best[ml] = score;
      }
    }
  }
}

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

  Equity scan_best[MAX_ALPHABET_SIZE];
  scan_per_letter_best(&scan, game_get_ld(game), scan_best);

  const LetterDistribution *ld = game_get_ld(game);
  const int ld_size = ld_get_size(ld);
  for (int ml = 0; ml < ld_size; ml++) {
    if (scan_best[ml] != expected[ml]) {
      printf("ml=%d expected=%d scan=%d\n", ml, equity_to_int(expected[ml]),
             equity_to_int(scan_best[ml]));
    }
    assert(scan_best[ml] == expected[ml]);
    if (ml != 0 && expected[ml] > 0) {
      assert(((scan.playable_set >> ml) & 1) != 0);
    }
  }

  game_destroy(game);
  config_destroy(config);
}

// PRETZEL 8H + SSSS?? as deterministic full leave (no draw).
// Verifies the per-square interpretation: top1=57 (S at O8 making
// PRETZELS), top2=10 (blank at L9 making ZA/ZE/ZO with face 0).
static void test_pretzel_deterministic(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp true -s1 score -s2 score -r1 all -r2 all "
      "-numplays 1");
  Game *game = config_game_create(config);
  load_cgp_or_die(game, PRETZEL_CGP);
  const LetterDistribution *ld = game_get_ld(game);

  SingleTileScan scan;
  single_tile_scan(game, &scan);

  uint8_t leave_counts[MAX_ALPHABET_SIZE] = {0};
  MachineLetter mls[7];
  const int n_mls = ld_str_to_mls(ld, "SSSS??", false, mls, 6);
  for (int i = 0; i < n_mls; i++) {
    leave_counts[mls[i]]++;
  }
  uint8_t pool_counts[MAX_ALPHABET_SIZE] = {0};

  SingleTileFeatures feats;
  single_tile_features(&scan, ld, leave_counts, pool_counts, /*pool_size=*/0,
                       /*draw_size=*/0, /*rack_size=*/6, &feats);

  // Equity is millipoints: 57 points = 57000.
  if (fabs(feats.e_top1 - 57000.0) > 1e-6 ||
      fabs(feats.e_top2 - 10000.0) > 1e-6) {
    printf("PRETZEL det: top1=%.1f top2=%.1f frac=%.3f\n", feats.e_top1,
           feats.e_top2, feats.frac_playable);
  }
  assert(fabs(feats.e_top1 - 57000.0) < 1e-6);
  assert(fabs(feats.e_top2 - 10000.0) < 1e-6);

  game_destroy(game);
  config_destroy(config);
}

// PRETZEL 8H + SSSS?? leave + 1 draw from the actual bag (87 tiles).
// Expected E[top2] is dominated by the blank's L9 hook (10 most of
// the time), with small contributions from drawing a vowel (11 at L9)
// or the X (17 at M7/M9 DLS through E). Compare closed-form vs MC.
static void test_pretzel_with_draw(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp true -s1 score -s2 score -r1 all -r2 all "
      "-numplays 1");
  Game *game = config_game_create(config);
  load_cgp_or_die(game, PRETZEL_CGP);
  const LetterDistribution *ld = game_get_ld(game);
  const int ld_size = ld_get_size(ld);

  SingleTileScan scan;
  single_tile_scan(game, &scan);

  uint8_t leave_counts[MAX_ALPHABET_SIZE] = {0};
  MachineLetter mls[7];
  const int n_mls = ld_str_to_mls(ld, "SSSS??", false, mls, 6);
  for (int i = 0; i < n_mls; i++) {
    leave_counts[mls[i]]++;
  }

  // Pool = bag (everything not on board, not in our leave).
  uint8_t pool_counts[MAX_ALPHABET_SIZE] = {0};
  int pool_size = 0;
  const Bag *bag = game_get_bag(game);
  for (int ml = 0; ml < ld_size; ml++) {
    pool_counts[ml] = (uint8_t)bag_get_letter(bag, (MachineLetter)ml);
    pool_size += pool_counts[ml];
  }

  SingleTileFeatures cf;
  single_tile_features(&scan, ld, leave_counts, pool_counts, pool_size,
                       /*draw_size=*/1, /*rack_size=*/7, &cf);
  SingleTileFeatures mc;
  single_tile_features_mc(&scan, ld, leave_counts, pool_counts, pool_size,
                          /*draw_size=*/1, /*rack_size=*/7, /*samples=*/200000,
                          /*seed=*/42, &mc);

  // Convert to points for readability (Equity is millipoints).
  printf("PRETZEL +1 draw: cf top1=%.4f top2=%.4f frac=%.4f | "
         "mc top1=%.4f top2=%.4f frac=%.4f\n",
         cf.e_top1 / 1000.0, cf.e_top2 / 1000.0, cf.frac_playable,
         mc.e_top1 / 1000.0, mc.e_top2 / 1000.0, mc.frac_playable);

  // top1 should agree across both modes (E[top1] is invariant) up to
  // MC noise. Tolerance in millipoints.
  assert(fabs(cf.e_top1 - mc.e_top1) < 500.0);
  // top2 closed-form vs MC: both should be in the ~10–12 point range
  // (10000–12000 millipoints) per the back-of-envelope ~10.4.
  assert(cf.e_top2 > 10000.0 && cf.e_top2 < 12000.0);
  assert(mc.e_top2 > 10000.0 && mc.e_top2 < 12000.0);
  // frac is exact in both modes (linearity).
  assert(fabs(cf.frac_playable - mc.frac_playable) < 1e-9);

  game_destroy(game);
  config_destroy(config);
}

// Brute-force enumeration of expected per-square top1/top2 for a known
// leave + small pool + small draw.
typedef struct {
  double e_top1;
  double e_top2;
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

static void brute_per_rack(const SingleTileScan *scan,
                           const LetterDistribution *ld,
                           const uint8_t *rack_counts, double *top1,
                           double *top2) {
  *top1 = 0;
  *top2 = 0;
  for (int i = 0; i < scan->num_squares; i++) {
    const SingleTileSquare *sq = &scan->squares[i];
    Equity best = 0;
    uint64_t bits = sq->playable_letters;
    while (bits != 0) {
      const int ml = __builtin_ctzll(bits);
      bits &= bits - 1;
      if (rack_counts[ml] == 0) {
        continue;
      }
      const Equity face = ld_get_score(ld, (MachineLetter)ml);
      const Equity score = sq->base + face * sq->coef;
      if (score > best) {
        best = score;
      }
    }
    const double v = (double)best;
    if (v > *top1) {
      *top2 = *top1;
      *top1 = v;
    } else if (v > *top2) {
      *top2 = v;
    }
  }
}

static void brute_recurse(const SingleTileScan *scan,
                          const LetterDistribution *ld,
                          const uint8_t *leave_counts,
                          const uint8_t *pool_counts, int draw_size,
                          uint8_t *current_draw, int ml, uint64_t weight,
                          int rack_size, int ld_size, double total_denom,
                          BruteFeats *out, double *check_total_weight) {
  if (draw_size == 0) {
    uint8_t rack_counts[MAX_ALPHABET_SIZE] = {0};
    int playable_in_rack = 0;
    for (int j = 0; j < ld_size; j++) {
      rack_counts[j] = (uint8_t)(leave_counts[j] + current_draw[j]);
      if (((scan->playable_set >> j) & 1) != 0) {
        playable_in_rack += rack_counts[j];
      }
    }
    double top1;
    double top2;
    brute_per_rack(scan, ld, rack_counts, &top1, &top2);
    const double w = (double)weight / total_denom;
    out->e_top1 += w * top1;
    out->e_top2 += w * top2;
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
    brute_recurse(scan, ld, leave_counts, pool_counts, draw_size - take,
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
  const LetterDistribution *ld = game_get_ld(game);
  const int ld_size = ld_get_size(ld);

  SingleTileScan scan;
  single_tile_scan(game, &scan);

  uint8_t leave_counts[MAX_ALPHABET_SIZE] = {0};
  uint8_t pool_counts[MAX_ALPHABET_SIZE] = {0};
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

  int pool_total = 0;
  for (int ml = 1; ml < ld_size && pool_total < 12; ml++) {
    pool_counts[ml]++;
    pool_total++;
  }
  const int draw_size = 4;
  const int rack_size = 3 + draw_size;

  SingleTileFeatures cf;
  single_tile_features(&scan, ld, leave_counts, pool_counts, pool_total,
                       draw_size, rack_size, &cf);

  BruteFeats brute = {0};
  uint8_t current_draw[MAX_ALPHABET_SIZE] = {0};
  const double total_denom = (double)binom(pool_total, draw_size);
  double check = 0.0;
  brute_recurse(&scan, ld, leave_counts, pool_counts, draw_size, current_draw,
                0, 1, rack_size, ld_size, total_denom, &brute, &check);

  assert(fabs(check - 1.0) < 1e-9);

  // top1 closed-form should match brute exactly (ignoring fp noise).
  if (fabs(cf.e_top1 - brute.e_top1) > 1e-6) {
    printf("e_top1 cf=%.6f brute=%.6f\n", cf.e_top1, brute.e_top1);
  }
  assert(fabs(cf.e_top1 - brute.e_top1) < 1e-6);
  // top2 closed-form is a Jensen-biased proxy for brute. Bias is
  // bounded by some factor of the underlying scale; allow generous
  // tolerance.
  printf("e_top2 cf=%.6f brute=%.6f (bias=%.6f)\n", cf.e_top2, brute.e_top2,
         cf.e_top2 - brute.e_top2);
  // frac_playable should match exactly.
  assert(fabs(cf.frac_playable - brute.e_frac) < 1e-9);

  // MC should converge to brute.
  SingleTileFeatures mc;
  single_tile_features_mc(&scan, ld, leave_counts, pool_counts, pool_total,
                          draw_size, rack_size, /*samples=*/100000,
                          /*seed=*/42, &mc);
  printf("e_top2 mc=%.6f vs brute=%.6f (err=%.6f)\n", mc.e_top2, brute.e_top2,
         mc.e_top2 - brute.e_top2);
  // Tolerances in millipoints: ~100 points scale, MC noise on 100k samples.
  assert(fabs(mc.e_top1 - brute.e_top1) < 200.0);
  assert(fabs(mc.e_top2 - brute.e_top2) < 500.0);

  game_destroy(game);
  config_destroy(config);
}

void test_single_tile_play(void) {
  test_scan_matches_movegen();
  test_pretzel_deterministic();
  test_pretzel_with_draw();
  test_features_against_enumeration();
}

// Throughput benchmark for single_tile_scan + single_tile_features
// (closed-form). One call = scan + features for "us" + features for
// "opp". Empty board is excluded — no single-tile play can ever be
// legal there (no 1-letter words), so the scan does no work and the
// timing isn't meaningful for this feature.
void test_single_tile_bench(void) {
  const char *iters_env = getenv("STBENCH_ITERS");
  const int iters =
      (iters_env != NULL) ? (int)strtol(iters_env, NULL, 10) : 200000;
  const char *seed_env = getenv("STBENCH_SEED");
  const uint64_t seed =
      (seed_env != NULL) ? (uint64_t)strtoull(seed_env, NULL, 10) : 42ULL;

  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp true -s1 score -s2 score -r1 all -r2 all "
      "-numplays 1");
  Game *game = config_game_create(config);
  load_cgp_or_die(game, MIDGAME_CGP);
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

  return_rack_to_bag(game, us_idx);
  draw_to_full_rack(game, us_idx);
  SingleTileScan warmup_scan;
  single_tile_scan(game, &warmup_scan);

  struct timespec start;
  struct timespec end;
  clock_gettime(CLOCK_MONOTONIC, &start); // NOLINT(misc-include-cleaner)

  double sink = 0.0;
  for (int i = 0; i < iters; i++) {
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
      pool_us[ml] = (uint8_t)(n_bag + n_opp);
      pool_us_size += n_bag + n_opp;
      pool_opp[ml] = (uint8_t)(n_bag + n_opp + n_us);
      pool_opp_size += n_bag + n_opp + n_us;
    }

    SingleTileScan scan;
    single_tile_scan(game, &scan);

    SingleTileFeatures feats_us;
    single_tile_features(&scan, ld, leave_us, pool_us, pool_us_size,
                         RACK_SIZE - leave_size, RACK_SIZE, &feats_us);
    SingleTileFeatures feats_opp;
    single_tile_features(&scan, ld, leave_opp, pool_opp, pool_opp_size,
                         RACK_SIZE, RACK_SIZE, &feats_opp);

    sink += feats_us.e_top1 + feats_opp.e_top1 + feats_us.e_top2 +
            feats_opp.e_top2 + feats_us.frac_playable + feats_opp.frac_playable;
  }

  clock_gettime(CLOCK_MONOTONIC, &end);
  const double elapsed = (double)(end.tv_sec - start.tv_sec) +
                         (double)(end.tv_nsec - start.tv_nsec) / 1e9;
  const double calls_per_sec = (double)iters / elapsed;
  const double us_per_call = elapsed * 1e6 / (double)iters;

  printf("singletile bench iters=%d: %.3fs  %.0f calls/sec  "
         "%.3f us/call  (sink=%.6f)\n",
         iters, elapsed, calls_per_sec, us_per_call, sink);

  game_destroy(game);
  config_destroy(config);
}
