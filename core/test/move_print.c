#include "../src/move.h"

#include "alphabet_print.h"
#include "move_print.h"
#include "test_util.h"

void write_user_visible_move_to_end_of_buffer(char * buf, Board * b, Move * m, Alphabet * alphabet) {
    if (m->move_type == MOVE_TYPE_PASS) {
        write_string_to_end_of_buffer(buf, "pass 0");
        return;
    }

    if (m->move_type == MOVE_TYPE_EXCHANGE) {
        write_string_to_end_of_buffer(buf, "exch ");
        for (int i = 0; i < m->tiles_played; i++) {
            write_user_visible_letter_to_end_of_buffer(buf, alphabet, m->tiles[i]);
        }
        write_string_to_end_of_buffer(buf, " 0");
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
            print_tile = get_letter(b, current_row, current_col);
            if (i == 0) {
                write_string_to_end_of_buffer(buf, "(");
            }
        }

        write_user_visible_letter_to_end_of_buffer(buf, alphabet, print_tile);

        if ((tile == PLAYED_THROUGH_MARKER) && (i == m->tiles_length - 1 || m->tiles[i+1] != PLAYED_THROUGH_MARKER)) {
            write_string_to_end_of_buffer(buf, ")");
        }

        if (tile != PLAYED_THROUGH_MARKER && (i + 1 < m->tiles_length) && m->tiles[i+1] == PLAYED_THROUGH_MARKER) {
            write_string_to_end_of_buffer(buf, "(");
        }
    
        if (m->vertical) {
            current_row++;
        } else {
            current_col++;
        }
    }
    write_spaces_to_end_of_buffer(buf, 1);
    write_int_to_end_of_buffer(buf, m->score);
}

void write_move_list_to_end_of_buffer(char * buf, MoveList * ml, Board * b, Alphabet * alph) {
    for (int i = 0; i < ml->count; i++) {
        write_user_visible_move_to_end_of_buffer(buf, b, ml->moves[i], alph);
        write_string_to_end_of_buffer(buf, "\n");
    }
}