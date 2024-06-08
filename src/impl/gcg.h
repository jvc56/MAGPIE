
#ifndef GCG_H
#define GCG_H

#include "../def/gcg_defs.h"

#include "../ent/game_history.h"
#include "config.h"

gcg_parse_status_t parse_gcg(const char *gcg_filename, Config *config,
                             GameHistory *game_history);
gcg_parse_status_t parse_gcg_string(const char *input_gcg_string,
                                    Config *config, GameHistory *game_history);
void game_play_to_turn(GameHistory *game_history, Game *game, int turn_index);
void game_play_to_end(GameHistory *game_history, Game *game);

#endif
