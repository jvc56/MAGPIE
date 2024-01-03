#ifndef WIN_PCT_H
#define WIN_PCT_H

struct WinPct;
typedef struct WinPct WinPct;

WinPct *win_pct_create(const char *win_pct_name);
void win_pct_destroy(WinPct *wp);
float win_pct_get(const WinPct *wp, int spread_plus_leftover,
                  unsigned int game_get_unseen_tiles);
#endif