#ifndef WIN_PCT_H
#define WIN_PCT_H

#include "../util/io_util.h"
#include <stdbool.h>

typedef struct WinPct WinPct;

WinPct *win_pct_create(const char *data_paths, const char *win_pct_name,
                       ErrorStack *error_stack);
void win_pct_destroy(WinPct *wp);
const char *win_pct_get_name(const WinPct *wp);
float win_pct_get(const WinPct *wp, int spread_plus_leftover,
                  unsigned int game_unseen_tiles);
bool is_win_pct_within_cutoff(const double win_pct, const double cutoff);
bool are_win_pcts_within_cutoff_or_equal(const double wp1, const double wp2,
                                         const double cutoff);
double convert_cutoff_to_user_cutoff(const double cutoff);
double convert_user_cutoff_to_cutoff(const double user_cutoff);

#endif