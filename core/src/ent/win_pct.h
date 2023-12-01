#ifndef WIN_PCT_H
#define WIN_PCT_H

struct WinPct;
typedef struct WinPct WinPct;

WinPct *create_win_pct(const char *win_pct_name);
void destroy_win_pct(WinPct *wp);
float win_pct(const WinPct *wp, int spread_plus_leftover,
              unsigned int tiles_unseen);
#endif