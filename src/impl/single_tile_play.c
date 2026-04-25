// Computes per-letter best single-tile play scores by scanning cross_sets,
// and per-rack expected single-tile-play features (fraction playable,
// E[max score], E[2nd-max score]) for racks of the form
// (known leave) + (random hypergeometric draw from a pool).

#include "single_tile_play.h"

#include "../def/board_defs.h"
#include "../def/cross_set_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/players_data_defs.h"
#include "../ent/board.h"
#include "../ent/bonus_square.h"
#include "../ent/equity.h"
#include "../ent/letter_distribution.h"
#include <stdint.h>
#include <string.h>

// ---------------------------------------------------------------------
// Board scan
// ---------------------------------------------------------------------

void single_tile_scan(const Game *game, SingleTileScan *scan) {
  memset(scan, 0, sizeof(*scan));

  const Board *board = game_get_board(game);
  const LetterDistribution *ld = game_get_ld(game);
  const int player_index = game_get_player_on_turn_index(game);
  const bool kwgs_shared = game_get_data_is_shared(game, PLAYERS_DATA_TYPE_KWG);
  const int csi = board_get_cross_set_index(kwgs_shared, player_index);

  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      if (!board_is_empty(board, row, col)) {
        continue;
      }
      const uint64_t cs_h =
          board_get_cross_set(board, row, col, BOARD_HORIZONTAL_DIRECTION, csi);
      const uint64_t cs_v =
          board_get_cross_set(board, row, col, BOARD_VERTICAL_DIRECTION, csi);
      const uint64_t playable = cs_h & cs_v;
      // Stranded squares have both cross_sets == TRIVIAL, so playable ==
      // TRIVIAL too. They aren't actually playable; skip them.
      if (playable == TRIVIAL_CROSS_SET || playable == 0) {
        continue;
      }
      scan->playable_set |= playable;

      const BonusSquare bonus = board_get_bonus_square(board, row, col);
      const int letter_mult = bonus_square_get_letter_multiplier(bonus);
      const int word_mult = bonus_square_get_word_multiplier(bonus);
      // cross_score[V] is the sum of horizontal-neighbor tile values;
      // cross_score[H] is the sum of vertical-neighbor tile values.
      const Equity h_run_score =
          board_get_cross_score(board, row, col, BOARD_VERTICAL_DIRECTION, csi);
      const Equity v_run_score = board_get_cross_score(
          board, row, col, BOARD_HORIZONTAL_DIRECTION, csi);
      const bool has_h_word = (cs_v != TRIVIAL_CROSS_SET);
      const bool has_v_word = (cs_h != TRIVIAL_CROSS_SET);

      uint64_t bits = playable;
      while (bits != 0) {
        const int ml = __builtin_ctzll(bits);
        bits &= bits - 1;

        const Equity face = ld_get_score(ld, (MachineLetter)ml);
        Equity score = 0;
        if (has_h_word) {
          score += (h_run_score + face * letter_mult) * word_mult;
        }
        if (has_v_word) {
          score += (v_run_score + face * letter_mult) * word_mult;
        }
        if (score > scan->best_score[ml]) {
          scan->best_score[ml] = score;
        }
      }
    }
  }
}

// ---------------------------------------------------------------------
// Per-rack features
// ---------------------------------------------------------------------

// C(n, k) for k <= 7 fits comfortably in uint64 for any reasonable n.
// Returns 0 if k > n.
static uint64_t binomial_u64(int n, int k) {
  if (k < 0 || k > n) {
    return 0;
  }
  if (k > n - k) {
    k = n - k;
  }
  uint64_t result = 1;
  for (int i = 0; i < k; i++) {
    result = result * (uint64_t)(n - i) / (uint64_t)(i + 1);
  }
  return result;
}

// Sorts the (score, leave_count, pool_count) triples by ascending score.
// Insertion sort: K is small (<= MAX_ALPHABET_SIZE).
typedef struct {
  Equity score;
  int leave_count;
  int pool_count;
} ScoreBin;

