#include "../util/string_util.h"
#include "letter_distribution_string.h"

#include "../ent/board.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"

void string_builder_add_move_description(const Move *move,
                                         const LetterDistribution *ld,
                                         StringBuilder *move_string_builder) {
  if (get_move_type(move) != GAME_EVENT_PASS) {
    if (get_move_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
      if (dir_is_vertical(get_dir(move))) {
        string_builder_add_formatted_string(move_string_builder, "%c%d ",
                                            get_col_start(move) + 'A',
                                            get_row_start(move) + 1);
      } else {
        string_builder_add_formatted_string(move_string_builder, "%d%c ",
                                            get_row_start(move) + 1,
                                            get_col_start(move) + 'A');
      }
    } else {
      string_builder_add_string(move_string_builder, "(Exch ");
    }

    int number_of_tiles_to_print = get_tiles_length(move);

    for (int i = 0; i < number_of_tiles_to_print; i++) {
      uint8_t letter = get_tile(move, i);
      if (letter == PLAYED_THROUGH_MARKER &&
          get_move_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
        string_builder_add_char(move_string_builder, ASCII_PLAYED_THROUGH);
      } else {
        string_builder_add_user_visible_letter(ld, move_string_builder, letter);
      }
    }
    if (get_move_type(move) == GAME_EVENT_EXCHANGE) {
      string_builder_add_string(move_string_builder, ")");
    }
  } else {
    string_builder_add_string(move_string_builder, "(Pass)");
  }
}

void string_builder_add_move(const Board *board, const Move *move,
                             const LetterDistribution *letter_distribution,
                             StringBuilder *string_builder) {
  if (get_move_type(move) == GAME_EVENT_PASS) {
    string_builder_add_string(string_builder, "pass 0");
    return;
  }

  if (get_move_type(move) == GAME_EVENT_EXCHANGE) {
    string_builder_add_string(string_builder, "(exch ");
    for (int i = 0; i < move_get_tiles_played(move); i++) {
      string_builder_add_user_visible_letter(letter_distribution,
                                             string_builder, get_tile(move, i));
    }
    string_builder_add_string(string_builder, ")");
    return;
  }

  if (dir_is_vertical(get_dir(move))) {
    string_builder_add_char(string_builder, get_col_start(move) + 'A');
    string_builder_add_int(string_builder, get_row_start(move) + 1);
  } else {
    string_builder_add_int(string_builder, get_row_start(move) + 1);
    string_builder_add_char(string_builder, get_col_start(move) + 'A');
  }

  string_builder_add_spaces(string_builder, 1);
  int current_row = get_row_start(move);
  int current_col = get_col_start(move);
  for (int i = 0; i < get_tiles_length(move); i++) {
    uint8_t tile = get_tile(move, i);
    uint8_t print_tile = tile;
    if (tile == PLAYED_THROUGH_MARKER) {
      if (board) {
        print_tile = get_letter(board, current_row, current_col);
      }
      if (i == 0 && board) {
        string_builder_add_string(string_builder, "(");
      }
    }

    if (tile == PLAYED_THROUGH_MARKER && !board) {
      string_builder_add_string(string_builder, ".");
    } else {
      string_builder_add_user_visible_letter(letter_distribution,
                                             string_builder, print_tile);
    }

    if (board && (tile == PLAYED_THROUGH_MARKER) &&
        (i == get_tiles_length(move) - 1 ||
         get_tile(move, i + 1) != PLAYED_THROUGH_MARKER)) {
      string_builder_add_string(string_builder, ")");
    }

    if (board && (tile != PLAYED_THROUGH_MARKER) &&
        (i + 1 < get_tiles_length(move)) &&
        get_tile(move, i + 1) == PLAYED_THROUGH_MARKER) {
      string_builder_add_string(string_builder, "(");
    }

    if (dir_is_vertical(get_dir(move))) {
      current_row++;
    } else {
      current_col++;
    }
  }
  string_builder_add_spaces(string_builder, 1);
  if (board) {
    string_builder_add_int(string_builder, get_score(move));
  }
}

void string_builder_add_ucgi_move(const Move *move, const Board *board,
                                  const LetterDistribution *ld,
                                  StringBuilder *move_string_builder) {

  if (get_move_type(move) != GAME_EVENT_PASS) {
    if (get_move_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
      if (dir_is_vertical(get_dir(move))) {
        string_builder_add_formatted_string(move_string_builder, "%c%d.",
                                            get_col_start(move) + 'a',
                                            get_row_start(move) + 1);
      } else {
        string_builder_add_formatted_string(move_string_builder, "%d%c.",
                                            get_row_start(move) + 1,
                                            get_col_start(move) + 'a');
      }
    } else {
      string_builder_add_string(move_string_builder, "ex.");
    }

    int number_of_tiles_to_print = get_tiles_length(move);

    int ri = 0;
    int ci = 0;
    if (dir_is_vertical(get_dir(move))) {
      ri = 1;
    } else {
      ci = 1;
    }

    for (int i = 0; i < number_of_tiles_to_print; i++) {
      uint8_t letter = get_tile(move, i);
      if (letter == PLAYED_THROUGH_MARKER &&
          get_move_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
        int r = get_row_start(move) + (ri * i);
        int c = get_col_start(move) + (ci * i);
        letter = get_letter(board, r, c);
      }
      string_builder_add_user_visible_letter(ld, move_string_builder, letter);
    }
  } else {
    string_builder_add_string(move_string_builder, "pass");
  }
}