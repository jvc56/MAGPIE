#include "../def/board_defs.h"
#include "../def/game_defs.h"
#include "../def/game_history_defs.h"
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
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "bag_string.h"
#include "equity_string.h"
#include "letter_distribution_string.h"
#include "move_string.h"
#include "rack_string.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

void string_builder_add_game_variant(StringBuilder *sb,
                                     game_variant_t game_variant_type) {
  switch (game_variant_type) {
  case GAME_VARIANT_CLASSIC:
    string_builder_add_string(sb, GAME_VARIANT_CLASSIC_NAME);
    break;
  case GAME_VARIANT_WORDSMOG:
    string_builder_add_string(sb, GAME_VARIANT_WORDSMOG_NAME);
    break;
  default:
    string_builder_add_string(sb, GAME_VARIANT_UNKNOWN_NAME);
    break;
  }
}

void string_builder_add_player_row(StringBuilder *game_string,
                                   const LetterDistribution *ld,
                                   const Player *player,

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

  const Rack *player_rack = player_get_rack(player);
  string_builder_add_formatted_string(
      game_string, "%s%s%*s", player_marker, display_player_name,
      25 - string_length(display_player_name), "");
  string_builder_add_rack(game_string, player_rack, ld, false);
  string_builder_add_formatted_string(
      game_string, "%*s%d", 10 - rack_get_total_letters(player_rack), "",
      equity_to_int(player_get_score(player)));
  free(display_player_name);
}

void string_builder_add_board_row(StringBuilder *game_string,
                                  const LetterDistribution *ld,
                                  const Board *board, int row) {
  string_builder_add_formatted_string(game_string, "%2d|", row + 1);
  for (int i = 0; i < BOARD_DIM; i++) {
    MachineLetter current_letter = board_get_letter(board, row, i);
    if (current_letter == ALPHABET_EMPTY_SQUARE_MARKER) {
      string_builder_add_char(
          game_string,
          bonus_square_to_char(board_get_bonus_square(board, row, i)));
    } else {
      string_builder_add_user_visible_letter(game_string, ld, current_letter);
    }
    string_builder_add_string(game_string, " ");
  }
  string_builder_add_string(game_string, "|");
}

void string_builder_add_move_with_rank_and_equity(StringBuilder *game_string,
                                                  const Game *game,
                                                  const MoveList *move_list,
                                                  int move_index) {
  const Board *board = game_get_board(game);
  const Move *move = move_list_get_move(move_list, move_index);
  const LetterDistribution *ld = game_get_ld(game);
  string_builder_add_formatted_string(game_string, " %d ", move_index + 1);
  string_builder_add_move(game_string, board, move, ld);
  string_builder_add_string(game_string, " ");
  const Equity eq = move_get_equity(move);
  string_builder_add_equity(game_string, eq, "%0.3f");
}

void string_builder_add_game(StringBuilder *game_string, const Game *game,
                             const MoveList *move_list) {
  const Board *board = game_get_board(game);
  const Bag *bag = game_get_bag(game);
  const Player *player0 = game_get_player(game, 0);
  const Player *player1 = game_get_player(game, 1);
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

  string_builder_add_player_row(game_string, ld, player0,
                                player_on_turn_index == 0);
  string_builder_add_string(game_string, "\n   ");

  for (int i = 0; i < BOARD_DIM; i++) {
    string_builder_add_string(game_string, "--");
  }

  string_builder_add_string(game_string, "  ");
  string_builder_add_player_row(game_string, ld, player1,
                                player_on_turn_index == 1);
  string_builder_add_string(game_string, "\n");

  for (int i = 0; i < BOARD_DIM; i++) {
    string_builder_add_board_row(game_string, ld, board, i);
    if (i == 0) {
      string_builder_add_string(
          game_string, " --Tracking-----------------------------------");
    } else if (i == 1) {
      string_builder_add_string(game_string, " ");
      string_builder_add_bag(game_string, bag, ld);

      string_builder_add_formatted_string(game_string, "  %d",
                                          bag_get_tiles(bag));

    } else if (i - 2 < number_of_moves) {
      string_builder_add_move_with_rank_and_equity(game_string, game, move_list,
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
  StringBuilder *moves_string_builder = string_builder_create();
  const LetterDistribution *ld = game_get_ld(game);
  const Board *board = game_get_board(game);

  MoveList *sorted_move_list = move_list_duplicate(move_list);
  move_list_sort_moves(sorted_move_list);

  for (int i = 0; i < move_list_get_count(sorted_move_list); i++) {
    const Move *move = move_list_get_move(sorted_move_list, i);
    string_builder_add_string(moves_string_builder, "info currmove ");
    string_builder_add_ucgi_move(moves_string_builder, move, board, ld);
    double move_equity = -100000.0;
    if (move_get_type(move) != GAME_EVENT_PASS) {
      move_equity = equity_to_double(move_get_equity(move));
    }
    string_builder_add_formatted_string(
        moves_string_builder, " sc %d eq %.3f it 0\n",
        equity_to_int(move_get_score(move)), move_equity);
  }
  string_builder_add_string(moves_string_builder, "bestmove ");
  string_builder_add_ucgi_move(
      moves_string_builder, move_list_get_move(sorted_move_list, 0), board, ld);
  string_builder_add_string(moves_string_builder, "\n");
  char *ucgi_static_moves_string =
      string_builder_dump(moves_string_builder, NULL);
  string_builder_destroy(moves_string_builder);
  move_list_destroy(sorted_move_list);
  return ucgi_static_moves_string;
}

void print_ucgi_static_moves(const Game *game, const MoveList *move_list,
                             ThreadControl *thread_control) {
  char *starting_moves_string_pointer = ucgi_static_moves(game, move_list);
  thread_control_print(thread_control, starting_moves_string_pointer);
  free(starting_moves_string_pointer);
}