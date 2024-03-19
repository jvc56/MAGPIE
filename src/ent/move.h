#ifndef MOVE_H
#define MOVE_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include "../def/board_defs.h"
#include "../def/game_history_defs.h"
#include "../def/move_defs.h"

#include "../util/log.h"

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
  uint8_t tiles[BOARD_DIM];
} Move;

typedef struct MoveList {
  int count;
  int capacity;
  int moves_size;
  Move *spare_move;
  Move **moves;
} MoveList;

Move *move_create();
void move_destroy(Move *move);

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

void move_set_type(Move *move, game_event_t move_type);
void move_set_score(Move *move, int score);
void move_set_row_start(Move *move, int row_start);
void move_set_col_start(Move *move, int col_start);
void move_set_tiles_played(Move *move, int tiles_played);
void move_set_tiles_length(Move *move, int tiles_length);
void move_set_dir(Move *move, int dir);
void move_set_tile(Move *move, uint8_t tile, int index);
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

void move_set_all(Move *move, uint8_t strip[], int leftstrip, int rightstrip,
                  int score, int row_start, int col_start, int tiles_played,
                  int dir, game_event_t move_type, double leave_value);
void move_set_as_pass(Move *move);
void move_copy(Move *dest_move, const Move *src_move);

typedef struct MoveList MoveList;

MoveList *move_list_create(int capacity);
MoveList *move_list_duplicate(const MoveList *ml);
void move_list_destroy(MoveList *ml);

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

void move_list_set_spare_move_as_pass(MoveList *ml);

static inline bool within_epsilon_for_equity(double a, double b) {
  return fabs(a - b) < COMPARE_MOVES_EPSILON;
}

// Returns 1 if move_1 is "better" than move_2
// Returns 0 if move_2 is "better" than move_1
// Returns -1 if the moves are equivalent
// Dies if moves are equivalent and duplicates
// are not allowed
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

void move_list_insert_spare_move(MoveList *ml, double equity);
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
Move *move_list_pop_move(MoveList *ml);
void move_list_sort_moves(MoveList *ml);
void move_list_reset(MoveList *ml);
bool move_list_move_exists(MoveList *ml, Move *m);
void move_list_resize(MoveList *ml, int new_capacity);

#endif