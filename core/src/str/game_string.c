#include "../ent/bag.h"
#include "../ent/board.h"
#include "../ent/game.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/movegen.h"
#include "../ent/player.h"

#include "string_util.h"

void string_builder_add_player_row(
    const LetterDistribution *letter_distribution, const Player *player,
    StringBuilder *game_string, bool player_on_turn) {

  const char *player_on_turn_marker = "-> ";
  const char *player_off_turn_marker = "   ";
  const char *player_marker = player_on_turn_marker;
  if (!player_on_turn) {
    player_marker = player_off_turn_marker;
  }

  char *display_player_name;
  char *player_name = player_get_name(player);
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
  string_builder_add_rack(player_rack, letter_distribution, game_string);
  string_builder_add_formatted_string(game_string, "%*s%d",
                                      10 - get_number_of_letters(player_rack),
                                      "", player_get_score(player));
  free(display_player_name);
}

void string_builder_add_board_row(const LetterDistribution *letter_distribution,
                                  const Board *board,
                                  StringBuilder *game_string, int row) {
  string_builder_add_formatted_string(game_string, "%2d|", row + 1);
  for (int i = 0; i < BOARD_DIM; i++) {
    uint8_t current_letter = get_letter(board, row, i);
    if (current_letter == ALPHABET_EMPTY_SQUARE_MARKER) {
      string_builder_add_char(game_string,
                              CROSSWORD_GAME_BOARD[(row * BOARD_DIM) + i]);
    } else {
      string_builder_add_user_visible_letter(letter_distribution, game_string,
                                             current_letter);
    }
    string_builder_add_string(game_string, " ");
  }
  string_builder_add_string(game_string, "|");
}

void string_builder_add_move_with_rank_and_equity(const Game *game,
                                                  StringBuilder *game_string,
                                                  int move_index) {
  Generator *gen = game_get_gen(game);
  Player *player0 = game_get_player(game, 0);
  Player *player1 = game_get_player(game, 1);
  MoveList *move_list = gen_get_move_list(gen);
  Move *move = move_list_get_move(move_list, move_index);
  Board *board = gen_get_board(gen);
  LetterDistribution *ld = gen_get_ld(gen);
  string_builder_add_formatted_string(game_string, " %d ", move_index + 1);
  string_builder_add_move(board, move, ld, game_string);
  string_builder_add_formatted_string(game_string, " %0.2f", get_equity(move));
}

void string_builder_add_game(const Game *game, StringBuilder *game_string) {
  // TODO: update for super crossword game
  Generator *gen = game_get_gen(game);
  Player *player0 = game_get_player(game, 0);
  Player *player1 = game_get_player(game, 1);
  MoveList *move_list = gen_get_move_list(gen);
  Board *board = gen_get_board(gen);
  Bag *bag = gen_get_bag(gen);
  LetterDistribution *ld = gen_get_ld(gen);
  int number_of_moves = move_list_get_count(move_list);
  int player_on_turn_index = game_get_player_on_turn_index(game);

  string_builder_add_string(game_string, "   A B C D E F G H I J K L M N O   ");
  string_builder_add_player_row(ld, player0, game_string,
                                player_on_turn_index == 0);
  string_builder_add_string(game_string,
                            "\n   ------------------------------  ");
  string_builder_add_player_row(ld, player1, game_string,
                                player_on_turn_index == 1);
  string_builder_add_string(game_string, "\n");

  for (int i = 0; i < BOARD_DIM; i++) {
    string_builder_add_board_row(ld, board, game_string, i);
    if (i == 0) {
      string_builder_add_string(
          game_string, " --Tracking-----------------------------------");
    } else if (i == 1) {
      string_builder_add_string(game_string, " ");
      string_builder_add_bag(bag, ld, game_string);

      string_builder_add_formatted_string(game_string, "  %d",
                                          get_tiles_remaining(bag));

    } else if (i - 2 < number_of_moves) {
      string_builder_add_move_with_rank_and_equity(game, game_string, i - 2);
    }
    string_builder_add_string(game_string, "\n");
  }

  string_builder_add_string(game_string, "   ------------------------------\n");
}