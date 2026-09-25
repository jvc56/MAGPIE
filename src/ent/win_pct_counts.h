#ifndef WIN_PCT_COUNTS_H
#define WIN_PCT_COUNTS_H

#include "../def/rack_defs.h"
#include "../util/io_util.h"
#include <stdbool.h>

// Observed outcomes of self-play games, the data behind a win percentage
// table. For each game state and each of WIN_PCT_NUM_SAMPLES independent
// samples of games, a row holds the histogram of the swing: the change in the
// on-turn player's spread from that state to the end of the game.
//
// A state is the bag size and the two rack sizes. While the bag holds tiles
// both racks are full, so those states are keyed by the bag size alone; once
// the bag is empty the rack sizes decide who is likely to go out first and how
// many turns each player gets, so each (on-turn rack, off-turn rack) pair is
// its own state. Cells are ordered: the RACK_SIZE * RACK_SIZE bag-empty states
// (on-turn rack size major), then bag sizes 1..max_bag.
//
// Histogram bin b counts swings of b - (max_spread + 1). The first and last
// bins also count every swing beyond them, so the win percentage is exact for
// spreads within +/- max_spread. Each row ends with the sum of the swings, so
// the expected swing is exact too. Rows are doubles so that smoothed tables
// share the format; recorded tables hold integers.
//
// Text format:
//   winpct 2 <rack size> <max bag> <max spread>
//   <sample> <bag> <on-turn rack> <off-turn rack> <swing sum> <bins...>
// Missing rows are zero.

enum {
  WIN_PCT_NUM_SAMPLES = 2,
  WIN_PCT_COUNTS_VERSION = 2,
  WIN_PCT_NUM_BAG_EMPTY_CELLS = RACK_SIZE * RACK_SIZE,
};

typedef struct WinPctCounts WinPctCounts;

WinPctCounts *win_pct_counts_create(int max_bag, int max_spread);
WinPctCounts *win_pct_counts_duplicate(const WinPctCounts *counts);
void win_pct_counts_destroy(WinPctCounts *counts);

int win_pct_counts_get_max_bag(const WinPctCounts *counts);
int win_pct_counts_get_max_spread(const WinPctCounts *counts);
int win_pct_counts_get_num_cells(const WinPctCounts *counts);
int win_pct_counts_get_num_bins(const WinPctCounts *counts);
// Bins plus the trailing swing sum.
int win_pct_counts_get_row_length(const WinPctCounts *counts);

// Clamps rack sizes of bag-empty states to 1..RACK_SIZE and ignores rack sizes
// when the bag holds tiles. The bag size must not exceed the maximum.
static inline int win_pct_get_cell_index(int bag, int on_turn_rack_tiles,
                                         int off_turn_rack_tiles) {
  if (bag > 0) {
    return WIN_PCT_NUM_BAG_EMPTY_CELLS + bag - 1;
  }
  if (on_turn_rack_tiles < 1) {
    on_turn_rack_tiles = 1;
  } else if (on_turn_rack_tiles > RACK_SIZE) {
    on_turn_rack_tiles = RACK_SIZE;
  }
  if (off_turn_rack_tiles < 1) {
    off_turn_rack_tiles = 1;
  } else if (off_turn_rack_tiles > RACK_SIZE) {
    off_turn_rack_tiles = RACK_SIZE;
  }
  return (on_turn_rack_tiles - 1) * RACK_SIZE + off_turn_rack_tiles - 1;
}

// The bag size of a cell (0 for the bag-empty states).
int win_pct_counts_get_cell_bag(int cell_index);

double *win_pct_counts_get_row(WinPctCounts *counts, int sample, int cell);
const double *win_pct_counts_get_const_row(const WinPctCounts *counts,
                                           int sample, int cell);

// Records one observation of a swing.
void win_pct_counts_add_swing(WinPctCounts *counts, int sample, int cell,
                              int swing);
// Adds src's rows to dst's. The tables must have the same shape.
void win_pct_counts_add(WinPctCounts *dst, const WinPctCounts *src);
bool win_pct_counts_have_same_shape(const WinPctCounts *counts1,
                                    const WinPctCounts *counts2);
void win_pct_counts_reset(WinPctCounts *counts);

// Row statistics. A row's total is the sum of its bins.
double win_pct_counts_row_get_total(const WinPctCounts *counts,
                                    const double *row);
double win_pct_counts_row_get_mean_swing(const WinPctCounts *counts,
                                         const double *row);
// Writes the on-turn player's win probability (ties count half) for each
// current spread -max_spread..max_spread into win_pcts. The row's total must
// be positive.
void win_pct_counts_row_get_win_pcts(const WinPctCounts *counts,
                                     const double *row, double *win_pcts);

// Whether a win percentage file's contents are in this format rather than the
// original one keyed by unseen tiles.
bool win_pct_counts_string_has_header(const char *contents);
WinPctCounts *win_pct_counts_create_from_string(const char *contents,
                                                const char *filename,
                                                ErrorStack *error_stack);
char *win_pct_counts_get_string(const WinPctCounts *counts);

#endif
