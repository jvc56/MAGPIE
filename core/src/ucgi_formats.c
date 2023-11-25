#include <stdio.h>

#include "board.h"
#include "letter_distribution.h"
#include "move.h"
#include "string_util.h"

void string_builder_add_ucgi_move(const Move *move, const Board *board,
                                  const LetterDistribution *ld,
                                  StringBuilder *move_string_builder) {

  if (move->move_type != GAME_EVENT_PASS) {
    if (move->move_type == GAME_EVENT_TILE_PLACEMENT_MOVE) {
      if (dir_is_vertical(move->dir)) {
        string_builder_add_formatted_string(move_string_builder, "%c%d.",
                                            move->col_start + 'a',
                                            move->row_start + 1);
      } else {
        string_builder_add_formatted_string(move_string_builder, "%d%c.",
                                            move->row_start + 1,
                                            move->col_start + 'a');
      }
    } else {
      string_builder_add_string(move_string_builder, "ex.");
    }

    int number_of_tiles_to_print = move->tiles_length;

    int ri = 0;
    int ci = 0;
    if (dir_is_vertical(move->dir)) {
      ri = 1;
    } else {
      ci = 1;
    }

    for (int i = 0; i < number_of_tiles_to_print; i++) {
      uint8_t letter = move->tiles[i];
      if (letter == PLAYED_THROUGH_MARKER &&
          move->move_type == GAME_EVENT_TILE_PLACEMENT_MOVE) {
        int r = move->row_start + (ri * i);
        int c = move->col_start + (ci * i);
        letter = get_letter(board, r, c);
      }
      string_builder_add_user_visible_letter(ld, move_string_builder, letter);
    }
  } else {
    string_builder_add_string(move_string_builder, "pass");
  }
}