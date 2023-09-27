#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "board.h"
#include "constants.h"
#include "log.h"
#include "move.h"

Move *create_move() { return malloc(sizeof(Move)); }

MoveList *create_move_list(int capacity) {
  MoveList *ml = malloc(sizeof(MoveList));
  ml->count = 0;
  ml->capacity = capacity;
  ml->spare_move = create_move();
  ml->moves = malloc(sizeof(Move *) * capacity);
  for (int i = 0; i < capacity; i++) {
    ml->moves[i] = create_move();
  }
  ml->moves[0]->equity = INITIAL_TOP_MOVE_EQUITY;
  return ml;
}

void destroy_move(Move *move) { free(move); }

void destroy_move_list(MoveList *ml) {
  for (int i = 0; i < ml->capacity; i++) {
    destroy_move(ml->moves[i]);
  }
  destroy_move(ml->spare_move);
  free(ml->moves);
  free(ml);
}

void reset_move_list(MoveList *ml) {
  ml->count = 0;
  ml->moves[0]->equity = INITIAL_TOP_MOVE_EQUITY;
}

int within_epsilon_for_equity(double a, double b) { return fabs(a - b) < 1e-6; }

void print_move(Move *m) {
  printf("%2.6f %4d %2d %2d %2d %2d %d %d", m->equity, m->score, m->row_start,
         m->col_start, m->tiles_played, m->tiles_length, m->vertical,
         m->move_type);
  for (int i = 0; i < m->tiles_length; i++) {
    printf("%d ", m->tiles[i]);
  }
  printf("\n");
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

void store_move_description(Move *move, char *placeholder,
                            LetterDistribution *ld) {
  char tiles[20];
  char *tp = tiles;
  for (int i = 0; i < move->tiles_length; i++) {
    if (move->tiles[i] == 0) {
      tp += sprintf(tp, ".");
    } else {
      char tile[MAX_LETTER_CHAR_LENGTH];
      machine_letter_to_human_readable_letter(ld, move->tiles[i], tile);
      tp += sprintf(tp, "%s", tile);
    }
  }
  char coords[20];
  tp = coords;

  if (move->move_type == GAME_EVENT_TILE_PLACEMENT_MOVE) {
    if (move->vertical) {
      tp += sprintf(tp, "%c", move->col_start + 'A');
      tp += sprintf(tp, "%d", move->row_start + 1);
    } else {
      tp += sprintf(tp, "%d", move->row_start + 1);
      tp += sprintf(tp, "%c", move->col_start + 'A');
    }
    sprintf(placeholder, "%s %s", coords, tiles);
  } else if (move->move_type == GAME_EVENT_EXCHANGE) {
    sprintf(placeholder, "(Exch %s)", tiles);
  } else if (move->move_type == GAME_EVENT_PASS) {
    sprintf(placeholder, "(Pass)");
  }
}
