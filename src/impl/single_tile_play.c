// Per-square scan of single-tile play scores plus closed-form and
// Monte-Carlo computation of expected top-1 / top-2 single-tile play
// scores for racks of the form (known leave) + (random hypergeometric
// draw from a pool).

#include "single_tile_play.h"

#include "../def/board_defs.h"
#include "../def/cross_set_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/players_data_defs.h"
#include "../ent/board.h"
#include "../ent/bonus_square.h"
#include "../ent/equity.h"
#include "../ent/letter_distribution.h"
#include "../ent/xoshiro.h"
#include <stdint.h>
#include <string.h>

// ---------------------------------------------------------------------
// Board scan
// ---------------------------------------------------------------------

void single_tile_scan(const Game *game, SingleTileScan *scan) {
  scan->playable_set = 0;
  scan->num_squares = 0;
  memset(scan->best_score, 0, sizeof(scan->best_score));

  const Board *board = game_get_board(game);
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
      // Stranded squares have both cross_sets == TRIVIAL → playable ==
      // TRIVIAL. They aren't actually playable; skip.
      if (playable == TRIVIAL_CROSS_SET || playable == 0) {
        continue;
      }

      const BonusSquare bonus = board_get_bonus_square(board, row, col);
      const int letter_mult = bonus_square_get_letter_multiplier(bonus);
      const int word_mult = bonus_square_get_word_multiplier(bonus);
      const Equity h_run =
          board_get_cross_score(board, row, col, BOARD_VERTICAL_DIRECTION, csi);
      const Equity v_run = board_get_cross_score(
          board, row, col, BOARD_HORIZONTAL_DIRECTION, csi);
      const int has_h_word = (cs_v != TRIVIAL_CROSS_SET) ? 1 : 0;
      const int has_v_word = (cs_h != TRIVIAL_CROSS_SET) ? 1 : 0;

      SingleTileSquare *sq = &scan->squares[scan->num_squares++];
      sq->playable_letters = playable;
      sq->base = (h_run * has_h_word + v_run * has_v_word) * word_mult;
      sq->coef = (Equity)(letter_mult * word_mult * (has_h_word + has_v_word));
      scan->playable_set |= playable;

      // Update per-letter best_score for use by closed-form top1.
      const LetterDistribution *ld_for_face = game_get_ld(game);
      uint64_t bits = playable;
      while (bits != 0) {
        const int ml = __builtin_ctzll(bits);
        bits &= bits - 1;
        const Equity face = ld_get_score(ld_for_face, (MachineLetter)ml);
        const Equity score = sq->base + face * sq->coef;
        if (score > scan->best_score[ml]) {
          scan->best_score[ml] = score;
        }
      }
    }
  }
}

// ---------------------------------------------------------------------
// Combinatorics + per-square order stats
// ---------------------------------------------------------------------

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

typedef struct {
  Equity score;
  int leave_count;
  int pool_count;
} ScoreBin;

