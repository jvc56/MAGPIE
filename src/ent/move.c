#include "move.h"

#include <math.h>
#include <stdbool.h>
#include <stdlib.h>

#include "../def/board_defs.h"
#include "../def/game_history_defs.h"
#include "../def/move_defs.h"

#include "../util/log.h"
#include "../util/util.h"

struct Move {
  game_event_t move_type;
  int score;
  int row_start;
  int col_start;
  int tiles_played;
  int tiles_length;
  double equity;
  int dir;
  uint8_t tiles[BOARD_DIM];
};

struct MoveList {
  int count;
  int capacity;
  Move *spare_move;
  Move **moves;
};

Move *move_create() { return malloc_or_die(sizeof(Move)); }

void move_destroy(Move *move) {
  if (!move) {
    return;
  }
  free(move);
}

game_event_t move_get_type(const Move *move) { return move->move_type; }

int move_get_score(const Move *move) { return move->score; }

int move_get_row_start(const Move *move) { return move->row_start; }

int move_get_col_start(const Move *move) { return move->col_start; }

int move_get_tiles_played(const Move *move) { return move->tiles_played; }

int move_get_tiles_length(const Move *move) { return move->tiles_length; }

double move_get_equity(const Move *move) { return move->equity; }

int move_get_dir(const Move *move) { return move->dir; }

uint8_t move_get_tile(const Move *move, int index) {
  return move->tiles[index];
}

Move *move_list_get_spare_move(const MoveList *ml) { return ml->spare_move; }

int move_list_get_count(const MoveList *ml) { return ml->count; }

int move_list_get_capacity(const MoveList *ml) { return ml->capacity; }

Move *move_list_get_move(const MoveList *ml, int move_index) {
  return ml->moves[move_index];
}

void move_set_type(Move *move, game_event_t move_type) {
  move->move_type = move_type;
}

void move_set_score(Move *move, int score) { move->score = score; }

void move_set_row_start(Move *move, int row_start) {
  move->row_start = row_start;
}

void move_set_col_start(Move *move, int col_start) {
  move->col_start = col_start;
}

void move_set_tiles_played(Move *move, int tiles_played) {
  move->tiles_played = tiles_played;
}

void move_set_tiles_length(Move *move, int tiles_length) {
  move->tiles_length = tiles_length;
}

void move_set_dir(Move *move, int dir) { move->dir = dir; }

void move_set_tile_at_index(Move *move, uint8_t tile, int index) {
  if (index >= 0 && index < BOARD_DIM) {
    move->tiles[index] = tile;
  }
}

bool within_epsilon_for_equity(double a, double board) {
  return fabs(a - board) < COMPARE_MOVES_EPSILON;
}