static int build_score_bins(const SingleTileScan *scan,
                            const uint8_t *leave_counts,
                            const uint8_t *pool_counts, ScoreBin *bins) {
  // Group letters by their best_score. Letters with the same score share
  // a bin. Always include score = 0 (covers non-playable letters and the
  // baseline lower bound of the CDF).
  int num_bins = 1;
  bins[0].score = 0;
  bins[0].leave_count = 0;
  bins[0].pool_count = 0;
  for (int ml = 0; ml < MAX_ALPHABET_SIZE; ml++) {
    const int lc = leave_counts[ml];
    const int pc = pool_counts[ml];
    if (lc == 0 && pc == 0) {
      continue;
    }
    const Equity s = scan->best_score[ml];
    int found = -1;
    for (int i = 0; i < num_bins; i++) {
      if (bins[i].score == s) {
        found = i;
        break;
      }
    }
    if (found >= 0) {
      bins[found].leave_count += lc;
      bins[found].pool_count += pc;
    } else {
      bins[num_bins].score = s;
      bins[num_bins].leave_count = lc;
      bins[num_bins].pool_count = pc;
      num_bins++;
    }
  }
  // Insertion sort by ascending score.
  for (int i = 1; i < num_bins; i++) {
    ScoreBin tmp = bins[i];
    int j = i - 1;
    while (j >= 0 && bins[j].score > tmp.score) {
      bins[j + 1] = bins[j];
      j--;
    }
    bins[j + 1] = tmp;
  }
  return num_bins;
}

void single_tile_features(const SingleTileScan *scan,
                          const uint8_t *leave_counts,
                          const uint8_t *pool_counts, int pool_size,
                          int draw_size, int rack_size,
                          SingleTileFeatures *out) {
  // Fraction playable: linearity of expectation.
  int leave_playable = 0;
  int pool_playable = 0;
  for (int ml = 0; ml < MAX_ALPHABET_SIZE; ml++) {
    if (((scan->playable_set >> ml) & 1) != 0) {
      leave_playable += leave_counts[ml];
      pool_playable += pool_counts[ml];
    }
  }
  const double pool_frac =
      (pool_size > 0) ? (double)pool_playable / (double)pool_size : 0.0;
  out->frac_playable =
      ((double)leave_playable + (double)draw_size * pool_frac) /
      (double)rack_size;

  // Order statistics for E[max] and E[2nd_max].
  ScoreBin bins[MAX_ALPHABET_SIZE + 1];
  const int num_bins = build_score_bins(scan, leave_counts, pool_counts, bins);

  const uint64_t denom = binomial_u64(pool_size, draw_size);
  const int d = (denom == 0) ? 0 : draw_size;
  const uint64_t denom_eff = (denom == 0) ? 1 : denom;

  // Cumulative counts strictly above each bin: leave_above[i] is the
  // number of leave tiles with score > bins[i].score (and similarly for
  // the pool). Computed by walking descending once.
  int leave_above[MAX_ALPHABET_SIZE + 1];
  int pool_above[MAX_ALPHABET_SIZE + 1];
  if (num_bins > 0) {
    leave_above[num_bins - 1] = 0;
    pool_above[num_bins - 1] = 0;
    for (int i = num_bins - 2; i >= 0; i--) {
      leave_above[i] = leave_above[i + 1] + bins[i + 1].leave_count;
      pool_above[i] = pool_above[i + 1] + bins[i + 1].pool_count;
    }
  }

  // Walk bins ascending and accumulate E[X] = Σ s_i · (P(X≤s_i) −
  // P(X≤s_{i−1})).
  double e_max = 0.0;
  double e_2nd_max = 0.0;
  double prev_p_max = 0.0; // P(X <= -inf) = 0
  double prev_p_2nd_max = 0.0;
  for (int i = 0; i < num_bins; i++) {
    double p_max = 0.0;
    double p_2nd_max = 0.0;
    if (leave_above[i] == 0) {
      p_max = (double)binomial_u64(pool_size - pool_above[i], d) /
              (double)denom_eff;
      double term2 = 0.0;
      if (d >= 1) {
        term2 = (double)pool_above[i] *
                (double)binomial_u64(pool_size - pool_above[i], d - 1) /
                (double)denom_eff;
      }
      p_2nd_max = p_max + term2;
    } else if (leave_above[i] == 1) {
      p_2nd_max = (double)binomial_u64(pool_size - pool_above[i], d) /
                  (double)denom_eff;
    }
    e_max += (double)bins[i].score * (p_max - prev_p_max);
    e_2nd_max += (double)bins[i].score * (p_2nd_max - prev_p_2nd_max);
    prev_p_max = p_max;
    prev_p_2nd_max = p_2nd_max;
  }
  out->e_max_score = e_max;
  out->e_2nd_max_score = e_2nd_max;
}
