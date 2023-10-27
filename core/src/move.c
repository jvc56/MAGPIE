#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "board.h"
#include "constants.h"
#include "log.h"
#include "move.h"
#include "string_util.h"
#include "util.h"

Move *create_move() { return malloc_or_die(sizeof(Move)); }

void destroy_move(Move *move) { free(move); }

void create_moves(MoveList *ml, int capacity) {
  ml->capacity = capacity + 1;
  ml->moves = malloc_or_die(sizeof(Move *) * ml->capacity);
  for (int i = 0; i < ml->capacity; i++) {
    ml->moves[i] = create_move();
  }
}

void destroy_moves(MoveList *ml) {
  for (int i = 0; i < ml->capacity; i++) {
    destroy_move(ml->moves[i]);
  }
  free(ml->moves);
}

void update_move_list(MoveList *ml, int new_capacity) {
  if (ml->capacity != new_capacity) {
    destroy_moves(ml);
    create_moves(ml, new_capacity);
  }
}

MoveList *create_move_list(int capacity) {
  MoveList *ml = malloc_or_die(sizeof(MoveList));
  ml->count = 0;
  // We set increment capacity here
  // because we need to temporarily hold
  // capacity + 1 moves to before popping
  // the least desirable move.
  ml->spare_move = create_move();
  create_moves(ml, capacity);
  ml->moves[0]->equity = INITIAL_TOP_MOVE_EQUITY;
  return ml;
}

void destroy_move_list(MoveList *ml) {
  destroy_moves(ml);
  destroy_move(ml->spare_move);
  free(ml);
}

void reset_move_list(MoveList *ml) {
  ml->count = 0;
  ml->moves[0]->equity = INITIAL_TOP_MOVE_EQUITY;
}

int within_epsilon_for_equity(double a, double board) {
  return fabs(a - board) < 1e-6;
}

// Enforce arbitrary order to keep
// move order deterministic
int compare_moves(Move *move_1, Move *move_2) {
  if (!within_epsilon_for_equity(move_1->equity, move_2->equity)) {
    return move_1->equity > move_2->equity;
  }
  if (move_1->score != move_2->score) {
    return move_1->score > move_2->score;
  }
  if (move_1->move_type != move_2->move_type) {
    return move_1->move_type < move_2->move_type;
  }
  if (move_1->row_start != move_2->row_start) {
    return move_1->row_start < move_2->row_start;
  }
  if (move_1->col_start != move_2->col_start) {
    return move_1->col_start < move_2->col_start;
  }
  if (move_1->vertical != move_2->vertical) {
    return move_2->vertical;
  }
  if (move_1->tiles_played != move_2->tiles_played) {
    return move_1->tiles_played < move_2->tiles_played;
  }
  if (move_1->tiles_length != move_2->tiles_length) {
    return move_1->tiles_length < move_2->tiles_length;
  }
  for (int i = 0; i < move_1->tiles_length; i++) {
    if (move_1->tiles[i] != move_2->tiles[i]) {
      return move_1->tiles[i] < move_2->tiles[i];
    }
  }
  if (move_1->move_type == GAME_EVENT_PASS) {
    return 0;
  }
  log_fatal("duplicate move in move list detected\n");
  return 0;
}

void up_heapify(MoveList *ml, int index) {
  Move *temp;
  int parent_node = (index - 1) / 2;

  if (index > 0 && compare_moves(ml->moves[parent_node], ml->moves[index])) {
    temp = ml->moves[parent_node];
    ml->moves[parent_node] = ml->moves[index];
    ml->moves[index] = temp;
    up_heapify(ml, parent_node);
  }
}

void down_heapify(MoveList *ml, int parent_node) {
  int left = parent_node * 2 + 1;
  int right = parent_node * 2 + 2;
  int min;
  Move *temp;

  if (left >= ml->count || left < 0)
    left = -1;
  if (right >= ml->count || right < 0)
    right = -1;

  if (left != -1 && compare_moves(ml->moves[parent_node], ml->moves[left]))
    min = left;
  else
    min = parent_node;
  if (right != -1 && compare_moves(ml->moves[min], ml->moves[right]))
    min = right;

  if (min != parent_node) {
    temp = ml->moves[min];
    ml->moves[min] = ml->moves[parent_node];
    ml->moves[parent_node] = temp;
    down_heapify(ml, min);
  }
}

void set_move(Move *move, uint8_t strip[], int leftstrip, int rightstrip,
              int score, int row_start, int col_start, int tiles_played,
              int vertical, game_event_t move_type) {
  move->score = score;
  move->row_start = row_start;
  move->col_start = col_start;
  move->tiles_played = tiles_played;
  move->vertical = vertical;
  move->move_type = move_type;
  move->tiles_length = rightstrip - leftstrip + 1;
  if (move_type != GAME_EVENT_PASS) {
    for (int i = 0; i < move->tiles_length; i++) {
      move->tiles[i] = strip[leftstrip + i];
    }
  }
}

void copy_move(Move *src_move, Move *dest_move) {
  for (int i = 0; i < (BOARD_DIM); i++) {
    dest_move->tiles[i] = src_move->tiles[i];
  }
  dest_move->score = src_move->score;
  dest_move->row_start = src_move->row_start;
  dest_move->col_start = src_move->col_start;
  dest_move->tiles_played = src_move->tiles_played;
  dest_move->tiles_length = src_move->tiles_length;
  dest_move->equity = src_move->equity;
  dest_move->vertical = src_move->vertical;
  dest_move->move_type = src_move->move_type;
}

