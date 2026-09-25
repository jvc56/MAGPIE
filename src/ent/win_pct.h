#ifndef WIN_PCT_H
#define WIN_PCT_H

#include "../util/io_util.h"
#include "equity.h"
#include <stdbool.h>

typedef struct WinPct WinPct;

WinPct *win_pct_create(const char *data_paths, const char *win_pct_name,
                       ErrorStack *error_stack);
void win_pct_destroy(WinPct *wp);
const char *win_pct_get_name(const WinPct *wp);
// The largest bag size the table covers.
unsigned int win_pct_get_max_bag(const WinPct *wp);
// The on-turn player's chance of winning when ahead by spread_plus_leftover,
// given the tiles in the bag and on each rack (see win_pct_counts.h).
float win_pct_get(const WinPct *wp, int spread_plus_leftover,
                  unsigned int bag_tiles, unsigned int on_turn_rack_tiles,
                  unsigned int off_turn_rack_tiles);
// The expected change in the on-turn player's spread from this state to the
// end of the game.
Equity win_pct_get_expected_swing(const WinPct *wp, unsigned int bag_tiles,
                                  unsigned int on_turn_rack_tiles,
                                  unsigned int off_turn_rack_tiles);
bool is_win_pct_within_cutoff(const double win_pct, const double cutoff);
bool are_win_pcts_within_cutoff_or_equal(const double wp1, const double wp2,
                                         const double cutoff);
double convert_cutoff_to_user_cutoff(const double cutoff);
double convert_user_cutoff_to_cutoff(const double user_cutoff);

#endif