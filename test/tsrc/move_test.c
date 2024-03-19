#include <assert.h>
#include <stdlib.h>

#include "../../src/def/board_defs.h"
#include "../../src/def/game_history_defs.h"

#include "../../src/ent/board.h"
#include "../../src/ent/move.h"

void test_move_resize() {
  MoveList *ml = move_list_create(3);

  assert(move_list_get_capacity(ml) == 3);

  Move *m1 = move_list_get_move(ml, 0);
  Move *m2 = move_list_get_move(ml, 1);
  Move *m3 = move_list_get_move(ml, 2);

  int m1_score = 1234;
  int m2_score = 5678;
  int m3_score = 99834;

  move_set_score(m1, m1_score);
  move_set_score(m2, m2_score);
  move_set_score(m3, m3_score);

  move_list_resize(ml, 5);

  // Resizing should leave existing moves unchanged;

  assert(move_list_get_move(ml, 0) == m1);
  assert(move_list_get_move(ml, 1) == m2);
  assert(move_list_get_move(ml, 2) == m3);

  assert(move_get_score(m1) == m1_score);
  assert(move_get_score(m2) == m2_score);
  assert(move_get_score(m3) == m3_score);

  move_list_destroy(ml);
}

void test_move_compare() {

  MoveList *ml = move_list_create(1);

  int leftstrip = 2;
  int rightstrip = 9;
  int score = 2;
  int row_start = 3;
  int col_start = 4;
  int tiles_played = 5;
  int dir = BOARD_HORIZONTAL_DIRECTION;
  game_event_t mtype = GAME_EVENT_TILE_PLACEMENT_MOVE;
  double equity = 6.7;

  int tiles_length = rightstrip - leftstrip + 1;

  uint8_t tiles[BOARD_DIM];

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

void test_move() {
  // The majority of the move and move list functionalities
  // are tested in movegen tests.
  test_move_resize();
  test_move_compare();
}