#include "move_test.h"

#include "../src/def/board_defs.h"
#include "../src/def/equity_defs.h"
#include "../src/def/game_history_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/ent/board.h"
#include "../src/ent/equity.h"
#include "../src/ent/move.h"
#include <assert.h>

// NOLINTNEXTLINE
_Static_assert(sizeof(SmallMove) == 16, "SmallMove must be 16 bytes");

void test_move_resize(void) {
  MoveList *ml = move_list_create(3);

  assert(move_list_get_capacity(ml) == 3);

  Move *move1 = move_list_get_move(ml, 0);
  Move *move2 = move_list_get_move(ml, 1);
  Move *move3 = move_list_get_move(ml, 2);

  int move1_score = 1234;
  int move2_score = 5678;
  int move3_score = 99834;

  move_set_score(move1, move1_score);
  move_set_score(move2, move2_score);
  move_set_score(move3, move3_score);

  move_list_resize(ml, 5);

  // Resizing should leave existing moves unchanged;

  assert(move_list_get_move(ml, 0) == move1);
  assert(move_list_get_move(ml, 1) == move2);
  assert(move_list_get_move(ml, 2) == move3);

  assert(move_get_score(move1) == move1_score);
  assert(move_get_score(move2) == move2_score);
  assert(move_get_score(move3) == move3_score);

  move_list_destroy(ml);
}

void test_move_compare(void) {
  MoveList *ml = move_list_create(1);

  int leftstrip = 2;
  int rightstrip = 9;
  int score = 2;
  int row_start = 3;
  int col_start = 4;
  int tiles_played = 5;
  int dir = BOARD_HORIZONTAL_DIRECTION;
  game_event_t mtype = GAME_EVENT_TILE_PLACEMENT_MOVE;
  Equity equity = double_to_equity(6.7);

  int tiles_length = rightstrip - leftstrip + 1;

  MachineLetter tiles[BOARD_DIM];

  for (int i = 0; i < tiles_length; i++) {
    tiles[i] = i + 10;
  }

  Move *m = move_create();
  move_set_all_except_equity(m, tiles, leftstrip, rightstrip, score, row_start,
                             col_start, tiles_played, dir, mtype);
  move_set_equity(m, equity);

  Move *spare_move = move_list_get_spare_move(ml);
  move_copy(spare_move, m);

  move_list_insert_spare_move(ml, equity);

  assert(move_list_move_exists(ml, m));

  move_set_score(m, score + 1);
  assert(!move_list_move_exists(ml, m));

  move_set_score(m, score);
  move_set_row_start(m, row_start + 1);
  assert(!move_list_move_exists(ml, m));

  move_set_row_start(m, row_start);
  move_set_col_start(m, col_start + 1);
  assert(!move_list_move_exists(ml, m));

  move_set_row_start(m, row_start);
  move_set_col_start(m, col_start + 1);
  assert(!move_list_move_exists(ml, m));

  move_set_col_start(m, col_start);
  move_set_tiles_played(m, tiles_played + 1);
  assert(!move_list_move_exists(ml, m));

  move_set_tiles_played(m, tiles_played);
  move_set_tiles_length(m, tiles_length + 1);
  assert(!move_list_move_exists(ml, m));

  move_set_tiles_length(m, tiles_length);
  move_set_dir(m, board_toggle_dir(dir));
  assert(!move_list_move_exists(ml, m));

  move_set_dir(m, dir);
  move_set_type(m, GAME_EVENT_EXCHANGE);
  assert(!move_list_move_exists(ml, m));

  move_set_type(m, GAME_EVENT_TILE_PLACEMENT_MOVE);
  move_set_equity(m, equity + 1);
  assert(!move_list_move_exists(ml, m));

  move_set_equity(m, equity);
  move_set_tile(m, 2, 5);
  assert(!move_list_move_exists(ml, m));

  // Should be 17 since the tiles are set
  // using the leftstrip as an offset:
  // move->tiles[i] = strip[leftstrip + i];
  move_set_tile(m, 17, 5);
  assert(move_list_move_exists(ml, m));

  move_list_destroy(ml);
  move_destroy(m);
}

void test_move_set_as_pass(void) {
  Move *m = move_create();
  move_set_as_pass(m);

  assert(move_get_type(m) == GAME_EVENT_PASS);
  assert(move_get_score(m) == 0);
  assert(move_get_row_start(m) == 0);
  assert(move_get_col_start(m) == 0);
  assert(move_get_tiles_played(m) == 0);
  assert(move_get_tiles_length(m) == 0);
  assert(move_get_dir(m) == BOARD_HORIZONTAL_DIRECTION);
  assert(move_get_equity(m) == EQUITY_PASS_VALUE);

  move_destroy(m);
}

static void test_small_move_list_reuse(void) {
  const int capacities[] = {0, 1, 3, 64};
  for (unsigned capacity_idx = 0;
       capacity_idx < sizeof(capacities) / sizeof(capacities[0]);
       capacity_idx++) {
    const int capacity = capacities[capacity_idx];
    MoveList *list = move_list_create_small(capacity);
    for (int round = 0; round < 3; round++) {
      small_move_list_reset(list);
      for (int move_idx = 0; move_idx < capacity; move_idx++) {
        small_move_set_as_pass(list->spare_small_move);
        list->spare_small_move->metadata.score = (round * capacity) + move_idx;
        move_list_insert_spare_small_move(list);
      }
      assert(list->count == capacity);
      // Ordering and reuse can move the original spare into any live slot.
      for (int move_idx = 0; move_idx < capacity; move_idx++) {
        assert(small_move_get_score(list->small_moves[move_idx]) ==
               (round * capacity) + move_idx);
        assert(list->small_moves[move_idx] != list->spare_small_move);
        for (int other_idx = move_idx + 1; other_idx < capacity; other_idx++) {
          assert(list->small_moves[move_idx] != list->small_moves[other_idx]);
        }
      }
      if (capacity > 1) {
        SmallMove *last = list->small_moves[capacity - 1];
        list->small_moves[capacity - 1] = list->small_moves[0];
        list->small_moves[0] = last;
      }
    }
    small_move_list_destroy(list);
  }
}

void test_move(void) {
  // The majority of the move and move list functionalities
  // are tested in movegen tests.
  test_small_move_list_reuse();
  test_move_resize();
  test_move_compare();
  test_move_set_as_pass();
}