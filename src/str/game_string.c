#include "game_string.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include "../def/board_defs.h"
#include "../def/game_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../ent/bag.h"
#include "../ent/board.h"
#include "../ent/bonus_square.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/player.h"
#include "../ent/rack.h"
#include "../ent/thread_control.h"
#include "../util/string_util.h"
#include "bag_string.h"
#include "letter_distribution_string.h"
#include "move_string.h"
#include "rack_string.h"

bool should_print_escape_codes(const GameStringOptions *game_string_options) {
  if (game_string_options == NULL) {
    return false;
  }
  return isatty(fileno(stdout));
}

bool use_ascii_on_turn_marker(const GameStringOptions *game_string_options) {
  if (game_string_options == NULL) {
    return true;
  }
  return game_string_options->on_turn_marker ==
         GAME_STRING_ON_TURN_MARKER_ASCII;
}

void string_builder_add_player_on_turn_color(
    StringBuilder *game_string, const GameStringOptions *game_string_options) {
  if (game_string_options == NULL) {
    return;
  }
  if (game_string_options->on_turn_color ==
      GAME_STRING_ON_TURN_COLOR_ANSI_GREEN) {
    string_builder_add_string(game_string, "\x1b[32m");
  }
}

bool game_string_option_has_on_turn_color(
    const GameStringOptions *game_string_options) {
  if (game_string_options == NULL) {
    return false;
  }
  return game_string_options->on_turn_color != GAME_STRING_ON_TURN_COLOR_NONE;
}

void string_builder_add_color_reset(StringBuilder *game_string) {
  string_builder_add_string(game_string, "\x1b[0m");
}

void string_builder_add_color_bold(StringBuilder *game_string) {
  string_builder_add_string(game_string, "\x1b[1m");
}

bool use_bold_for_score(const GameStringOptions *game_string_options) {
  if (game_string_options == NULL) {
    return false;
  }
  return game_string_options->on_turn_score_style ==
         GAME_STRING_ON_TURN_SCORE_BOLD;
}

void string_builder_add_player_row(const LetterDistribution *ld,
                                   const Player *player,
                                   const GameStringOptions *game_string_options,
                                   StringBuilder *game_string,
                                   bool player_on_turn) {
  const char *player_on_turn_marker =
      use_ascii_on_turn_marker(game_string_options) ? "-> " : "  ➤";
  const char *player_off_turn_marker = "   ";
  const char *player_marker = player_on_turn_marker;
  if (!player_on_turn) {
    player_marker = player_off_turn_marker;
  }

  char *display_player_name;
  const char *player_name = player_get_name(player);
  if (player_name) {
    display_player_name = string_duplicate(player_name);
  } else {
    display_player_name =
        get_formatted_string("Player %d", player_get_index(player) + 1);
  }

  if (player_on_turn && should_print_escape_codes(game_string_options)) {
    string_builder_add_player_on_turn_color(game_string, game_string_options);
  }
  Rack *player_rack = player_get_rack(player);
  string_builder_add_formatted_string(
      game_string, "%s%s%*s", player_marker, display_player_name,
      25 - string_length(display_player_name), "");
  if (player_on_turn && should_print_escape_codes(game_string_options) &&
      game_string_option_has_on_turn_color(game_string_options)) {
    string_builder_add_color_reset(game_string);
  }

  string_builder_add_rack(game_string, player_rack, ld, false);
  string_builder_add_formatted_string(game_string, "%*s%d",
                                      10 - rack_get_total_letters(player_rack),
                                      "", equity_to_int(player_get_score(player)));
  free(display_player_name);
}

void string_builder_add_board_square_color(StringBuilder *game_string,
                                           const Board *board, int row,
                                           int col) {
  const uint8_t current_letter = board_get_letter(board, row, col);
  if (current_letter == ALPHABET_EMPTY_SQUARE_MARKER) {
    string_builder_add_string(game_string,
                              bonus_square_to_color_code(
                                  board_get_bonus_square(board, row, col)));
  } else {
    string_builder_add_color_reset(game_string);
    string_builder_add_color_bold(game_string);
  }
}

