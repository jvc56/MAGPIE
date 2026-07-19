#include "../src/ent/game.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/sim_results.h"
#include "../src/ent/stats.h"
#include "../src/ent/xoshiro.h"
#include "../src/impl/config.h"
#include "../src/impl/nerfed_player.h"
#include "../src/impl/simmer.h"
#include "../src/util/io_util.h"
#include "test_util.h"
#include <assert.h>
#include <math.h>
#include <stdint.h>

// SCHMECK is the only bingo this rack forms (every bingo placement spells
// SCHMECK) and it is maximally obscure in CSW24 (logplay -2.0, loglit
// -1.0). So "the nerfed player found the obscure word" is exactly "the
// selected move played 7 tiles". The visibility gate should almost never
// surface it to a weak player and usually surface it to a strong one.
#define SCHMECK_RACK "CCEHKMS"
#define OBSCURE_TRIALS 50

// False-failure budget per one-sided binomial test. Thresholds below are
// the critical counts at this level, so a correctly calibrated model
// fails by chance far less than once in 10^4 runs.
#define OBSCURE_ALPHA 1.0e-4
// A weak player finding this word more often than P_WEAK_CEILING is a
// regression; the assertion rejects that at OBSCURE_ALPHA. The measured
// weak find rate is ~0.005, so the critical count (10/50) is never
// reached by chance while still catching a regression above ~20%.
#define P_WEAK_CEILING 0.05
// A conservative floor on the strong player's find rate (measured ~0.45);
// guards against a degenerate test where the word is never generated.
// Critical count 6/50, far below the measured strong counts (19-28).
#define P_STRONG_FLOOR 0.35

// P(X > k) for X ~ Binomial(n, p), computed from a numerically stable
// forward pmf recurrence (no factorials / overflow).
static double binomial_upper_tail(int n, int k, double p) {
  double pmf = pow(1.0 - p, n); // P(X = 0)
  double cdf = pmf;             // P(X <= 0)
  for (int i = 1; i <= k; i++) {
    pmf *= ((double)(n - i + 1) / (double)i) * (p / (1.0 - p));
    cdf += pmf;
  }
  return 1.0 - cdf;
}

static double log_choose(int n, int k) {
  return lgamma(n + 1.0) - lgamma(k + 1.0) - lgamma(n - k + 1.0);
}

// P(X >= k) for X ~ Hypergeometric(population N, K successes, n draws),
// computed in log-space (the counts are far too large for exact choose()).
// This is the exact null distribution for the two-batch bait test: of the
// N = 2 * BAIT_PICKS pick slots, K = BAIT_PICKS belong to the bait batch,
// and the n = total_obscure obscure picks land at random among all slots
// under "bait has no effect".
static double hypergeometric_upper_tail(int N, int K, int n, int k) {
  const double log_denominator = log_choose(N, n);
  double tail = 0.0;
  const int hi = (n < K) ? n : K;
  for (int i = k; i <= hi; i++) {
    if (n - i > N - K) {
      continue;
    }
    tail += exp(log_choose(K, i) + log_choose(N - K, n - i) - log_denominator);
  }
  return tail;
}

// Smallest count T such that P(X > T) < alpha under Binomial(n, p): the
// upper critical value for a one-sided "not more than p" test.
static int binomial_upper_critical(int n, double p, double alpha) {
  for (int k = 0; k <= n; k++) {
    if (binomial_upper_tail(n, k, p) < alpha) {
      return k;
    }
  }
  return n;
}

// Largest count T such that P(X < T) < alpha under Binomial(n, p): the
// lower critical value for a one-sided "at least p" test.
static int binomial_lower_critical(int n, double p, double alpha) {
  for (int k = n; k >= 0; k--) {
    // P(X < k) = P(X <= k-1) = 1 - P(X > k-1).
    const double lower = 1.0 - binomial_upper_tail(n, k - 1, p);
    if (lower < alpha) {
      return k;
    }
  }
  return 0;
}

// Runs OBSCURE_TRIALS independent selections from the SCHMECK opening at
// the given rating and returns how many played the obscure bingo.
static int count_obscure_finds(int rating, uint64_t seed) {
  Config *config = config_create_or_die(
      "set -lex CSW24 -wmp true -s1 equity -s2 equity -numplays 15");
  load_and_exec_config_or_die(
      config, "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 " SCHMECK_RACK
              "/ 0/0 0");
  Game *game = config_get_game(config);
  ErrorStack *error_stack = error_stack_create();
  NerfedPlayer *nerfed_player = nerfed_player_create(game, rating, error_stack);
  assert(error_stack_is_empty(error_stack));
  XoshiroPRNG *prng = prng_create(seed);
  int finds = 0;
  for (int trial = 0; trial < OBSCURE_TRIALS; trial++) {
    const Move *move = nerfed_player_select_move(nerfed_player, game, prng);
    if (move_get_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE &&
        move_get_tiles_played(move) == 7) {
      finds++;
    }
  }
  prng_destroy(prng);
  nerfed_player_destroy(nerfed_player);
  error_stack_destroy(error_stack);
  config_destroy(config);
  return finds;
}

