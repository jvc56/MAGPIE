#ifndef ENDGAME_STRING_H
#define ENDGAME_STRING_H

#include "../ent/endgame_results.h"
#include "../ent/game.h"
#include "../ent/game_history.h"
#include <stdbool.h>

char *endgame_results_get_string(EndgameResults *results, const Game *game,
                                 const GameHistory *game_history,
                                 bool add_line_breaks);

#endif
