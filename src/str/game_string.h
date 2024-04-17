#ifndef GAME_STRING_H
#define GAME_STRING_H

#include "../ent/game.h"
#include "../ent/move.h"

#include "../util/string_util.h"

typedef enum {
    GAME_STRING_BOARD_COLOR_NONE,
    GAME_STRING_BOARD_COLOR_ANSI,
} game_string_board_color_t;

struct GameStringOptions {
  game_string_board_color_t board_color;
};

typedef struct GameStringOptions GameStringOptions;

void string_builder_add_game(const Game *game, const MoveList *move_list,
                             const GameStringOptions *game_string_options,
                             StringBuilder *game_string);

char *ucgi_static_moves(const Game *game, const MoveList *move_list);

void print_ucgi_static_moves(Game *game, MoveList *move_list,
                             ThreadControl *thread_control);

GameStringOptions *game_string_options_create_default();

GameStringOptions *game_string_options_create(game_string_board_color_t board_color);

void game_string_options_destroy(GameStringOptions *gso);

#endif
