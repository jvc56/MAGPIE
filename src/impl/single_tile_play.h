#ifndef SINGLE_TILE_PLAY_H
#define SINGLE_TILE_PLAY_H

#include "../def/board_defs.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/letter_distribution.h"
#include <stdint.h>

// Per-square data: playable letters and score parameters such that
// score(square, letter L) = base + face_value(L) * coef.
//
// base = (h_run_score * has_h_word + v_run_score * has_v_word) * word_mult
// coef = letter_mult * word_mult * (has_h_word + has_v_word)
//
// playable_letters is cross_set[H] & cross_set[V] for the square (with
// the blank bit set whenever any non-blank bit is set, per MAGPIE's
// cross-set-with-blank convention).
typedef struct {
  uint64_t playable_letters;
  Equity base;
  Equity coef;
} SingleTileSquare;

// Result of one pass over the board. Caller-allocated (no heap).
typedef struct {
  uint64_t playable_set; // OR of playable_letters across all squares
  // best_score[L] = max over all squares of score(square, L). Used for
  // the closed-form E[top1] computation (which is exact via per-letter
  // order statistics on this array).
  Equity best_score[MAX_ALPHABET_SIZE];
  int num_squares;
  SingleTileSquare squares[BOARD_DIM * BOARD_DIM];
} SingleTileScan;

// Scans every empty, non-stranded square once. Caller must ensure
// cross_sets are valid (true after every move in classic gameplay).
void single_tile_scan(const Game *game, SingleTileScan *scan);

// Features describing the distribution of single-tile-play scores for a
// rack of the form (known leave) + (random hypergeometric draw of size
// draw_size from a pool of size pool_size).
//
//   frac_playable: E[# tiles in rack that have ANY single-tile play
//                    available somewhere on the board] / rack_size
//   e_top1:        E[max single-tile play score over the whole rack]
//   e_top2:        E[2nd-largest single-tile play score across DISTINCT
//                    squares, where each square's contribution is the
//                    best score achievable with any rack tile at that
//                    square]
//
// e_top2 is the "what's the next best play if my top play is blocked?"
// notion — distinct from "2nd-best tile in my rack", which would
// double-count tile copies playing the same square.
typedef struct {
  double frac_playable;
  double e_top1;
  double e_top2;
} SingleTileFeatures;

// Closed-form computation. Per square, computes E[max over rack of
// score(s,L)] via the same hypergeometric order-stat machinery, then
// takes top-2 across squares. Note: top-2 of the per-square E[max]
// values is *not* identical to E[top-2 across squares] for random
// racks (Jensen-biased upward); use single_tile_features_mc to
// quantify the bias.
void single_tile_features(const SingleTileScan *scan,
                          const LetterDistribution *ld,
                          const uint8_t *leave_counts,
                          const uint8_t *pool_counts, int pool_size,
                          int draw_size, int rack_size,
                          SingleTileFeatures *out);

// Monte Carlo: draws `samples` random racks (leave + draw_size random
// tiles without replacement from pool), for each rack computes per-
// square max and the per-rack top-2 across distinct squares, averages.
// Slow (~hundreds of microseconds for thousands of samples) but exact
// in the limit.
void single_tile_features_mc(const SingleTileScan *scan,
                             const LetterDistribution *ld,
                             const uint8_t *leave_counts,
                             const uint8_t *pool_counts, int pool_size,
                             int draw_size, int rack_size, uint64_t samples,
                             uint64_t seed, SingleTileFeatures *out);

#endif
