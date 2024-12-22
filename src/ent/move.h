#ifndef MOVE_H
#define MOVE_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "../def/board_defs.h"
#include "../def/game_history_defs.h"
#include "../def/move_defs.h"

#include "../util/log.h"
#include "../util/util.h"

typedef struct Move {
  game_event_t move_type;
  int score;
  int row_start;
  int col_start;
  // Number of tiles played or exchanged
  int tiles_played;
  // Equal to tiles_played for exchanges
  int tiles_length;
  double equity;
  int dir;
  uint8_t tiles[MOVE_MAX_TILES];
} Move;

typedef struct MoveList {
  int count;
  int capacity;
  int moves_size;
  Move *spare_move;
  Move **moves;
} MoveList;

static inline Move *move_create(void) { return malloc_or_die(sizeof(Move)); }

static inline void move_destroy(Move *move) {
  if (!move) {
    return;
  }
  free(move);
}

static inline game_event_t move_get_type(const Move *move) {
  return move->move_type;
}

static inline int move_get_score(const Move *move) { return move->score; }

static inline int move_get_row_start(const Move *move) {
  return move->row_start;
}

static inline int move_get_col_start(const Move *move) {
  return move->col_start;
}

static inline int move_get_tiles_played(const Move *move) {
  return move->tiles_played;
}

static inline int move_get_tiles_length(const Move *move) {
  return move->tiles_length;
}

static inline double move_get_equity(const Move *move) { return move->equity; }

static inline int move_get_dir(const Move *move) { return move->dir; }

static inline uint8_t move_get_tile(const Move *move, int index) {
  return move->tiles[index];
}

static inline void move_set_type(Move *move, game_event_t move_type) {
  move->move_type = move_type;
}

static inline void move_set_score(Move *move, int score) {
  move->score = score;
}

static inline void move_set_row_start(Move *move, int row_start) {
  move->row_start = row_start;
}

static inline void move_set_col_start(Move *move, int col_start) {
  move->col_start = col_start;
}

static inline void move_set_tiles_played(Move *move, int tiles_played) {
  move->tiles_played = tiles_played;
}

static inline void move_set_tiles_length(Move *move, int tiles_length) {
  move->tiles_length = tiles_length;
}

static inline void move_set_dir(Move *move, int dir) { move->dir = dir; }

static inline void move_set_tile(Move *move, uint8_t tile, int index) {
  if (index >= 0 && index < BOARD_DIM) {
    move->tiles[index] = tile;
  }
}

static inline void move_set_equity(Move *move, double equity) {
  move->equity = equity;
}

