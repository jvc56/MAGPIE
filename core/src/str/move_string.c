#include "string_util.h"

#include "../ent/board.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"

void string_builder_add_move_description(const Move *move,
                                         const LetterDistribution *ld,
                                         StringBuilder *move_string_builder) {
  if get_move_type(move) != GAME_EVENT_PASS) {
    if get_move_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
      if (dir_is_verticalget_dir(move))) {
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

    int number_of_tiles_to_print =get_tiles_length(move);

    for (int i = 0; i < number_of_tiles_to_print; i++) {
      uint8_t letter =get_tile(move, i);
      if (letter == PLAYED_THROUGH_MARKER &&
         get_move_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
        string_builder_add_char(move_string_builder, ASCII_PLAYED_THROUGH);
      } else {
        string_builder_add_user_visible_letter(ld, move_string_builder, letter);
      }
    }
    if get_move_type(move) == GAME_EVENT_EXCHANGE) {
      string_builder_add_string(move_string_builder, ")");
    }
  } else {
    string_builder_add_string(move_string_builder, "(Pass)");
  }
}

void string_builder_add_move(const Board *board, const Move *m,
                             const LetterDistribution *letter_distribution,
                             StringBuilder *string_builder) {
  if (m->move_type == GAME_EVENT_PASS) {
    string_builder_add_string(string_builder, "pass 0");
    return;
  }

  if (m->move_type == GAME_EVENT_EXCHANGE) {
    string_builder_add_string(string_builder, "(exch ");
    for (int i = 0; i < m->tiles_played; i++) {
      string_builder_add_user_visible_letter(letter_distribution,
                                             string_builder, m->tiles[i]);
    }
    string_builder_add_string(string_builder, ")");
    return;
  }

  if (dir_is_vertical(m->dir)) {
    string_builder_add_char(string_builder, m->col_start + 'A');
    string_builder_add_int(string_builder, m->row_start + 1);
  } else {
    string_builder_add_int(string_builder, m->row_start + 1);
    string_builder_add_char(string_builder, m->col_start + 'A');
  }

  string_builder_add_spaces(string_builder, 1);
  int current_row = m->row_start;
  int current_col = m->col_start;
  for (int i = 0; i < m->tiles_length; i++) {
    uint8_t tile = m->tiles[i];
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
        (i == m->tiles_length - 1 ||
         m->tiles[i + 1] != PLAYED_THROUGH_MARKER)) {
      string_builder_add_string(string_builder, ")");
    }

    if (board && tile != PLAYED_THROUGH_MARKER && (i + 1 < m->tiles_length) &&
        m->tiles[i + 1] == PLAYED_THROUGH_MARKER) {
      string_builder_add_string(string_builder, "(");
    }

    if (dir_is_vertical(m->dir)) {
      current_row++;
    } else {
      current_col++;
    }
  }
  string_builder_add_spaces(string_builder, 1);
  if (board) {
    string_builder_add_int(string_builder, m->score);
  }
}