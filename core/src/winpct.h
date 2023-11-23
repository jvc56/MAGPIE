#ifndef WIN_PCT_H
#define WIN_PCT_H

// ugly and hard-coded for right now
#define MAX_SPREAD 300
#define MAX_COLS 94

typedef struct WinPct {
  float **win_pcts;
  int min_spread;
  int max_spread;
  int number_of_spreads;
  unsigned int max_tiles_unseen;
} WinPct;

WinPct *create_winpct(const char *winpct_name);
void destroy_winpct(WinPct *wp);

// FIXME: should this be inlined?
inline float win_pct(const WinPct *wp, int spread_plus_leftover,
                     unsigned int tiles_unseen) {
  if (spread_plus_leftover > wp->max_spread) {
    spread_plus_leftover = wp->max_spread;
  }
  if (spread_plus_leftover < wp->min_spread) {
    spread_plus_leftover = wp->min_spread;
  }
  if (tiles_unseen > wp->max_tiles_unseen) {
    tiles_unseen = wp->max_tiles_unseen;
  }
  return wp->win_pcts[MAX_SPREAD - spread_plus_leftover][tiles_unseen];
}

#endif