bool use_board_color(const GameStringOptions *game_string_options) {
  if (game_string_options == NULL) {
    return false;
  }
  return game_string_options->board_color != GAME_STRING_BOARD_COLOR_NONE;
}

bool should_print_alt_tiles(const GameStringOptions *game_string_options) {
  if (game_string_options == NULL) {
    return false;
  }
  return (game_string_options->board_tile_glyphs ==
          GAME_STRING_BOARD_TILE_GLYPHS_ALT);
}

void string_builder_add_board_top_border(
    const GameStringOptions *game_string_options, StringBuilder *game_string) {
  string_builder_add_string(game_string, "  ");
  if ((game_string_options == NULL) ||
      (game_string_options->board_border == GAME_STRING_BOARD_BORDER_ASCII)) {
    string_builder_add_string(game_string, " ");
    for (int i = 0; i < BOARD_DIM; i++) {
      string_builder_add_string(game_string, "--");
    }
    string_builder_add_string(game_string, " ");
  } else {
    string_builder_add_string(game_string, "┏");
    for (int i = 0; i < BOARD_DIM; i++) {
      string_builder_add_string(game_string, "━━");
    }
    string_builder_add_string(game_string, "┓");
  }
  string_builder_add_string(game_string, " ");
}

void string_builder_add_board_bottom_border(
    const GameStringOptions *game_string_options, StringBuilder *game_string) {
  string_builder_add_string(game_string, "  ");
  if ((game_string_options == NULL) ||
      (game_string_options->board_border == GAME_STRING_BOARD_BORDER_ASCII)) {
    string_builder_add_string(game_string, " ");
    for (int i = 0; i < BOARD_DIM; i++) {
      string_builder_add_string(game_string, "--");
    }
    string_builder_add_string(game_string, " ");
  } else {
    string_builder_add_string(game_string, "┗");
    for (int i = 0; i < BOARD_DIM; i++) {
      string_builder_add_string(game_string, "━━");
    }
    string_builder_add_string(game_string, "┛");
  }
  string_builder_add_string(game_string, " ");
}

void string_builder_add_board_side_border(
    const GameStringOptions *game_string_options, StringBuilder *game_string) {
  if ((game_string_options == NULL) ||
      (game_string_options->board_border == GAME_STRING_BOARD_BORDER_ASCII)) {
    string_builder_add_string(game_string, "|");
  } else {
    string_builder_add_string(game_string, "┃");
  }
}

static const char *full_width_column_label_strings[] = {
    "Ａ", "Ｂ", "Ｃ", "Ｄ", "Ｅ", "Ｆ", "Ｇ", "Ｈ", "Ｉ",
    "Ｊ", "Ｋ", "Ｌ", "Ｍ", "Ｎ", "Ｏ", "Ｐ", "Ｑ", "Ｒ",
    "Ｓ", "Ｔ", "Ｕ", "Ｖ", "Ｗ", "Ｘ", "Ｙ", "Ｚ"};

void string_builder_add_board_row(const LetterDistribution *ld,
                                  const Board *board,
                                  const GameStringOptions *game_string_options,
                                  StringBuilder *game_string, int row) {
  string_builder_add_formatted_string(game_string, "%2d", row + 1);
  string_builder_add_board_side_border(game_string_options, game_string);
  for (int i = 0; i < BOARD_DIM; i++) {
    if (should_print_escape_codes(game_string_options) &&
        use_board_color(game_string_options)) {
      string_builder_add_board_square_color(game_string, board, row, i);
    }
    const uint8_t current_letter = board_get_letter(board, row, i);
    if (current_letter == ALPHABET_EMPTY_SQUARE_MARKER) {
      if (should_print_alt_tiles(game_string_options)) {
        string_builder_add_string(game_string,
                                  bonus_square_to_alt_string(
                                      board_get_bonus_square(board, row, i)));
      } else {
        string_builder_add_char(
            game_string,
            bonus_square_to_char(board_get_bonus_square(board, row, i)));
        string_builder_add_string(game_string, " ");
      }
    } else {
      if (should_print_alt_tiles(game_string_options)) {
        string_builder_add_user_visible_alt_letter(game_string, ld,
                                                   current_letter);
      } else {
        string_builder_add_user_visible_letter(game_string, ld, current_letter);
        string_builder_add_string(game_string, " ");
      }
    }
    if (should_print_escape_codes(game_string_options)) {
      string_builder_add_color_reset(game_string);
    }
  }
  string_builder_add_board_side_border(game_string_options, game_string);
}