// Enforce arbitrary order to keep
// move order deterministic
int compare_moves(const Move *move_1, const Move *move_2) {
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
  if (move_1->dir != move_2->dir) {
    return move_2->dir;
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
  log_fatal("duplicate move in move list detected: %d\n", move_1->move_type);
  return 0;
}

void move_set_all_except_equity(Move *move, uint8_t strip[], int leftstrip,
                                int rightstrip, int score, int row_start,
                                int col_start, int tiles_played, int dir,
                                game_event_t move_type) {
  move->score = score;
  move->row_start = row_start;
  move->col_start = col_start;
  move->tiles_played = tiles_played;
  move->dir = dir;
  move->move_type = move_type;
  if (move_type == GAME_EVENT_EXCHANGE) {
    move->tiles_length = move->tiles_played;
  } else {
    move->tiles_length = rightstrip - leftstrip + 1;
  }
  if (move_type != GAME_EVENT_PASS) {
    for (int i = 0; i < move->tiles_length; i++) {
      move->tiles[i] = strip[leftstrip + i];
    }
  }
}

void move_set_all(Move *move, uint8_t strip[], int leftstrip, int rightstrip,
                  int score, int row_start, int col_start, int tiles_played,
                  int dir, game_event_t move_type, double leave_value) {
  move_set_all_except_equity(move, strip, leftstrip, rightstrip, score,
                             row_start, col_start, tiles_played, dir,
                             move_type);
  move->equity = score + leave_value;
}

void move_copy(Move *dest_move, const Move *src_move) {
  for (int i = 0; i < (BOARD_DIM); i++) {
    dest_move->tiles[i] = src_move->tiles[i];
  }
  dest_move->score = src_move->score;
  dest_move->row_start = src_move->row_start;
  dest_move->col_start = src_move->col_start;
  dest_move->tiles_played = src_move->tiles_played;
  dest_move->tiles_length = src_move->tiles_length;
  dest_move->equity = src_move->equity;
  dest_move->dir = src_move->dir;
  dest_move->move_type = src_move->move_type;
}

void move_set_as_pass(Move *move) {
  move_set_all_except_equity(move, NULL, 0, 0, 0, 0, 0, 0, 0, GAME_EVENT_PASS);
}

void create_moves_for_move_list(MoveList *ml, int capacity) {
  ml->capacity = capacity + 1;
  ml->moves = malloc_or_die(sizeof(Move *) * ml->capacity);
  for (int i = 0; i < ml->capacity; i++) {
    ml->moves[i] = move_create();
  }
}

void destroy_moves_for_move_list(MoveList *ml) {
  for (int i = 0; i < ml->capacity; i++) {
    move_destroy(ml->moves[i]);
  }
  free(ml->moves);
}

MoveList *move_list_create(int capacity) {
  MoveList *ml = malloc_or_die(sizeof(MoveList));
  ml->count = 0;
  ml->spare_move = move_create();
  create_moves_for_move_list(ml, capacity);
  ml->moves[0]->equity = INITIAL_TOP_MOVE_EQUITY;
  return ml;
}

void move_list_destroy(MoveList *ml) {
  if (!ml) {
    return;
  }
  destroy_moves_for_move_list(ml);
  move_destroy(ml->spare_move);
  free(ml);
}

void move_list_reset(MoveList *ml) {
  ml->count = 0;
  ml->moves[0]->equity = INITIAL_TOP_MOVE_EQUITY;
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

void move_list_set_spare_move_as_pass(MoveList *ml) {
  move_set_as_pass(ml->spare_move);
}

void move_list_set_spare_move(MoveList *ml, uint8_t strip[], int leftstrip,
                              int rightstrip, int score, int row_start,
                              int col_start, int tiles_played, int dir,
                              game_event_t move_type) {
  move_set_all_except_equity(ml->spare_move, strip, leftstrip, rightstrip,
                             score, row_start, col_start, tiles_played, dir,
                             move_type);
}

void move_list_insert_spare_move(MoveList *ml, double equity) {
  ml->spare_move->equity = equity;

  Move *swap = ml->moves[ml->count];
  ml->moves[ml->count] = ml->spare_move;
  ml->spare_move = swap;

  up_heapify(ml, ml->count);
  ml->count++;

  if (ml->count == ml->capacity) {
    move_list_pop_move(ml);
  }
}

void move_list_insert_spare_move_top_equity(MoveList *ml, double equity) {
  ml->spare_move->equity = equity;
  if (compare_moves(ml->spare_move, ml->moves[0])) {
    Move *swap = ml->moves[0];
    ml->moves[0] = ml->spare_move;
    ml->spare_move = swap;
    ml->count = 1;
  }
}

Move *move_list_pop_move(MoveList *ml) {
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

// Converts the MoveList from a min heap
// to a descending sorted array. The
// count stays constant.
void move_list_sort_moves(MoveList *ml) {
  int number_of_moves = ml->count;
  for (int i = 1; i < number_of_moves; i++) {
    Move *move = move_list_pop_move(ml);
    // Use a swap var to preserve the spare leave pointer
    Move *swap = ml->moves[ml->count];
    ml->moves[ml->count] = move;
    ml->spare_move = swap;
  }
  // Reset the count
  ml->count = number_of_moves;
}
