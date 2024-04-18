#include "game_string.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#include "../def/board_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../ent/bag.h"
#include "../ent/board.h"
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

void string_builder_add_player_row(const LetterDistribution *ld,
                                   const Player *player,
                                   StringBuilder *game_string,
                                   bool player_on_turn) {
  const char *player_on_turn_marker = "-> ";
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

  Rack *player_rack = player_get_rack(player);
  string_builder_add_formatted_string(
      game_string, "%s%s%*s", player_marker, display_player_name,
      25 - string_length(display_player_name), "");
  string_builder_add_rack(player_rack, ld, game_string);
  string_builder_add_formatted_string(game_string, "%*s%d",
                                      10 - rack_get_total_letters(player_rack),
                                      "", player_get_score(player));
  free(display_player_name);
}

void string_builder_add_color_reset(StringBuilder *game_string) {
  string_builder_add_string(game_string, "\x1b[0m");
}

void string_builder_add_color_bold(StringBuilder *game_string) {
  string_builder_add_string(game_string, "\x1b[1m");
}

void string_builder_add_board_square_color(StringBuilder *game_string,
                                           const Board *board, int row,
                                           int col) {
  const uint8_t current_letter = board_get_letter(board, row, col);
  if (current_letter == ALPHABET_EMPTY_SQUARE_MARKER) {
    string_builder_add_string(game_string,
                              bonus_square_value_to_color_code(
                                  board_get_bonus_square(board, row, col)));
  } else {
    string_builder_add_color_reset(game_string);
    string_builder_add_color_bold(game_string);
  }
}

bool should_print_escape_codes(const GameStringOptions *game_string_options) {
  if (game_string_options == NULL) {
    return false;
  }
  if (game_string_options->board_color == GAME_STRING_BOARD_COLOR_NONE) {
    return false;
  }
  return isatty(fileno(stdout));
}

bool should_print_alt_tiles(const GameStringOptions *game_string_options) {
  if (game_string_options == NULL) {
    return false;
  }
  return (game_string_options->board_tile_glyphs ==
          GAME_STRING_BOARD_TILE_GLYPHS_ALT);
}

void string_builder_add_board_row(const LetterDistribution *ld,
                                  const Board *board,
                                  const GameStringOptions *game_string_options,
                                  StringBuilder *game_string, int row) {
  string_builder_add_formatted_string(game_string, "%2d|", row + 1);
  for (int i = 0; i < BOARD_DIM; i++) {
    if (should_print_escape_codes(game_string_options)) {
      string_builder_add_board_square_color(game_string, board, row, i);
    }
    const uint8_t current_letter = board_get_letter(board, row, i);
    if (current_letter == ALPHABET_EMPTY_SQUARE_MARKER) {
      if (should_print_alt_tiles(game_string_options)) {
        string_builder_add_string(game_string,
                                  bonus_square_value_to_alt_string(
                                      board_get_bonus_square(board, row, i)));
      } else {
        string_builder_add_char(
            game_string,
            bonus_square_value_to_char(board_get_bonus_square(board, row, i)));
        string_builder_add_string(game_string, " ");
      }
    } else {
      if (should_print_alt_tiles(game_string_options)) {
        string_builder_add_user_visible_alt_letter(ld, game_string,
                                                   current_letter);
      } else {
        string_builder_add_user_visible_letter(ld, game_string, current_letter);
        string_builder_add_string(game_string, " ");
      }
    }
    if (should_print_escape_codes(game_string_options)) {
      string_builder_add_color_reset(game_string);
    }
  }
  string_builder_add_string(game_string, "|");
}

void string_builder_add_move_with_rank_and_equity(const Game *game,
                                                  const MoveList *move_list,
                                                  StringBuilder *game_string,
                                                  int move_index) {
  Board *board = game_get_board(game);
  Move *move = move_list_get_move(move_list, move_index);
  const LetterDistribution *ld = game_get_ld(game);
  string_builder_add_formatted_string(game_string, " %d ", move_index + 1);
  string_builder_add_move(board, move, ld, game_string);
  string_builder_add_formatted_string(game_string, " %0.2f",
                                      move_get_equity(move));
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

  string_builder_add_string(game_string, "  ");

  for (int i = 0; i < BOARD_DIM; i++) {
    string_builder_add_formatted_string(game_string, " %c", i + 65);
  }

  string_builder_add_string(game_string, "   ");

  string_builder_add_player_row(ld, player0, game_string,
                                player_on_turn_index == 0);
  string_builder_add_string(game_string, "\n   ");

  for (int i = 0; i < BOARD_DIM; i++) {
    string_builder_add_string(game_string, "--");
  }

  string_builder_add_string(game_string, "  ");
  string_builder_add_player_row(ld, player1, game_string,
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
      string_builder_add_bag(bag, ld, game_string);

      string_builder_add_formatted_string(game_string, "  %d",
                                          bag_get_tiles(bag));

    } else if (i - 2 < number_of_moves) {
      string_builder_add_move_with_rank_and_equity(game, move_list, game_string,
                                                   i - 2);
    }
    string_builder_add_string(game_string, "\n");
  }

  string_builder_add_string(game_string, "   ");

  for (int i = 0; i < BOARD_DIM; i++) {
    string_builder_add_string(game_string, "--");
  }

  string_builder_add_string(game_string, "\n");
}

char *ucgi_static_moves(const Game *game, const MoveList *move_list) {
  if (move_list_get_count(move_list) == 0) {
    return string_duplicate("no moves to print\n");
  }
  StringBuilder *moves_string_builder = create_string_builder();
  const LetterDistribution *ld = game_get_ld(game);
  Board *board = game_get_board(game);

  MoveList *sorted_move_list = move_list_duplicate(move_list);
  move_list_sort_moves(sorted_move_list);

  for (int i = 0; i < move_list_get_count(sorted_move_list); i++) {
    Move *move = move_list_get_move(sorted_move_list, i);
    string_builder_add_string(moves_string_builder, "info currmove ");
    string_builder_add_ucgi_move(move, board, ld, moves_string_builder);

    string_builder_add_formatted_string(
        moves_string_builder, " sc %d eq %.3f it 0\n", move_get_score(move),
        move_get_equity(move));
  }
  string_builder_add_string(moves_string_builder, "bestmove ");
  string_builder_add_ucgi_move(move_list_get_move(sorted_move_list, 0), board,
                               ld, moves_string_builder);
  string_builder_add_string(moves_string_builder, "\n");
  char *ucgi_static_moves_string =
      string_builder_dump(moves_string_builder, NULL);
  destroy_string_builder(moves_string_builder);
  move_list_destroy(sorted_move_list);
  return ucgi_static_moves_string;
}

void print_ucgi_static_moves(Game *game, MoveList *move_list,
                             ThreadControl *thread_control) {
  char *starting_moves_string_pointer = ucgi_static_moves(game, move_list);
  thread_control_print(thread_control, starting_moves_string_pointer);
  free(starting_moves_string_pointer);
}

GameStringOptions *game_string_options_create_default() {
  GameStringOptions *gso = malloc_or_die(sizeof(GameStringOptions));
  gso->board_color = GAME_STRING_BOARD_COLOR_NONE;
  return gso;
}

GameStringOptions *game_string_options_create(
    game_string_board_color_t board_color,
    game_string_board_tile_glyphs_t board_tile_glyphs) {
  GameStringOptions *gso = game_string_options_create_default();
  gso->board_color = board_color;
  gso->board_tile_glyphs = board_tile_glyphs;
  return gso;
}

void game_string_options_destroy(GameStringOptions *gso) { free(gso); }