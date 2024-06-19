#ifndef GAME_STRING_H
#define GAME_STRING_H

#include "../ent/game.h"
#include "../ent/move.h"
#include "../ent/thread_control.h"

#include "../util/string_util.h"

void string_builder_add_game_variant(StringBuilder *sb,
                                     game_variant_t game_variant_type);

void string_builder_add_game(Game *game, MoveList *move_list,
                             StringBuilder *game_string);

char *ucgi_static_moves(const Game *game, const MoveList *move_list);

void print_ucgi_static_moves(Game *game, MoveList *move_list,
                             ThreadControl *thread_control);

#endif
