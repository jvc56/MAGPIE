#include "../src/letter_distribution.h"
#include "../src/move.h"
#include "../src/util.h"

#include "move_print.h"
#include "test_util.h"

void write_user_visible_move_to_end_of_buffer(
    char *buf, Board *b, Move *m, LetterDistribution *letter_distribution) {
  if (m->move_type == GAME_EVENT_PASS) {
    write_string_to_end_of_buffer(buf, "pass 0");
    return;
  }

  if (m->move_type == GAME_EVENT_EXCHANGE) {
    write_string_to_end_of_buffer(buf, "(exch ");
    for (int i = 0; i < m->tiles_played; i++) {
      write_user_visible_letter_to_end_of_buffer(buf, letter_distribution,
                                                 m->tiles[i]);
    }
    write_string_to_end_of_buffer(buf, ")");
    return;
  }

  if (m->vertical) {
    write_char_to_end_of_buffer(buf, m->col_start + 'A');
    write_int_to_end_of_buffer(buf, m->row_start + 1);
  } else {
    write_int_to_end_of_buffer(buf, m->row_start + 1);
    write_char_to_end_of_buffer(buf, m->col_start + 'A');
  }

  write_spaces_to_end_of_buffer(buf, 1);
  int current_row = m->row_start;
  int current_col = m->col_start;
  for (int i = 0; i < m->tiles_length; i++) {
    uint8_t tile = m->tiles[i];
    uint8_t print_tile = tile;
    if (tile == PLAYED_THROUGH_MARKER) {
      if (b) {
        print_tile = get_letter(b, current_row, current_col);
      }
      if (i == 0 && b) {
        write_string_to_end_of_buffer(buf, "(");
      }
    }

    if (tile == PLAYED_THROUGH_MARKER && !b) {
      write_string_to_end_of_buffer(buf, ".");
    } else {
      write_user_visible_letter_to_end_of_buffer(buf, letter_distribution,
                                                 print_tile);
    }

    if (b && (tile == PLAYED_THROUGH_MARKER) &&
        (i == m->tiles_length - 1 ||
         m->tiles[i + 1] != PLAYED_THROUGH_MARKER)) {
      write_string_to_end_of_buffer(buf, ")");
    }

    if (b && tile != PLAYED_THROUGH_MARKER && (i + 1 < m->tiles_length) &&
        m->tiles[i + 1] == PLAYED_THROUGH_MARKER) {
      write_string_to_end_of_buffer(buf, "(");
    }

    if (m->vertical) {
      current_row++;
    } else {
      current_col++;
    }
  }
  write_spaces_to_end_of_buffer(buf, 1);
  if (b) {
    write_int_to_end_of_buffer(buf, m->score);
  }
}

void write_move_list_to_end_of_buffer(char *buf, MoveList *ml, Board *b,
                                      LetterDistribution *letter_distribution) {
  for (int i = 0; i < ml->count; i++) {
    write_user_visible_move_to_end_of_buffer(buf, b, ml->moves[i],
                                             letter_distribution);
    write_string_to_end_of_buffer(buf, "\n");
  }
}