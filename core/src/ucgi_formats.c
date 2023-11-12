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
      if (move->vertical) {
        string_builder_add_formatted_string(move_string_builder, "%c%d.",
                                            move->col_start + 'a',
                                            move->row_start + 1);
      } else {
        string_builder_add_formatted_string(move_string_builder, "%d%c.",
                                            move->row_start + 1,
                                            move->col_start + 'a');
      }
    } else {
      string_builder_add_string(move_string_builder, "ex.", 0);
    }

    int number_of_tiles_to_print = move->tiles_length;

    // FIXME: make sure tiles_length == tiles_played for exchanges
    // this is not true currently.
    if (move->move_type == GAME_EVENT_EXCHANGE) {
      number_of_tiles_to_print = move->tiles_played;
    }

    int ri = 0;
    int ci = 0;
    if (move->vertical) {
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
      string_builder_add_user_visible_letter(ld, letter, 0,
                                             move_string_builder);
    }
  } else {
    string_builder_add_string(move_string_builder, "pass", 0);
  }
}