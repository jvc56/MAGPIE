#ifndef GAME_STRING_H
#define GAME_STRING_H

#include "../ent/game.h"
#include "../ent/move.h"
#include "../util/string_util.h"

typedef enum {
  GAME_STRING_BOARD_COLOR_NONE,
  GAME_STRING_BOARD_COLOR_ANSI,
  GAME_STRING_BOARD_COLOR_XTERM_256,
  GAME_STRING_BOARD_COLOR_TRUECOLOR,
} game_string_board_color_t;

typedef enum {
  GAME_STRING_BOARD_TILE_GLYPHS_PRIMARY,
  GAME_STRING_BOARD_TILE_GLYPHS_ALT,
} game_string_board_tile_glyphs_t;

typedef enum {
  GAME_STRING_BOARD_BORDER_ASCII,
  GAME_STRING_BOARD_BORDER_BOX_DRAWING,
} game_string_board_border_t;

typedef enum {
  GAME_STRING_BOARD_COLUMN_LABEL_ASCII,
  GAME_STRING_BOARD_COLUMN_LABEL_FULLWIDTH,
} game_string_board_column_label_t;

typedef enum {
  GAME_STRING_ON_TURN_MARKER_ASCII,
  GAME_STRING_ON_TURN_MARKER_ARROWHEAD,
} game_string_on_turn_marker_t;

typedef enum {
  GAME_STRING_ON_TURN_COLOR_NONE,
  GAME_STRING_ON_TURN_COLOR_ANSI_GREEN,
} game_string_on_turn_color_t;

typedef enum {
  GAME_STRING_ON_TURN_SCORE_NORMAL,
  GAME_STRING_ON_TURN_SCORE_BOLD,
} game_string_on_turn_score_style_t;

struct GameStringOptions {
  game_string_board_color_t board_color;
  game_string_board_tile_glyphs_t board_tile_glyphs;
  game_string_board_border_t board_border;
  game_string_board_column_label_t board_column_label;
  game_string_on_turn_marker_t on_turn_marker;
  game_string_on_turn_color_t on_turn_color;
  game_string_on_turn_score_style_t on_turn_score_style;
};

typedef struct GameStringOptions GameStringOptions;

void string_builder_add_game(const Game *game, const MoveList *move_list,
                             const GameStringOptions *game_string_options,
                             StringBuilder *game_string);

char *ucgi_static_moves(const Game *game, const MoveList *move_list);

void print_ucgi_static_moves(Game *game, MoveList *move_list,
                             ThreadControl *thread_control);

GameStringOptions *game_string_options_create_default();

GameStringOptions *game_string_options_create(
    game_string_board_color_t board_color,
    game_string_board_tile_glyphs_t board_tile_glyphs,
    game_string_board_border_t board_border,
    game_string_board_column_label_t board_column_label,
    game_string_on_turn_marker_t on_turn_marker,
    game_string_on_turn_color_t on_turn_color,
    game_string_on_turn_score_style_t on_turn_score_style);

void game_string_options_destroy(GameStringOptions *gso);

#endif
