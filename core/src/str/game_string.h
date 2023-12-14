#ifndef GAME_STRING_H
#define GAME_STRING_H

#include "../ent/game.h"
#include "../ent/move.h"

#include "../util/string_util.h"

void string_builder_add_game(Game *game, StringBuilder *game_string);

char *ucgi_static_moves(Game *game);

void print_ucgi_static_moves(Game *game, ThreadControl *thread_control);

#endif