void test_nerfed_player_obscure_word_visibility(void) {
  const int weak_max =
      binomial_upper_critical(OBSCURE_TRIALS, P_WEAK_CEILING, OBSCURE_ALPHA);
  const int strong_min =
      binomial_lower_critical(OBSCURE_TRIALS, P_STRONG_FLOOR, OBSCURE_ALPHA);
  // Independent seeds: each is its own test at level OBSCURE_ALPHA.
  const uint64_t seeds[] = {0x9E3779B97F4A7C15ULL, 1, 42, 12345, 0xABCDEF};
  for (int i = 0; i < 5; i++) {
    const int weak_finds = count_obscure_finds(1000, seeds[i]);
    const int strong_finds = count_obscure_finds(2200, seeds[i]);
    // A 1000 should almost never surface the obscure bingo.
    assert(weak_finds <= weak_max);
    // The strong player must actually be able to find it (findability
    // guard), and clearly more than the weak player.
    assert(strong_finds >= strong_min);
    assert(strong_finds > weak_finds);
  }
}

// ---------------------------------------------------------------------------
// A simming 2200 facing a 1000, opening rack ACHLMY + blank. The rack makes
// two same-scoring bingos: ALCHEMY (common, literacy -0.5, blank = E) and
// ALCHYMY (its floor-obscure archaic variant, blank = Y). On the calibrated
// model the two sim within ~1% win probability of each other, with the
// common word marginally ahead -- so a plain simmer has no reason to prefer
// the obscure one. But against a weak opponent under double challenge the
// challenge-bait EV (the 1000 is likely unfamiliar with ALCHYMY and may
// wrongly challenge it, forfeiting a turn) makes the expert prefer the
// obscure word to invite the challenge.
//
// The sim runs once and the noisy pick is looped cheaply; the bait context is
// the only difference between the two pick batches. Under the null "baiting
// does not increase obscure picks" the obscure picks are exchangeable between
// the (independent) batches, so the bait-batch share of the obscure picks is
// Hypergeometric under the null. The test rejects that null.
#define BAIT_RACK "ACHLMY?"
#define BAIT_PICKS 300
// Visibility seed under which both bingos survive the 2200 gate and are
// simmed (a floor-obscure word is only visible to a 2200 ~45% of the time,
// so the arm set is frozen to a seed where ALCHYMY is present).
#define BAIT_ARM_SEED 8
#define BAIT_PICK_SEED 999
#define BAIT_ALPHA 1.0e-4
// The two bingos must sim within this window (a genuinely close decision) for
// the bait to be what tips it.
#define BAIT_SIM_NEAR_TIE 0.06

// Classifies a bingo by the letter its blank represents. Two of the three
// bingos are obscure bait words (a 1000 would not recognize either, both
// literacy -1.0 / absent from text): 'Y' -> ALCHYMY (the floor-obscure
// ALCHEMY variant) and 'S' -> CHLAMYS (playable but literately obscure).
// 'E' -> ALCHEMY, the one common word.
static char bait_blank_letter(const Move *move, MachineLetter e_ml,
                              MachineLetter y_ml, MachineLetter s_ml) {
  const int tiles = move_get_tiles_length(move);
  for (int i = 0; i < tiles; i++) {
    const MachineLetter tile = move_get_tile(move, i);
    if (get_is_blanked(tile)) {
      const MachineLetter unblanked = get_unblanked_machine_letter(tile);
      if (unblanked == e_ml) {
        return 'E';
      }
      if (unblanked == y_ml) {
        return 'Y';
      }
      if (unblanked == s_ml) {
        return 'S';
      }
    }
  }
  return '?';
}

// Number of the BAIT_PICKS noisy picks (over the shared sim results) that
// chose an obscure bait bingo (ALCHYMY or CHLAMYS).
static int bait_count_obscure(NerfedPlayer *nerfed_player, Config *config,
                              const SimResults *sim_results, MachineLetter e_ml,
                              MachineLetter y_ml, MachineLetter s_ml,
                              uint64_t pick_seed) {
  XoshiroPRNG *pick_prng = prng_create(pick_seed);
  int obscure = 0;
  for (int pick = 0; pick < BAIT_PICKS; pick++) {
    const Move *move = nerfed_player_pick_simmed_move(
        nerfed_player, sim_results, config_get_utility_w_winpct(config),
        config_get_utility_w_spread(config),
        config_get_utility_spread_scale(config), pick_prng);
    const char which = bait_blank_letter(move, e_ml, y_ml, s_ml);
    if (which == 'Y' || which == 'S') {
      obscure++;
    }
  }
  prng_destroy(pick_prng);
  return obscure;
}

