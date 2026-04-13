#ifndef ENDGAME_STRING_H
#define ENDGAME_STRING_H

#include "../ent/endgame_results.h"
#include "../ent/game.h"
#include "../ent/game_history.h"
#include "../util/string_util.h"

// Formats a single PVLine (by index into the multi-PV array, or the current
// best PV when num_pvs == 0) into sb as a per-move table with Player, Move,
// Score, Value, and Spread columns.
void string_builder_endgame_single_pv(StringBuilder *sb,
                                      EndgameResults *endgame_results,
                                      const Game *source_game,
                                      const GameHistory *game_history,
                                      int pv_idx);

char *endgame_results_get_string(EndgameResults *results, const Game *game,
                                 const GameHistory *game_history);

#endif