// Computes E[max_{L in rack} score(s, L)] for a single square s.
// Non-playable letters at s implicitly score 0; we handle this by
// including an implicit "score = 0" bin (which contributes nothing to
// E[max] but shifts the CDF baseline from -inf to 0).
static double e_max_at_square(const SingleTileSquare *sq,
                              const LetterDistribution *ld,
                              const uint8_t *leave_counts,
                              const uint8_t *pool_counts, int pool_size,
                              int draw_size, uint64_t denom_eff,
                              int effective_d) {
  ScoreBin bins[MAX_ALPHABET_SIZE];
  int num_bins = 0;
  int playable_leave = 0;
  int playable_pool = 0;

  uint64_t bits = sq->playable_letters;
  while (bits != 0) {
    const int ml = __builtin_ctzll(bits);
    bits &= bits - 1;
    const int lc = leave_counts[ml];
    const int pc = pool_counts[ml];
    if (lc + pc == 0) {
      continue;
    }
    playable_leave += lc;
    playable_pool += pc;
    const Equity face = ld_get_score(ld, (MachineLetter)ml);
    const Equity score = sq->base + face * sq->coef;
    int found = -1;
    for (int b = 0; b < num_bins; b++) {
      if (bins[b].score == score) {
        found = b;
        break;
      }
    }
    if (found >= 0) {
      bins[found].leave_count += lc;
      bins[found].pool_count += pc;
    } else {
      bins[num_bins].score = score;
      bins[num_bins].leave_count = lc;
      bins[num_bins].pool_count = pc;
      num_bins++;
    }
  }

  if (num_bins == 0) {
    return 0.0;
  }

  // Insertion sort ascending.
  for (int i = 1; i < num_bins; i++) {
    ScoreBin tmp = bins[i];
    int j = i - 1;
    while (j >= 0 && bins[j].score > tmp.score) {
      bins[j + 1] = bins[j];
      j--;
    }
    bins[j + 1] = tmp;
  }

  // leave_above[i] / pool_above[i] = counts strictly above bins[i].
  int leave_above[MAX_ALPHABET_SIZE];
  int pool_above[MAX_ALPHABET_SIZE];
  leave_above[num_bins - 1] = 0;
  pool_above[num_bins - 1] = 0;
  for (int i = num_bins - 2; i >= 0; i--) {
    leave_above[i] = leave_above[i + 1] + bins[i + 1].leave_count;
    pool_above[i] = pool_above[i + 1] + bins[i + 1].pool_count;
  }

  // Implicit P(max <= 0) — covers the case where the rack contains no
  // tiles playable at this square. Counts "above 0" = playable counts.
  double prev_p;
  if (playable_leave > 0) {
    prev_p = 0.0;
  } else {
    prev_p = (double)binomial_u64(pool_size - playable_pool, effective_d) /
             (double)denom_eff;
  }

  double e_max = 0.0;
  for (int i = 0; i < num_bins; i++) {
    double p = 0.0;
    if (leave_above[i] == 0) {
      p = (double)binomial_u64(pool_size - pool_above[i], effective_d) /
          (double)denom_eff;
    }
    e_max += (double)bins[i].score * (p - prev_p);
    prev_p = p;
  }
  return e_max;
}

// ---------------------------------------------------------------------
// Closed-form features
// ---------------------------------------------------------------------