void set_move_as_pass(Move *move) {
  set_move(move, NULL, 0, 0, 0, 0, 0, 0, 0, GAME_EVENT_PASS);
}

void set_spare_move_as_pass(MoveList *ml) { set_move_as_pass(ml->spare_move); }

void set_spare_move(MoveList *ml, uint8_t strip[], int leftstrip,
                    int rightstrip, int score, int row_start, int col_start,
                    int tiles_played, int vertical, game_event_t move_type) {
  set_move(ml->spare_move, strip, leftstrip, rightstrip, score, row_start,
           col_start, tiles_played, vertical, move_type);
}

void insert_spare_move(MoveList *ml, double equity) {
  ml->spare_move->equity = equity;

  Move *swap = ml->moves[ml->count];
  ml->moves[ml->count] = ml->spare_move;
  ml->spare_move = swap;

  up_heapify(ml, ml->count);
  ml->count++;

  if (ml->count == ml->capacity) {
    pop_move(ml);
  }
}

void insert_spare_move_top_equity(MoveList *ml, double equity) {
  ml->spare_move->equity = equity;
  if (compare_moves(ml->spare_move, ml->moves[0])) {
    Move *swap = ml->moves[0];
    ml->moves[0] = ml->spare_move;
    ml->spare_move = swap;
  }
}

Move *pop_move(MoveList *ml) {
  if (ml->count == 1) {
    ml->count--;
    return ml->moves[0];
  }
  Move *swap = ml->spare_move;
  ml->spare_move = ml->moves[0];
  ml->moves[0] = ml->moves[ml->count - 1];
  ml->moves[ml->count - 1] = swap;

  ml->count--;
  down_heapify(ml, 0);
  return ml->spare_move;
}

void sort_moves(MoveList *ml) {
  int number_of_moves = ml->count;
  for (int i = 1; i < number_of_moves; i++) {
    Move *move = pop_move(ml);
    // Use a swap var to preserve the spare leave pointer
    Move *swap = ml->moves[ml->count];
    ml->moves[ml->count] = move;
    ml->spare_move = swap;
  }
}

// Human readable print function

void string_builder_add_move_description(Move *move, LetterDistribution *ld,
                                         StringBuilder *move_string_builder) {
  if (move->move_type != GAME_EVENT_PASS) {
    if (move->move_type == GAME_EVENT_TILE_PLACEMENT_MOVE) {
      if (move->vertical) {
        string_builder_add_formatted_string(move_string_builder, "%c%d ",
                                            move->col_start + 'A',
                                            move->row_start + 1);
      } else {
        string_builder_add_formatted_string(move_string_builder, "%d%c ",
                                            move->row_start + 1,
                                            move->col_start + 'A');
      }
    } else {
      string_builder_add_string(move_string_builder, "(Exch ", 0);
    }

    int number_of_tiles_to_print = move->tiles_length;

    // FIXME: make sure tiles_length == tiles_played for exchanges
    // this is not true currently.
    if (move->move_type == GAME_EVENT_EXCHANGE) {
      number_of_tiles_to_print = move->tiles_played;
    }

    for (int i = 0; i < number_of_tiles_to_print; i++) {
      uint8_t letter = move->tiles[i];
      if (letter == PLAYED_THROUGH_MARKER &&
          move->move_type == GAME_EVENT_TILE_PLACEMENT_MOVE) {
        string_builder_add_char(move_string_builder, ASCII_PLAYED_THROUGH);
      } else {
        string_builder_add_user_visible_letter(ld, letter, 0,
                                               move_string_builder);
      }
    }
    if (move->move_type == GAME_EVENT_EXCHANGE) {
      string_builder_add_string(move_string_builder, ")", 0);
    }
  } else {
    string_builder_add_string(move_string_builder, "(Pass)", 0);
  }
}

void string_builder_add_move(Board *board, Move *m,
                             LetterDistribution *letter_distribution,
                             StringBuilder *string_builder) {
  if (m->move_type == GAME_EVENT_PASS) {
    string_builder_add_string(string_builder, "pass 0", 0);
    return;
  }

  if (m->move_type == GAME_EVENT_EXCHANGE) {
    string_builder_add_string(string_builder, "(exch ", 0);
    for (int i = 0; i < m->tiles_played; i++) {
      string_builder_add_user_visible_letter(letter_distribution, m->tiles[i],
                                             0, string_builder);
    }
    string_builder_add_string(string_builder, ")", 0);
    return;
  }

  if (m->vertical) {
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
        string_builder_add_string(string_builder, "(", 0);
      }
    }

    if (tile == PLAYED_THROUGH_MARKER && !board) {
      string_builder_add_string(string_builder, ".", 0);
    } else {
      string_builder_add_user_visible_letter(letter_distribution, print_tile, 0,
                                             string_builder);
    }

    if (board && (tile == PLAYED_THROUGH_MARKER) &&
        (i == m->tiles_length - 1 ||
         m->tiles[i + 1] != PLAYED_THROUGH_MARKER)) {
      string_builder_add_string(string_builder, ")", 0);
    }

    if (board && tile != PLAYED_THROUGH_MARKER && (i + 1 < m->tiles_length) &&
        m->tiles[i + 1] == PLAYED_THROUGH_MARKER) {
      string_builder_add_string(string_builder, "(", 0);
    }

    if (m->vertical) {
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