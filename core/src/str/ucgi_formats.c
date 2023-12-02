#include <stdio.h>

#include "board.h"
#include "letter_distribution.h"
#include "move.h"
#include "string_util.h"

void string_builder_add_ucgi_move(const Move *move, const Board *board,
                                  const LetterDistribution *ld,
                                  StringBuilder *move_string_builder) {

  if get_move_type(move) != GAME_EVENT_PASS) {
    if get_move_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
      if (dir_is_verticalget_dir(move))) {
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

    int number_of_tiles_to_print =get_tiles_length(move);

    int ri = 0;
    int ci = 0;
    if (dir_is_verticalget_dir(move))) {
      ri = 1;
    } else {
      ci = 1;
    }

    for (int i = 0; i < number_of_tiles_to_print; i++) {
      uint8_t letter =get_tile(move, i);
      if (letter == PLAYED_THROUGH_MARKER &&
         get_move_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
        int r =get_row_start(move) + (ri * i);
        int c =get_col_start(move) + (ci * i);
        letter = get_letter(board, r, c);
      }
      string_builder_add_user_visible_letter(ld, move_string_builder, letter);
    }
  } else {
    string_builder_add_string(move_string_builder, "pass");
  }
}