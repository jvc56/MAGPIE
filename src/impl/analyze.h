#ifndef ANALYZE_H
#define ANALYZE_H

#include "../ent/game.h"
#include "../ent/game_history.h"
#include "../ent/thread_control.h"
#include "../ent/win_pct.h"
#include "../util/io_util.h"
#include <stdbool.h>

// Analyzes all turns of a single GCG game for the players indicated by
// player_mask. Internal analysis structures (Game, MoveList, SimResults, etc.)
// are created on first call and reused across subsequent calls via static
// storage inside analyze.c.
void analyze_game(GameHistory *game_history, const GameArgs *game_args,
                  WinPct *win_pcts, ThreadControl *thread_control,
                  int player_mask, int num_threads, int num_plays,
                  int sim_plies, int endgame_plies,
                  double tt_fraction_of_mem, double stop_cond_pct,
                  bool sim_with_inference, bool human_readable,
                  int max_num_display_plays, const char *report_path,
                  ErrorStack *error_stack);

#endif
