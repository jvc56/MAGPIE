
#ifndef GCG_H
#define GCG_H

#include "../ent/game_history.h"
#include "../util/error_stack.h"

#include "config.h"

void parse_gcg(const char *gcg_filename, Config *config,
               GameHistory *game_history, ErrorStack *error_stack);
void parse_gcg_string(const char *input_gcg_string, Config *config,
                      GameHistory *game_history, ErrorStack *error_stack);
void game_play_to_turn(GameHistory *game_history, Game *game, int turn_index,
                       ErrorStack *error_stack);
void game_play_to_end(GameHistory *game_history, Game *game,
                      ErrorStack *error_stack);

#endif