void test_nerfed_player_bait_prefers_obscure(void) {
  Config *config = config_create_or_die(
      "set -lex NWL23 -wmp true -s1 equity -s2 equity -numplays 20 -plies 2 "
      "-threads 1 -iter 160 -scond none -seed 3 -uspread 0.5");
  load_and_exec_config_or_die(
      config,
      "cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 " BAIT_RACK "/ 0/0 0");
  // Allocate config->move_list (prepare_sim_arms refills it in place, and
  // config_simulate sims over it).
  load_and_exec_config_or_die(config, "gen");
  Game *game = config_get_game(config);
  const LetterDistribution *ld = game_get_ld(game);
  MachineLetter e_ml, y_ml, s_ml;
  ld_str_to_mls(ld, "E", false, &e_ml, 1);
  ld_str_to_mls(ld, "Y", false, &y_ml, 1);
  ld_str_to_mls(ld, "S", false, &s_ml, 1);
  ErrorStack *error_stack = error_stack_create();

  // Baseline expert (no bait): gate + stratify the arms, sim them once.
  NerfedPlayer *baseline = nerfed_player_create(game, 2200, error_stack);
  assert(error_stack_is_empty(error_stack));
  XoshiroPRNG *arm_prng = prng_create(BAIT_ARM_SEED);
  const Move *decided = nerfed_player_prepare_sim_arms(
      baseline, game, config_get_move_list(config), arm_prng);
  assert(decided == NULL);
  SimCtx *sim_ctx = NULL;
  config_simulate(config, &sim_ctx, NULL, config_get_sim_results(config), NULL,
                  0, error_stack);
  assert(error_stack_is_empty(error_stack));
  const SimResults *sim_results = config_get_sim_results(config);

  // Premise: both bingos are simmed and the decision is a near-tie.
  int e_index = -1;
  int y_index = -1;
  const int num_plays = sim_results_get_number_of_plays(sim_results);
  for (int play = 0; play < num_plays; play++) {
    const Move *move =
        simmed_play_get_move(sim_results_get_simmed_play(sim_results, play));
    const char which = bait_blank_letter(move, e_ml, y_ml, s_ml);
    if (which == 'E') {
      e_index = play;
    } else if (which == 'Y') {
      y_index = play;
    }
  }
  // The near-tie premise is documented on the ALCHEMY/ALCHYMY pair; both
  // must be simmed arms.
  assert(e_index >= 0 && y_index >= 0);
  const double win_common = stat_get_mean(simmed_play_get_win_pct_stat(
      sim_results_get_simmed_play(sim_results, e_index)));
  const double win_obscure = stat_get_mean(simmed_play_get_win_pct_stat(
      sim_results_get_simmed_play(sim_results, y_index)));
  assert(fabs(win_common - win_obscure) < BAIT_SIM_NEAR_TIE);

  const int baseline_obscure = bait_count_obscure(
      baseline, config, sim_results, e_ml, y_ml, s_ml, BAIT_PICK_SEED);

  // Exploitative expert (bait): identical arms (same visibility seed), so the
  // shared sim results still apply; only the challenge context differs.
  NerfedPlayer *baiter = nerfed_player_create(game, 2200, error_stack);
  assert(error_stack_is_empty(error_stack));
  nerfed_player_set_challenge_context(baiter, false, (1000.0 - 1500.0) / 300.0);
  XoshiroPRNG *baiter_arm_prng = prng_create(BAIT_ARM_SEED);
  const Move *baiter_decided = nerfed_player_prepare_sim_arms(
      baiter, game, config_get_move_list(config), baiter_arm_prng);
  assert(baiter_decided == NULL);
  const int bait_obscure = bait_count_obscure(baiter, config, sim_results, e_ml,
                                              y_ml, s_ml, BAIT_PICK_SEED + 1);

  // Non-degeneracy, direction, and the exchangeability test. Under "bait
  // does not increase obscure picks" the total_obscure picks land at random
  // among the 2 * BAIT_PICKS pick slots, so the bait-batch share is
  // Hypergeometric; reject that null at BAIT_ALPHA.
  const int total_obscure = baseline_obscure + bait_obscure;
  assert(total_obscure >= 20);
  assert(bait_obscure > baseline_obscure);
  assert(hypergeometric_upper_tail(2 * BAIT_PICKS, BAIT_PICKS, total_obscure,
                                   bait_obscure) < BAIT_ALPHA);

  prng_destroy(baiter_arm_prng);
  nerfed_player_destroy(baiter);
  sim_ctx_destroy(sim_ctx);
  prng_destroy(arm_prng);
  nerfed_player_destroy(baseline);
  error_stack_destroy(error_stack);
  config_destroy(config);
}
