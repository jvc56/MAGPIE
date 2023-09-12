#include <stdio.h>

#include "board.h"
#include "letter_distribution.h"
#include "move.h"

void store_move_ucgi(Move *move, Board *board, char *placeholder,
                     LetterDistribution *ld) {

  char tiles[25];
  char *tp = tiles;

  int ri = 0;
  int ci = 0;
  if (move->vertical) {
    ri = 1;
  } else {
    ci = 1;
  }

  int number_of_tiles_to_print = move->tiles_length;

  if (move->move_type == MOVE_TYPE_EXCHANGE) {
    number_of_tiles_to_print = move->tiles_played;
  }

  for (int i = 0; i < number_of_tiles_to_print; i++) {
    int letter = move->tiles[i];
    if (letter == 0 && move->move_type == MOVE_TYPE_PLAY) {
      int r = move->row_start + (ri * i);
      int c = move->col_start + (ci * i);
      letter = get_letter(board, r, c);
    }
    char tile[MAX_LETTER_CHAR_LENGTH];
    machine_letter_to_human_readable_letter(ld, letter, tile);
    tp += sprintf(tp, "%s", tile);
  }
  char coords[20];
  tp = coords;

  if (move->move_type == MOVE_TYPE_PLAY) {
    if (move->vertical) {
      tp += sprintf(tp, "%c", move->col_start + 'a');
      tp += sprintf(tp, "%d", move->row_start + 1);
    } else {
      tp += sprintf(tp, "%d", move->row_start + 1);
      tp += sprintf(tp, "%c", move->col_start + 'a');
    }
    sprintf(placeholder, "%s.%s", coords, tiles);
  } else if (move->move_type == MOVE_TYPE_EXCHANGE) {
    sprintf(placeholder, "ex.%s", tiles);
  } else if (move->move_type == MOVE_TYPE_PASS) {
    sprintf(placeholder, "pass");
  }
}