static inline void move_set_all_except_equity(Move *move, const uint8_t strip[],
                                              int leftstrip, int rightstrip,
                                              int score, int row_start,
                                              int col_start, int tiles_played,
                                              int dir, game_event_t move_type) {
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

static inline void move_set_all(Move *move, uint8_t strip[], int leftstrip,
                                int rightstrip, int score, int row_start,
                                int col_start, int tiles_played, int dir,
                                game_event_t move_type, double leave_value) {
  move_set_all_except_equity(move, strip, leftstrip, rightstrip, score,
                             row_start, col_start, tiles_played, dir,
                             move_type);
  move->equity = score + leave_value;
}

static inline void move_copy(Move *dest_move, const Move *src_move) {
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

static inline void move_set_as_pass(Move *move) {
  move_set_all(move, /*strip=*/NULL, /*leftstrip=*/0, /*rightstrip=*/-1,
               /*score=*/0, /*row_start=*/0, /*col_start=*/0,
               /*tiles_played=*/0, /*dir=*/0, GAME_EVENT_PASS,
               PASS_MOVE_EQUITY);
}

static inline Move *move_list_get_spare_move(const MoveList *ml) {
  return ml->spare_move;
}

static inline int move_list_get_count(const MoveList *ml) { return ml->count; }

static inline int move_list_get_capacity(const MoveList *ml) {
  return ml->capacity;
}

static inline Move *move_list_get_move(const MoveList *ml, int move_index) {
  return ml->moves[move_index];
}

static inline void move_list_set_spare_move(MoveList *ml, uint8_t strip[],
                                            int leftstrip, int rightstrip,
                                            int score, int row_start,
                                            int col_start, int tiles_played,
                                            int dir, game_event_t move_type) {
  move_set_all_except_equity(ml->spare_move, strip, leftstrip, rightstrip,
                             score, row_start, col_start, tiles_played, dir,
                             move_type);
}

static inline bool within_epsilon_for_equity(double a, double b) {
  return fabs(a - b) < COMPARE_MOVES_EPSILON;
}

// Returns 1 if move_1 is "better" than move_2
// Returns 0 if move_2 is "better" than move_1
// Returns -1 if the moves are equivalent
// Dies if moves are equivalent and duplicates
// are not allowed
// Assumes the moves are not null
static inline int compare_moves(const Move *move_1, const Move *move_2,
                                bool allow_duplicates) {
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
  if (!allow_duplicates) {
    log_fatal("duplicate move in move list detected: %d\n", move_1->move_type);
  }
  return -1;
}

// Does not compare move equity
// Returns 1 if move_1 is "better" than move_2
// Returns 0 if move_2 is "better" than move_1
// Returns -1 if the moves are equivalent
// Dies if moves are equivalent and duplicates
// are not allowed
// Assumes the moves are not null
static inline int compare_moves_without_equity(const Move *move_1,
                                               const Move *move_2,
                                               bool allow_duplicates) {
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
  if (!allow_duplicates) {
    log_fatal("duplicate move in move list detected: %d\n", move_1->move_type);
  }
  return -1;
}

static inline void move_list_insert_spare_move_top_equity(MoveList *ml,
                                                          double equity) {
  ml->spare_move->equity = equity;
  if (compare_moves(ml->spare_move, ml->moves[0], false)) {
    Move *swap = ml->moves[0];
    ml->moves[0] = ml->spare_move;
    ml->spare_move = swap;
    ml->count = 1;
  }
}

static inline void move_list_load_with_empty_moves(MoveList *ml, int capacity) {
  ml->capacity = capacity;
  // We need to use +1 here so that the
  // move list can temporarily hold the
  // the extra move to determine which
  // move to pop.
  ml->moves_size = ml->capacity + 1;
  ml->moves = malloc_or_die(sizeof(Move *) * ml->moves_size);
  for (int i = 0; i < ml->moves_size; i++) {
    ml->moves[i] = move_create();
  }
}

static inline void moves_for_move_list_destroy(MoveList *ml) {
  for (int i = 0; i < ml->moves_size; i++) {
    move_destroy(ml->moves[i]);
  }
  free(ml->moves);
}

static inline MoveList *move_list_create(int capacity) {
  MoveList *ml = malloc_or_die(sizeof(MoveList));
  ml->count = 0;
  ml->spare_move = move_create();
  move_list_load_with_empty_moves(ml, capacity);
  ml->moves[0]->equity = INITIAL_TOP_MOVE_EQUITY;
  return ml;
}

static inline MoveList *move_list_duplicate(const MoveList *ml) {
  MoveList *new_ml = malloc_or_die(sizeof(MoveList));
  new_ml->count = ml->count;
  new_ml->spare_move = move_create();
  move_list_load_with_empty_moves(new_ml, ml->capacity);
  for (int i = 0; i < new_ml->moves_size; i++) {
    move_copy(new_ml->moves[i], ml->moves[i]);
  }
  return new_ml;
}

static inline void move_list_destroy(MoveList *ml) {
  if (!ml) {
    return;
  }
  moves_for_move_list_destroy(ml);
  move_destroy(ml->spare_move);
  free(ml);
}

static inline void move_list_reset(MoveList *ml) {
  ml->count = 0;
  ml->moves[0]->equity = INITIAL_TOP_MOVE_EQUITY;
}

static inline void up_heapify(MoveList *ml, int index) {
  Move *temp;
  int parent_node = (index - 1) / 2;

  if (index > 0 &&
      compare_moves(ml->moves[parent_node], ml->moves[index], false)) {
    temp = ml->moves[parent_node];
    ml->moves[parent_node] = ml->moves[index];
    ml->moves[index] = temp;
    up_heapify(ml, parent_node);
  }
}

static inline void down_heapify(MoveList *ml, int parent_node) {
  int left = parent_node * 2 + 1;
  int right = parent_node * 2 + 2;
  int min;
  Move *temp;

  if (left >= ml->count || left < 0)
    left = -1;
  if (right >= ml->count || right < 0)
    right = -1;

  if (left != -1 &&
      compare_moves(ml->moves[parent_node], ml->moves[left], false))
    min = left;
  else
    min = parent_node;
  if (right != -1 && compare_moves(ml->moves[min], ml->moves[right], false))
    min = right;

  if (min != parent_node) {
    temp = ml->moves[min];
    ml->moves[min] = ml->moves[parent_node];
    ml->moves[parent_node] = temp;
    down_heapify(ml, min);
  }
}

static inline void move_list_set_spare_move_as_pass(MoveList *ml) {
  move_set_as_pass(ml->spare_move);
}

static inline Move *move_list_pop_move(MoveList *ml) {
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

static inline void move_list_insert_spare_move(MoveList *ml, double equity) {
  ml->spare_move->equity = equity;

  Move *swap = ml->moves[ml->count];
  ml->moves[ml->count] = ml->spare_move;
  ml->spare_move = swap;

  up_heapify(ml, ml->count);
  ml->count++;

  if (ml->count == ml->capacity + 1) {
    move_list_pop_move(ml);
  }
}

// Converts the MoveList from a min heap
// to a descending sorted array. The
// count stays constant.
static inline void move_list_sort_moves(MoveList *ml) {
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

static inline void move_list_resize(MoveList *ml, int new_capacity) {
  if (new_capacity == ml->capacity) {
    return;
  }
  int old_moves_size = ml->moves_size;
  ml->capacity = new_capacity;
  ml->moves_size = new_capacity + 1;
  ml->moves = realloc_or_die(ml->moves, sizeof(Move *) * ml->moves_size);
  for (int i = old_moves_size; i < ml->moves_size; i++) {
    ml->moves[i] = move_create();
  }
}

static inline bool move_list_move_exists(MoveList *ml, Move *m) {
  for (int i = 0; i < ml->count; i++) {
    if (compare_moves(ml->moves[i], m, true) == -1) {
      return true;
    }
  }
  return false;
}

#endif