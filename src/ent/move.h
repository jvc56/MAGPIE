#ifndef MOVE_H
#define MOVE_H

#include "../def/board_defs.h"
#include "../def/game_history_defs.h"
#include "../def/move_defs.h"
#include "../ent/equity.h"
#include "../ent/letter_distribution.h"
#include "../ent/rack.h"
#include "../util/io_util.h"
#include "board.h"
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
  MOVE_LIST_TYPE_DEFAULT,
  MOVE_LIST_TYPE_SMALL,
} move_list_type_t;

typedef struct Move {
  Equity score;
  Equity equity;
  game_event_t move_type;
  uint8_t row_start;
  uint8_t col_start;
  // Number of tiles played or exchanged
  uint8_t tiles_played;
  // Equal to tiles_played for exchanges
  uint8_t tiles_length;
  uint8_t dir;
  MachineLetter tiles[MOVE_MAX_TILES];
} Move;

typedef struct SmallMove {
  // tiny_move 64-bit schema:
  // From left to right, (63 to 0):
  // - 1 bit (I) indicating whether this is an invalid SmallMove. If it is 1,
  // this is not a valid move.
  // - 1 reserved bit
  // - 42 bits (7 groups of 6 bits) representing each tile value in the move.
  // The tiles go from 7 to 1 (so start on the right). If a tile is 0, the move
  // has ended.
  // - 1 reserved bit
  // - 7 bit flags, representing whether the associated tile is a blank or not
  // (1 = blank, 0 = no blank)
  // - 1 bit (E) indicating whether this small move is an exchange. We currently
  // do not generate exchanges in endgames but we may want to expand the use of
  // SmallMove in the future, so let's keep it flexible.
  // - 5 bits for row
  // - 5 bits for column
  // - 1 bit for horiz/vert (horiz = 0, vert = 1)

  // If move is a pass, the entire value is 0.
  // Bit layout:
  // 63   59   55   51   47   43   39   35   31   27   23   19   15   11
  //  xxxx xxxx xxxx xxxx xxxx xxxx xxxx xxxx xxxx xxxx xxxx xxxx xxxx xxxx
  //  I 77 7777 6666 6655 5555 4444 4433 3333 2222 2211 1111  BBB BBBB ERRR
  //
  // 7    3
  // xxxx xxxx
  // RRCC CCCV
  uint64_t tiny_move;
  // metadata schema:
  // From left to right (63 to 0):
  // - estimated_value (32 bits, representing a 2s-complement signed value).
  // - 8 bits for num_tiles_from_rack
  // - 8 bits for play_length (number of tiles in play, including playthrough)
  // - 16 bits for the score of the play
  uint64_t metadata;
} SmallMove;

#define SMALL_MOVE_COL_BITMASK 0x3E   // 0b00111110
#define SMALL_MOVE_ROW_BITMASK 0x07C0 // 0b00000111_11000000
#define SMALL_MOVE_BLANKS_BIT_MASK (uint64_t)(127ULL << 12)
#define INVALID_TINY_MOVE (uint64_t)(1ULL << 63)

static const uint64_t SMALL_MOVE_T_BITMASK[7] = {
    (uint64_t)(63ULL << 20), // T1
    (uint64_t)(63ULL << 26), // T2
    (uint64_t)(63ULL << 32), // T3
    (uint64_t)(63ULL << 38), // T4
    (uint64_t)(63ULL << 44), // T5
    (uint64_t)(63ULL << 50), // T6
    (uint64_t)(63ULL << 56)  // T7
};

typedef struct MoveList {
  int count;
  int capacity;
  int moves_size;
  Move *spare_move;
  Move **moves;
  SmallMove *spare_small_move;
  SmallMove **small_moves;
  Rack rack;
} MoveList;

static inline Move *move_create(void) {
  return (Move *)malloc_or_die(sizeof(Move));
}

static inline void move_destroy(Move *move) {
  if (!move) {
    return;
  }
  free(move);
}

static inline game_event_t move_get_type(const Move *move) {
  return move->move_type;
}

static inline Equity move_get_score(const Move *move) { return move->score; }

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

static inline Equity move_get_equity(const Move *move) { return move->equity; }

static inline int move_get_dir(const Move *move) { return move->dir; }

static inline MachineLetter move_get_tile(const Move *move, int index) {
  return move->tiles[index];
}

static inline void move_set_type(Move *move, game_event_t move_type) {
  move->move_type = move_type;
}

