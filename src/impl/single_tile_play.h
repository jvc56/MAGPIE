#ifndef SINGLE_TILE_PLAY_H
#define SINGLE_TILE_PLAY_H

#include "../def/letter_distribution_defs.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include <stdint.h>

// Per-letter best single-tile play data, computed once per board state by
// scanning the precomputed cross_sets and cross_scores. No KWG/WMP walks.
//
// best_score[L] is the maximum score achievable by placing letter L on any
// single empty square, considering both the main and cross words formed.
// 0 means L cannot be played as a single tile anywhere on the board.
//
// playable_set is the union over the board of letters that have any
// single-tile play available. Bit 0 (the blank) is set whenever any other
// letter is set (since the blank can stand for any letter).
typedef struct {
  Equity best_score[MAX_ALPHABET_SIZE];
  uint64_t playable_set;
} SingleTileScan;

// Scans every empty, non-stranded square once and updates scan->best_score
// and scan->playable_set. Caller must ensure cross_sets are valid (true
// after every move in classic gameplay).
void single_tile_scan(const Game *game, SingleTileScan *scan);

// Features describing the distribution of a 7-tile rack composed of a known
// leave plus a random draw without replacement from a pool, restricted to
// single-tile plays:
//
//   E[fraction of rack tiles playable as single tiles]
//   E[max single-tile play score across the rack]
//   E[2nd-highest single-tile play score across the rack]
//
// Reduces to deterministic max/2nd-max when draw_size == 0 (full leave) and
// to a pure 7-tile draw when leave is empty and draw_size == rack_size.
typedef struct {
  double frac_playable;
  double e_max_score;     // equity units (millipoints)
  double e_2nd_max_score; // equity units (millipoints)
} SingleTileFeatures;

// leave_counts and pool_counts are length-MAX_ALPHABET_SIZE arrays of
// per-letter counts. pool_size = sum of pool_counts; rack_size is the total
// rack size (leave_size + draw_size; typically RACK_SIZE = 7).
//
// If pool_size < draw_size the result is undefined (caller bug).
void single_tile_features(const SingleTileScan *scan,
                          const uint8_t *leave_counts,
                          const uint8_t *pool_counts, int pool_size,
                          int draw_size, int rack_size,
                          SingleTileFeatures *out);

#endif
