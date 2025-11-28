#ifndef GAME_STRING_H
#define GAME_STRING_H

#include "../ent/endgame_results.h"
#include "../ent/game.h"
#include "../ent/game_history.h"
#include "../ent/move.h"
#include "../ent/thread_control.h"
#include "../util/string_util.h"

typedef enum {
  GAME_STRING_BOARD_COLOR_NONE,
  GAME_STRING_BOARD_COLOR_ANSI,
  GAME_STRING_BOARD_COLOR_XTERM_256,
  GAME_STRING_BOARD_COLOR_TRUECOLOR,
} game_string_board_color_t;

#define GAME_STRING_BOARD_COLOR_NONE_STRING "none"
#define GAME_STRING_BOARD_COLOR_ANSI_STRING "ansi"
#define GAME_STRING_BOARD_COLOR_XTERM_256_STRING "xterm"
#define GAME_STRING_BOARD_COLOR_TRUECOLOR_STRING "truecolor"

typedef enum {
  GAME_STRING_BOARD_TILE_GLYPHS_PRIMARY,
  GAME_STRING_BOARD_TILE_GLYPHS_ALT,
} game_string_board_tile_glyphs_t;

#define GAME_STRING_BOARD_TILE_GLYPHS_PRIMARY_STRING "primary"
#define GAME_STRING_BOARD_TILE_GLYPHS_ALT_STRING "alt"

typedef enum {
  GAME_STRING_BOARD_BORDER_ASCII,
  GAME_STRING_BOARD_BORDER_BOX_DRAWING,
} game_string_board_border_t;

#define GAME_STRING_BOARD_BORDER_ASCII_STRING "ascii"
#define GAME_STRING_BOARD_BORDER_BOX_DRAWING_STRING "box"

typedef enum {
  GAME_STRING_BOARD_COLUMN_LABEL_ASCII,
  GAME_STRING_BOARD_COLUMN_LABEL_FULLWIDTH,
} game_string_board_column_label_t;

#define GAME_STRING_BOARD_COLUMN_LABEL_ASCII_STRING "ascii"
#define GAME_STRING_BOARD_COLUMN_LABEL_FULLWIDTH_STRING "fullwidth"

typedef enum {
  GAME_STRING_ON_TURN_MARKER_ASCII,
  GAME_STRING_ON_TURN_MARKER_ARROWHEAD,
} game_string_on_turn_marker_t;

#define GAME_STRING_ON_TURN_MARKER_ASCII_STRING "ascii"
#define GAME_STRING_ON_TURN_MARKER_ARROWHEAD_STRING "arrowhead"

typedef enum {
  GAME_STRING_ON_TURN_COLOR_NONE,
  GAME_STRING_ON_TURN_COLOR_ANSI_GREEN,
} game_string_on_turn_color_t;

#define GAME_STRING_ON_TURN_COLOR_NONE_STRING "none"
#define GAME_STRING_ON_TURN_COLOR_ANSI_GREEN_STRING "green"

typedef enum {
  GAME_STRING_ON_TURN_SCORE_NORMAL,
  GAME_STRING_ON_TURN_SCORE_BOLD,
} game_string_on_turn_score_style_t;

#define GAME_STRING_ON_TURN_SCORE_NORMAL_STRING "normal"
#define GAME_STRING_ON_TURN_SCORE_BOLD_STRING "bold"

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
                             const GameHistory *game_history,
                             StringBuilder *game_string);

GameStringOptions *game_string_options_create_default(void);

GameStringOptions *game_string_options_create(
    game_string_board_color_t board_color,
    game_string_board_tile_glyphs_t board_tile_glyphs,
    game_string_board_border_t board_border,
    game_string_board_column_label_t board_column_label,
    game_string_on_turn_marker_t on_turn_marker,
    game_string_on_turn_color_t on_turn_color,
    game_string_on_turn_score_style_t on_turn_score_style);

void game_string_options_destroy(GameStringOptions *gso);

void string_builder_add_game_variant(StringBuilder *sb, game_variant_t variant);

#endif