static inline void move_set_score(Move *move, Equity score) {
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

static inline void move_set_tile(Move *move, MachineLetter tile, int index) {
  if (index >= 0 && index < BOARD_DIM) {
    move->tiles[index] = tile;
  }
}

static inline void move_set_equity(Move *move, Equity equity) {
  move->equity = equity;
}

static inline void move_set_all_except_equity(Move *move,
                                              const MachineLetter *strip,
                                              int leftstrip, int rightstrip,
                                              Equity score, int row_start,
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
  if (strip) {
    for (int i = 0; i < move->tiles_length; i++) {
      move->tiles[i] = strip[leftstrip + i];
    }
  }
}

static inline void move_set_all(Move *move, MachineLetter strip[],
                                int leftstrip, int rightstrip, Equity score,
                                int row_start, int col_start, int tiles_played,
                                int dir, game_event_t move_type,
                                Equity leave_value) {
  move_set_all_except_equity(move, strip, leftstrip, rightstrip, score,
                             row_start, col_start, tiles_played, dir,
                             move_type);
  move->equity = score + leave_value;
}

static inline void move_copy(Move *dest_move, const Move *src_move) {
  memcpy(dest_move, src_move, sizeof(Move));
}

static inline void move_set_as_pass(Move *move) {
  move_set_all(move, /*strip=*/NULL, /*leftstrip=*/0, /*rightstrip=*/-1,
               /*score=*/0, /*row_start=*/0, /*col_start=*/0,
               /*tiles_played=*/0, /*dir=*/0, GAME_EVENT_PASS,
               EQUITY_PASS_VALUE);
}

static inline Move *move_list_get_spare_move(const MoveList *ml) {
  return ml->spare_move;
}

static inline int move_list_get_count(const MoveList *ml) { return ml->count; }

static inline int move_list_get_capacity(const MoveList *ml) {
  return ml->capacity;
}

static inline void move_list_set_rack(MoveList *ml, const Rack *rack) {
  ml->rack = *rack;
}

static inline const Rack *move_list_get_rack(const MoveList *ml) {
  return &(ml->rack);
}

static inline Move *move_list_get_move(const MoveList *ml, int move_index) {
  return ml->moves[move_index];
}

static inline SmallMove *small_move_list_get_spare_move(const MoveList *ml) {
  return ml->spare_small_move;
}

static inline void small_move_set_as_pass(SmallMove *move) {
  move->metadata = 0;
  move->tiny_move = 0;
}

static inline void
small_move_set_all(SmallMove *move, const MachineLetter strip[], int leftstrip,
                   int rightstrip, Equity score, int row_start, int col_start,
                   int tiles_played, bool dir_is_vertical,
                   game_event_t move_type) {

  int play_length;
  if (move_type == GAME_EVENT_EXCHANGE) {
    play_length = tiles_played;
  } else {
    play_length = rightstrip - leftstrip + 1;
  }

  const int score_int = equity_to_int(score);
  move->metadata = score_int | (play_length << 16) | (tiles_played << 24);

  if (move_type == GAME_EVENT_PASS) {
    move->tiny_move = 0;
    return;
  }
  uint64_t move_code = 0;
  int tidx = 0;
  int bts = 20; // start at bitshift of 20 for the first tile
  int blanks_mask = 0;
  for (int i = 0; i < play_length; i++) {
    MachineLetter ml = strip[leftstrip + i];
    if (ml == PLAYED_THROUGH_MARKER) {
      // play-through tile
      continue;
    }
    uint64_t val = ml;
    if (get_is_blanked(ml)) {
      blanks_mask |= (1 << tidx);
      val = get_unblanked_machine_letter(ml);
    }
    move_code |= (val << bts);
    tidx++;
    bts += 6;
  }
  if (dir_is_vertical) {
    move_code |= 1;
    // swap row, col
    int swap = row_start;
    row_start = col_start;
    col_start = swap;
  }
  move_code |= (col_start << 1);
  move_code |= (row_start << 6);
  move_code |= (blanks_mask << 12);

  move->tiny_move = move_code;
}

static inline void small_move_destroy(SmallMove *move) {
  if (!move) {
    return;
  }
  free(move);
}

// Returns 1 if move_1 is "better" than move_2
// Returns 0 if move_2 is "better" than move_1
// Returns -1 if the moves are equivalent
// Raises a fatal error if moves are equivalent and duplicates
// are not allowed
// Assumes the moves are not null
static inline int compare_moves(const Move *move_1, const Move *move_2,
                                bool allow_duplicates) {
  if (move_1->equity != move_2->equity) {
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
    log_fatal("duplicate move type '%d' in move list detected",
              move_1->move_type);
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
    log_fatal("duplicate move in move list detected: %d", move_1->move_type);
  }
  return -1;
}

static inline void move_list_insert_spare_move_top_equity(MoveList *ml,
                                                          Equity equity) {
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
  ml->moves = (Move **)malloc_or_die(sizeof(Move *) * ml->moves_size);
  for (int i = 0; i < ml->moves_size; i++) {
    // FIXME: maybe don't alloc for moves here
    ml->moves[i] = move_create();
  }
}

static inline void move_list_load_with_empty_small_moves(MoveList *ml,
                                                         int capacity) {
  ml->capacity = capacity;

  ml->small_moves =
      (SmallMove **)malloc_or_die(sizeof(SmallMove *) * ml->capacity);
  for (int i = 0; i < ml->capacity; i++) {
    // FIXME: maybe don't alloc for moves here
    ml->small_moves[i] = (SmallMove *)malloc_or_die(sizeof(SmallMove));
  }
}

static inline void moves_for_move_list_destroy(MoveList *ml) {
  for (int i = 0; i < ml->moves_size; i++) {
    move_destroy(ml->moves[i]);
  }
  free(ml->moves);
}

static inline MoveList *move_list_create(int capacity) {
  MoveList *ml = (MoveList *)malloc_or_die(sizeof(MoveList));
  ml->count = 0;
  ml->spare_move = move_create();
  move_list_load_with_empty_moves(ml, capacity);
  ml->moves[0]->equity = EQUITY_INITIAL_VALUE;
  return ml;
}

static inline MoveList *move_list_create_small(int capacity) {
  MoveList *ml = (MoveList *)malloc_or_die(sizeof(MoveList));
  ml->count = 0;
  ml->spare_small_move = (SmallMove *)malloc_or_die(sizeof(SmallMove));
  // Create spare_move as well, so that we can use it as a placeholder when
  // converting small moves.
  ml->spare_move = move_create();
  move_list_load_with_empty_small_moves(ml, capacity);
  return ml;
}

static inline MoveList *move_list_duplicate(const MoveList *ml) {
  MoveList *new_ml = (MoveList *)malloc_or_die(sizeof(MoveList));
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
  ml->moves[0]->equity = EQUITY_INITIAL_VALUE;
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

  if (left >= ml->count || left < 0) {
    left = -1;
  }
  if (right >= ml->count || right < 0) {
    right = -1;
  }

  if (left != -1 &&
      compare_moves(ml->moves[parent_node], ml->moves[left], false)) {
    min = left;
  } else {
    min = parent_node;
  }
  if (right != -1 && compare_moves(ml->moves[min], ml->moves[right], false)) {
    min = right;
  }

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

// Assumes the move list is not empty
static inline Equity move_list_peek_equity(const MoveList *ml) {
  return ml->moves[0]->equity;
}

static inline void move_list_insert_spare_move(MoveList *ml, Equity equity) {
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

// Assumes the move list is sorted and has enough capacity to accept
// an additional move
static inline void move_list_add_move_to_sorted_list(MoveList *ml,
                                                     const Move *move) {
  move_copy(ml->moves[ml->count], move);
  ml->count++;
  int current_index = ml->count - 1;
  while (current_index > 0 &&
         compare_moves(ml->moves[current_index - 1], ml->moves[current_index],
                       false) == 0) {
    Move *swap = ml->moves[current_index];
    ml->moves[current_index] = ml->moves[current_index - 1];
    ml->moves[current_index - 1] = swap;
    current_index--;
  }
}

static inline void move_list_resize(MoveList *ml, int new_capacity) {
  if (new_capacity == ml->capacity) {
    return;
  }
  int old_moves_size = ml->moves_size;
  ml->capacity = new_capacity;
  ml->moves_size = new_capacity + 1;
  ml->moves =
      (Move **)realloc_or_die(ml->moves, sizeof(Move *) * ml->moves_size);
  for (int i = old_moves_size; i < ml->moves_size; i++) {
    ml->moves[i] = move_create();
  }
}

static inline bool move_list_move_exists(const MoveList *ml, const Move *m) {
  for (int i = 0; i < ml->count; i++) {
    if (compare_moves(ml->moves[i], m, true) == -1) {
      return true;
    }
  }
  return false;
}

static inline void small_moves_for_move_list_destroy(MoveList *ml) {
  for (int i = 0; i < ml->capacity; i++) {
    small_move_destroy(ml->small_moves[i]);
  }
  free(ml->small_moves);
}

static inline void move_list_set_spare_small_move_as_pass(MoveList *ml) {
  small_move_set_as_pass(ml->spare_small_move);
}

static inline void move_list_insert_spare_small_move(MoveList *ml) {
  // small move list does not use a heap. We just append to the end of the list.
  // we're just swapping pointers here.
  SmallMove *swap = ml->small_moves[ml->count];
  ml->small_moves[ml->count] = ml->spare_small_move;
  ml->spare_small_move = swap;
  ml->count++;
}

static inline void small_move_list_destroy(MoveList *ml) {
  if (!ml) {
    return;
  }
  small_moves_for_move_list_destroy(ml);
  small_move_destroy(ml->spare_small_move);
  move_destroy(ml->spare_move);
  free(ml);
}

static inline void small_move_list_reset(MoveList *ml) { ml->count = 0; }

static inline int small_move_get_tiles_played(const SmallMove *sm) {
  return (int)((sm->metadata >> 24) & 0xFF);
}

static inline void small_move_set_estimated_value(SmallMove *sm, int32_t val) {
  // Cast val to uint32_t and then to uint64_t to ensure a non-negative value
  // for shifting
  uint64_t uval = (uint64_t)(uint32_t)val;
  // Shift left by 32 bits using the unsigned value
  sm->metadata |= (uval << 32);
}

static inline void small_move_add_estimated_value(SmallMove *sm, int32_t val) {
  uint64_t uval = (uint64_t)(uint32_t)val;
  sm->metadata += (uval << 32);
}

static inline uint16_t small_move_get_score(const SmallMove *sm) {
  return sm->metadata & 0xFFFF;
}

static inline bool small_move_is_pass(const SmallMove *sm) {
  return sm->tiny_move == 0;
}

// Sort function from highest to lowest value:
static inline int compare_small_moves_by_estimated_value(const void *a,
                                                         const void *b) {
  const SmallMove *sm1 = (const SmallMove *)a;
  const SmallMove *sm2 = (const SmallMove *)b;

  // Extract estimated values as int32_t from each metadata
  int32_t estimated_value1 = (int32_t)(sm1->metadata >> 32);
  int32_t estimated_value2 = (int32_t)(sm2->metadata >> 32);

  // Compare the estimated values
  if (estimated_value1 > estimated_value2) {
    return -1;
  }
  if (estimated_value1 < estimated_value2) {
    return 1;
  }
  return 0;
}

// Sort function from highest to lowest score:
static inline int compare_small_moves_by_score(const void *a, const void *b) {
  const SmallMove *sm1 = (const SmallMove *)a;
  const SmallMove *sm2 = (const SmallMove *)b;

  // Extract estimated values as int32_t from each metadata
  int32_t score1 = small_move_get_score(sm1);
  int32_t score2 = small_move_get_score(sm2);
  // Compare the estimated values
  if (score1 > score2) {
    return -1;
  }
  if (score1 < score2) {
    return 1;
  }
  return 0;
}

static inline void small_move_to_move(Move *move, const SmallMove *sm,
                                      const Board *board) {
  if (small_move_is_pass(sm)) {
    move_set_as_pass(move);
    return;
  }
  // Convert the small move to a Move*
  int row = (int)((sm->tiny_move & SMALL_MOVE_ROW_BITMASK) >> 6);
  int col = (int)((sm->tiny_move & SMALL_MOVE_COL_BITMASK) >> 1);
  bool vert = false;
  if ((sm->tiny_move & 1) > 0) {
    vert = true;
  }
  int ri = vert ? 1 : 0;
  int ci = vert ? 0 : 1;
  int bdim = BOARD_DIM;
  int r = row;
  int c = col;
  int blank_mask = (int)(sm->tiny_move & SMALL_MOVE_BLANKS_BIT_MASK);
  int tidx = 0;
  int midx = 0;
  int tile_shift = 20;
  bool out_of_bounds = false;

  while (!out_of_bounds) {
    MachineLetter on_board = board_get_letter(board, r, c);
    r += ri;
    c += ci;
    if (r >= bdim || c >= bdim) {
      out_of_bounds = true;
    }
    if (on_board != 0) {
      move->tiles[midx] = 0;
      midx++;
      continue;
    }
    if (tidx > 6) {
      break;
    }
    uint64_t shifted = sm->tiny_move & SMALL_MOVE_T_BITMASK[tidx];
    MachineLetter tile = shifted >> tile_shift;
    if (tile == 0) {
      break;
    }
    if (blank_mask & (1 << (tidx + 12))) {
      tile = get_blanked_machine_letter(tile);
    }
    tidx++;
    tile_shift += 6;
    move->tiles[midx] = tile;
    midx++;
  }
  move->move_type = GAME_EVENT_TILE_PLACEMENT_MOVE;
  move->tiles_length = midx;
  move->tiles_played = tidx;
  move->score = int_to_equity(small_move_get_score(sm));
  move->row_start = row;
  move->col_start = col;
  move->equity = 0;
  move->dir = vert ? BOARD_VERTICAL_DIRECTION : BOARD_HORIZONTAL_DIRECTION;
}

#endif