void string_builder_add_move_with_rank_and_equity(const Game *game,
                                                  const MoveList *move_list,
                                                  StringBuilder *game_string,
                                                  int move_index) {
  Board *board = game_get_board(game);
  Move *move = move_list_get_move(move_list, move_index);
  const LetterDistribution *ld = game_get_ld(game);
  string_builder_add_formatted_string(game_string, " %d ", move_index + 1);
  string_builder_add_move(game_string, board, move, ld);
  string_builder_add_formatted_string(game_string, " %0.2f",
                                      move_get_equity(move));
}

void string_builder_add_board_column_header(
    const GameStringOptions *game_string_options, int col,
    StringBuilder *game_string) {
  if ((game_string_options == NULL) ||
      game_string_options->board_column_label ==
          GAME_STRING_BOARD_COLUMN_LABEL_ASCII) {
    string_builder_add_formatted_string(game_string, "%c ", col + 65);
  } else {
    if (col < BOARD_NUM_COLUMN_LABELS) {
      string_builder_add_string(game_string,
                                full_width_column_label_strings[col]);
    } else {
      string_builder_add_string(game_string, " ");
    }
  }
}

void string_builder_add_game(const Game *game, const MoveList *move_list,
                             const GameStringOptions *game_string_options,
                             StringBuilder *game_string) {
  Board *board = game_get_board(game);
  Bag *bag = game_get_bag(game);
  Player *player0 = game_get_player(game, 0);
  Player *player1 = game_get_player(game, 1);
  const LetterDistribution *ld = game_get_ld(game);
  int number_of_moves = 0;
  if (move_list) {
    number_of_moves = move_list_get_count(move_list);
  }
  int player_on_turn_index = game_get_player_on_turn_index(game);

  string_builder_add_string(game_string, "   ");

  for (int i = 0; i < BOARD_DIM; i++) {
    string_builder_add_board_column_header(game_string_options, i, game_string);
  }

  string_builder_add_string(game_string, "  ");

  string_builder_add_player_row(ld, player0, game_string_options, game_string,
                                player_on_turn_index == 0);
  string_builder_add_string(game_string, "\n");

  string_builder_add_board_top_border(game_string_options, game_string);
  string_builder_add_player_row(ld, player1, game_string_options, game_string,
                                player_on_turn_index == 1);
  string_builder_add_string(game_string, "\n");

  for (int i = 0; i < BOARD_DIM; i++) {
    string_builder_add_board_row(ld, board, game_string_options, game_string,
                                 i);
    if (i == 0) {
      string_builder_add_string(
          game_string, " --Tracking-----------------------------------");
    } else if (i == 1) {
      string_builder_add_string(game_string, " ");
      string_builder_add_bag(game_string, bag, ld);

      string_builder_add_formatted_string(game_string, "  %d",
                                          bag_get_letters(bag));

    } else if (i - 2 < number_of_moves) {
      string_builder_add_move_with_rank_and_equity(game, move_list, game_string,
                                                   i - 2);
    }
    string_builder_add_string(game_string, "\n");
  }

  string_builder_add_board_bottom_border(game_string_options, game_string);
  string_builder_add_string(game_string, "\n");
}

