#include "../src/def/board_defs.h"
#include "../src/def/equity_defs.h"
#include "../src/def/game_history_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/ent/board.h"
#include "../src/ent/equity.h"
#include "../src/ent/move.h"
#include <assert.h>

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

void test_move(void) {
  // The majority of the move and move list functionalities
  // are tested in movegen tests.
  test_move_resize();
  test_move_compare();
  test_move_set_as_pass();
}