// Build distinct-score bins from per-letter scores. Always includes a
// score=0 bin for non-playable rack tiles. Returns num_bins.
static int build_per_letter_bins(const Equity *score_per_letter,
                                 const uint8_t *leave_counts,
                                 const uint8_t *pool_counts, ScoreBin *bins) {
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
    const Equity s = score_per_letter[ml];
    int found = -1;
    for (int b = 0; b < num_bins; b++) {
      if (bins[b].score == s) {
        found = b;
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

// E[max over rack of score_per_letter[L]] via standard per-letter
// order statistics. Used for closed-form E[top1].
static double e_max_per_letter(const Equity *score_per_letter,
                               const uint8_t *leave_counts,
                               const uint8_t *pool_counts, int pool_size,
                               int draw_size, uint64_t denom_eff,
                               int effective_d) {
  ScoreBin bins[MAX_ALPHABET_SIZE + 1];
  const int num_bins =
      build_per_letter_bins(score_per_letter, leave_counts, pool_counts, bins);

  int leave_above[MAX_ALPHABET_SIZE + 1];
  int pool_above[MAX_ALPHABET_SIZE + 1];
  leave_above[num_bins - 1] = 0;
  pool_above[num_bins - 1] = 0;
  for (int i = num_bins - 2; i >= 0; i--) {
    leave_above[i] = leave_above[i + 1] + bins[i + 1].leave_count;
    pool_above[i] = pool_above[i + 1] + bins[i + 1].pool_count;
  }

  double e_max = 0.0;
  double prev_p = 0.0;
  for (int i = 0; i < num_bins; i++) {
    double p = 0.0;
    if (leave_above[i] == 0) {
      p = (double)binomial_u64(pool_size - pool_above[i], effective_d) /
          (double)denom_eff;
    }
    e_max += (double)bins[i].score * (p - prev_p);
    prev_p = p;
  }
  return e_max;
}

void single_tile_features(const SingleTileScan *scan,
                          const LetterDistribution *ld,
                          const uint8_t *leave_counts,
                          const uint8_t *pool_counts, int pool_size,
                          int draw_size, int rack_size,
                          SingleTileFeatures *out) {
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

  const uint64_t denom = binomial_u64(pool_size, draw_size);
  const int effective_d = (denom == 0) ? 0 : draw_size;
  const uint64_t denom_eff = (denom == 0) ? 1 : denom;

  // E[top1] = E[max over rack of best_score[L]] — exact via per-letter
  // order statistics.
  out->e_top1 = e_max_per_letter(scan->best_score, leave_counts, pool_counts,
                                 pool_size, draw_size, denom_eff, effective_d);

  // E[top2] proxy: per-square E[max], then 2nd-largest across squares.
  // This is a Jensen-biased approximation of E[2nd-largest per-square
  // max across distinct squares].
  double top2 = 0.0;
  double bestseen = 0.0;
  for (int i = 0; i < scan->num_squares; i++) {
    const double e_max =
        e_max_at_square(&scan->squares[i], ld, leave_counts, pool_counts,
                        pool_size, draw_size, denom_eff, effective_d);
    if (e_max > bestseen) {
      top2 = bestseen;
      bestseen = e_max;
    } else if (e_max > top2) {
      top2 = e_max;
    }
  }
  out->e_top2 = top2;
}

// ---------------------------------------------------------------------
// Monte Carlo
// ---------------------------------------------------------------------

// Draws draw_size tiles from pool_counts (without replacement) into
// drawn_counts. scratch_counts must be size MAX_ALPHABET_SIZE; gets
// trashed.
static void draw_random(XoshiroPRNG *prng, const uint8_t *pool_counts,
                        int pool_size, int draw_size, uint8_t *drawn_counts,
                        uint16_t *scratch_counts) {
  for (int ml = 0; ml < MAX_ALPHABET_SIZE; ml++) {
    scratch_counts[ml] = pool_counts[ml];
    drawn_counts[ml] = 0;
  }
  int remaining = pool_size;
  for (int i = 0; i < draw_size; i++) {
    const uint64_t pick = prng_get_random_number(prng, (uint64_t)remaining);
    uint64_t cum = 0;
    for (int ml = 0; ml < MAX_ALPHABET_SIZE; ml++) {
      cum += scratch_counts[ml];
      if (pick < cum) {
        drawn_counts[ml]++;
        scratch_counts[ml]--;
        remaining--;
        break;
      }
    }
  }
}

// Per-rack: for each square, find the max score over rack tiles at that
// square. Returns (top1, top2) over distinct squares via the out args.
static void per_rack_top12(const SingleTileScan *scan,
                           const LetterDistribution *ld,
                           const uint8_t *rack_counts, double *out_top1,
                           double *out_top2) {
  double top1 = 0.0;
  double top2 = 0.0;
  for (int i = 0; i < scan->num_squares; i++) {
    const SingleTileSquare *sq = &scan->squares[i];
    Equity best_at_sq = 0;
    uint64_t bits = sq->playable_letters;
    while (bits != 0) {
      const int ml = __builtin_ctzll(bits);
      bits &= bits - 1;
      if (rack_counts[ml] == 0) {
        continue;
      }
      const Equity face = ld_get_score(ld, (MachineLetter)ml);
      const Equity score = sq->base + face * sq->coef;
      if (score > best_at_sq) {
        best_at_sq = score;
      }
    }
    const double v = (double)best_at_sq;
    if (v > top1) {
      top2 = top1;
      top1 = v;
    } else if (v > top2) {
      top2 = v;
    }
  }
  *out_top1 = top1;
  *out_top2 = top2;
}

void single_tile_features_mc(const SingleTileScan *scan,
                             const LetterDistribution *ld,
                             const uint8_t *leave_counts,
                             const uint8_t *pool_counts, int pool_size,
                             int draw_size, int rack_size, uint64_t samples,
                             uint64_t seed, SingleTileFeatures *out) {
  // frac_playable is exact via linearity; no MC needed.
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

  if (samples == 0) {
    out->e_top1 = 0.0;
    out->e_top2 = 0.0;
    return;
  }

  XoshiroPRNG *prng = prng_create(seed);
  uint8_t drawn_counts[MAX_ALPHABET_SIZE];
  uint16_t scratch_counts[MAX_ALPHABET_SIZE];
  uint8_t rack_counts[MAX_ALPHABET_SIZE];

  double sum_top1 = 0.0;
  double sum_top2 = 0.0;
  for (uint64_t s = 0; s < samples; s++) {
    if (draw_size > 0 && pool_size >= draw_size) {
      draw_random(prng, pool_counts, pool_size, draw_size, drawn_counts,
                  scratch_counts);
    } else {
      memset(drawn_counts, 0, sizeof(drawn_counts));
    }
    for (int ml = 0; ml < MAX_ALPHABET_SIZE; ml++) {
      rack_counts[ml] = (uint8_t)(leave_counts[ml] + drawn_counts[ml]);
    }
    double top1;
    double top2;
    per_rack_top12(scan, ld, rack_counts, &top1, &top2);
    sum_top1 += top1;
    sum_top2 += top2;
  }
  out->e_top1 = sum_top1 / (double)samples;
  out->e_top2 = sum_top2 / (double)samples;
  prng_destroy(prng);
}