char *ucgi_static_moves(const Game *game, const MoveList *move_list) {
  if (move_list_get_count(move_list) == 0) {
    return string_duplicate("no moves to print\n");
  }
  StringBuilder *moves_string_builder = string_builder_create();
  const LetterDistribution *ld = game_get_ld(game);
  Board *board = game_get_board(game);

  MoveList *sorted_move_list = move_list_duplicate(move_list);
  move_list_sort_moves(sorted_move_list);

  for (int i = 0; i < move_list_get_count(sorted_move_list); i++) {
    Move *move = move_list_get_move(sorted_move_list, i);
    string_builder_add_string(moves_string_builder, "info currmove ");
    string_builder_add_ucgi_move(moves_string_builder, move, board, ld);

    string_builder_add_formatted_string(
        moves_string_builder, " sc %d eq %.3f it 0\n", move_get_score(move),
        move_get_equity(move));
  }
  string_builder_add_string(moves_string_builder, "bestmove ");
  string_builder_add_ucgi_move(moves_string_builder, move_list_get_move(sorted_move_list, 0), board, ld);
  string_builder_add_string(moves_string_builder, "\n");
  char *ucgi_static_moves_string =
      string_builder_dump(moves_string_builder, NULL);
  string_builder_destroy(moves_string_builder);
  move_list_destroy(sorted_move_list);
  return ucgi_static_moves_string;
}

void print_ucgi_static_moves(Game *game, MoveList *move_list,
                             ThreadControl *thread_control) {
  char *starting_moves_string_pointer = ucgi_static_moves(game, move_list);
  thread_control_print(thread_control, starting_moves_string_pointer);
  free(starting_moves_string_pointer);
}

GameStringOptions *game_string_options_create_default(void) {
  GameStringOptions *gso = malloc_or_die(sizeof(GameStringOptions));
  gso->board_color = GAME_STRING_BOARD_COLOR_NONE;
  gso->board_tile_glyphs = GAME_STRING_BOARD_TILE_GLYPHS_PRIMARY;
  gso->board_border = GAME_STRING_BOARD_BORDER_ASCII;
  gso->board_column_label = GAME_STRING_BOARD_COLUMN_LABEL_ASCII;
  gso->on_turn_marker = GAME_STRING_ON_TURN_MARKER_ASCII;
  gso->on_turn_color = GAME_STRING_ON_TURN_COLOR_NONE;
  gso->on_turn_score_style = GAME_STRING_ON_TURN_SCORE_NORMAL;
  return gso;
}

GameStringOptions *game_string_options_create(
    game_string_board_color_t board_color,
    game_string_board_tile_glyphs_t board_tile_glyphs,
    game_string_board_border_t board_border,
    game_string_board_column_label_t column_label,
    game_string_on_turn_marker_t on_turn_marker,
    game_string_on_turn_color_t on_turn_color,
    game_string_on_turn_score_style_t on_turn_score_style) {
  GameStringOptions *gso = game_string_options_create_default();
  gso->board_color = board_color;
  gso->board_tile_glyphs = board_tile_glyphs;
  gso->board_border = board_border;
  gso->board_column_label = column_label;
  gso->on_turn_marker = on_turn_marker;
  gso->on_turn_color = on_turn_color;
  gso->on_turn_score_style = on_turn_score_style;
  return gso;
}

void game_string_options_destroy(GameStringOptions *gso) { free(gso); }

void string_builder_add_game_variant(StringBuilder *sb, game_variant_t variant) {
  const char *variant_name;
  switch (variant) {
    case GAME_VARIANT_CLASSIC:
      variant_name = GAME_VARIANT_CLASSIC_NAME;
      break;
    case GAME_VARIANT_WORDSMOG:
      variant_name = GAME_VARIANT_WORDSMOG_NAME;
      break;
    default:
      variant_name = GAME_VARIANT_UNKNOWN_NAME;
      break;
  }
  string_builder_add_string(sb, variant_name);
}