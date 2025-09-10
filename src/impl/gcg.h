
#ifndef GCG_H
#define GCG_H

#include "../ent/game_history.h"
#include "../util/io_util.h"
#include "config.h"

void parse_gcg(const char *gcg_filename, Config *config,
               GameHistory *game_history, ErrorStack *error_stack);
void parse_gcg_string(const char *input_gcg_string, Config *config,
                      GameHistory *game_history, ErrorStack *error_stack);
// FIXME: move this to gameplay
void game_play_to_event_index(GameHistory *game_history, Game *game,
                              int event_index, ErrorStack *error_stack);
// FIXME: move this to gameplay
void game_play_to_end(GameHistory *game_history, Game *game,
                      ErrorStack *error_stack);
void write_gcg(const char *gcg_filename, const LetterDistribution *ld,
               GameHistory *game_history, ErrorStack *error_stack);

char *gcg_next(GameHistory *game_history, Game *game, ErrorStack *error_stack);
char *gcg_previous(GameHistory *game_history, Game *game,
                   ErrorStack *error_stack);
char *gcg_goto(GameHistory *game_history, Game *game, int index,
               ErrorStack *error_stack);

#endif
