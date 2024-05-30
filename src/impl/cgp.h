#ifndef CGP_H
#define CGP_H

#include "../def/game_defs.h"

#include "../ent/game.h"

cgp_parse_status_t game_load_cgp(Game *game, const char *cgp);
char *game_get_cgp(const Game *game, bool write_player_on_turn_first);
char *game_get_cgp_with_options(const Config *config, const Game *game,
                                bool write_player_on_turn_first);

#endif
