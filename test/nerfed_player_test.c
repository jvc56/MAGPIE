#include "../src/ent/game.h"
#include "../src/ent/move.h"
#include "../src/ent/xoshiro.h"
#include "../src/impl/config.h"
#include "../src/impl/nerfed_player.h"
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
