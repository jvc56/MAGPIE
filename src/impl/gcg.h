
#ifndef GCG_H
#define GCG_H

#include "../def/config_defs.h"
#include "../def/gcg_defs.h"

#include "../ent/config.h"
#include "../ent/game.h"
#include "../ent/game_history.h"

gcg_parse_status_t parse_gcg(const char *gcg_filename,
                             GameHistory *game_history);
gcg_parse_status_t parse_gcg_string(const char *input_gcg_string,
                                    GameHistory *game_history);
config_load_status_t
load_config_with_game_history(const GameHistory *game_history, Config *config);
gcg_parse_status_t play_game_history_to_turn(const GameHistory *game_history,
                                             Game *game, int final_turn_number);
#endif
