#include "../src/ent/game.h"
#include "../src/ent/heat_map.h"
#include "../src/ent/move.h"
#include "../src/ent/validated_move.h"
#include "../src/impl/config.h"
#include "test_constants.h"
#include "test_util.h"
#include <assert.h>

void assert_heat_map_add_move(const Game *game, const char *ucgi_move_str,
                              HeatMap *hm, int play, int ply) {
  ErrorStack *error_stack = error_stack_create();
  ValidatedMoves *vms = validated_moves_create(game, 0, ucgi_move_str, true,
                                               false, true, error_stack);
  assert(error_stack_is_empty(error_stack));
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    assert(0);
  }
  heat_map_add_move(hm, play, ply, validated_moves_get_move(vms, 0));
  validated_moves_destroy(vms);
  error_stack_destroy(error_stack);
}

void assert_heat_map_values(HeatMap *hm,
                            const uint64_t expected_total_count_sum, int play,
                            int ply, int row, int col, heat_map_t type,
                            const uint64_t expected_count) {
  assert(heat_map_get_total_counts_sum(hm) == expected_total_count_sum);
  const uint64_t actual_total_count_sum = heat_map_get_total_counts_sum(hm);
  if (actual_total_count_sum != expected_total_count_sum) {
    printf("heat map total mismatch:\n");
    printf("play: %d, ply: %d, row: %d, col: %d, type: %d\n", play, ply, row,
           col, type);
    printf("actual_total_count_sum:   %lu\n", actual_total_count_sum);
    printf("expected_total_count_sum: %lu\n", expected_total_count_sum);
    assert(0);
  }
  const uint64_t actual_count =
      heat_map_get_count(hm, play, ply, row, col, type);
  if (actual_count != expected_count) {
    printf("heat map counts mismatch:\n");
    printf("play: %d, ply: %d, row: %d, col: %d, type: %d\n", play, ply, row,
           col, type);
    printf("actual_count:   %lu\n", actual_count);
    printf("expected_count: %lu\n", expected_count);
    assert(0);
  }
}

void assert_heat_map_single_move(const Game *game, const char *ucgi_move_str,
                                 HeatMap *hm, int play, int ply) {
  ErrorStack *error_stack = error_stack_create();
  ValidatedMoves *vms = validated_moves_create(game, 0, ucgi_move_str, true,
                                               false, true, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    assert(0);
  }
  const Move *move = validated_moves_get_move(vms, 0);
  const int hm_num_plays = (int)heat_map_get_num_plays(hm);
  const int hm_num_plies = (int)heat_map_get_num_plies(hm);
  heat_map_reset(hm, hm_num_plays, hm_num_plies);
  heat_map_add_move(hm, play, ply, move);
  assert(heat_map_get_board_total_moves(hm, play, ply, HEAT_MAP_TYPE_ALL) == 1);
  const int move_tiles_played = move_get_tiles_played(move);
  const bool is_bingo = move_tiles_played == RACK_SIZE;
  uint64_t expected_total_count_sum = move_tiles_played;
  if (is_bingo) {
    expected_total_count_sum *= 2;
  }
  assert(heat_map_get_total_counts_sum(hm) == expected_total_count_sum);
  const int move_len = move_get_tiles_length(move);
  int *move_coords = malloc_or_die(sizeof(int) * move_len * 2);
  const int row_inc = move_get_dir(move) == BOARD_VERTICAL_DIRECTION;
  const int col_inc = move_get_dir(move) == BOARD_HORIZONTAL_DIRECTION;
  int curr_row = move_get_row_start(move);
  int curr_col = move_get_col_start(move);
  for (int i = 0; i < move_len; i++) {
    move_coords[i * 2] = curr_row;
    move_coords[i * 2 + 1] = curr_col;
    curr_row += row_inc;
    curr_col += col_inc;
  }
  int move_coords_index = 0;
  for (int i = 0; i < hm_num_plays; i++) {
    for (int j = 0; j < hm_num_plies; j++) {
      for (int k = 0; k < BOARD_DIM; k++) {
        for (int l = 0; l < BOARD_DIM; l++) {
          const uint64_t all_count =
              heat_map_get_count(hm, i, j, k, l, HEAT_MAP_TYPE_ALL);
          const uint64_t bingo_count =
              heat_map_get_count(hm, i, j, k, l, HEAT_MAP_TYPE_BINGO);
          if (move_coords_index < move_len && i == play && j == ply &&
              k == move_coords[move_coords_index * 2] &&
              l == move_coords[move_coords_index * 2 + 1]) {
            assert(all_count == 1);
            assert(bingo_count == is_bingo);
            move_coords_index++;
          } else {
            assert(all_count == 0);
            assert(bingo_count == 0);
          }
        }
      }
    }
  }

  free(move_coords);

  validated_moves_destroy(vms);
  error_stack_destroy(error_stack);
}

void test_heat_map(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 score -s2 score -r1 all -r2 all -numplays 1");
  load_and_exec_config_or_die(config, "newgame");

  HeatMap *hm = heat_map_create();
  heat_map_reset(hm, 6, 6);

  Game *game = config_get_game(config);

  assert_heat_map_add_move(game, "pass", hm, 0, 0);
  assert(heat_map_get_board_total_moves(hm, 0, 0, HEAT_MAP_TYPE_ALL) == 1);
  assert(heat_map_get_board_total_moves(hm, 0, 1, HEAT_MAP_TYPE_ALL) == 0);
  assert(heat_map_get_board_total_moves(hm, 1, 0, HEAT_MAP_TYPE_ALL) == 0);
  assert(heat_map_get_board_count_max(hm, 0, 0, HEAT_MAP_TYPE_ALL) == 0);
  assert(heat_map_get_board_count_max(hm, 0, 0, HEAT_MAP_TYPE_BINGO) == 0);
  assert_heat_map_values(hm, 0, 0, 0, 0, 0, 0, 0);
  assert_heat_map_add_move(game, "ex.ABC.ABCDEFG", hm, 0, 0);
  assert(heat_map_get_board_total_moves(hm, 0, 0, HEAT_MAP_TYPE_ALL) == 2);
  assert(heat_map_get_board_count_max(hm, 0, 0, HEAT_MAP_TYPE_ALL) == 0);
  assert(heat_map_get_board_count_max(hm, 0, 0, HEAT_MAP_TYPE_BINGO) == 0);
  assert_heat_map_values(hm, 0, 0, 0, 0, 0, 0, 0);
  assert_heat_map_add_move(game, "8h.DO", hm, 0, 0);
  assert(heat_map_get_board_total_moves(hm, 0, 0, HEAT_MAP_TYPE_ALL) == 3);
  assert(heat_map_get_board_count_max(hm, 0, 0, HEAT_MAP_TYPE_ALL) == 1);
  assert(heat_map_get_board_count_max(hm, 0, 0, HEAT_MAP_TYPE_BINGO) == 0);
  assert_heat_map_values(hm, 2, 0, 0, 7, 7, 0, 1);
  assert_heat_map_values(hm, 2, 0, 0, 7, 8, 0, 1);
  assert_heat_map_add_move(game, "8h.PRETZEL", hm, 0, 0);
  assert(heat_map_get_board_total_moves(hm, 0, 0, HEAT_MAP_TYPE_ALL) == 4);
  assert(heat_map_get_board_total_moves(hm, 0, 0, HEAT_MAP_TYPE_BINGO) == 1);
  assert(heat_map_get_board_count_max(hm, 0, 0, HEAT_MAP_TYPE_ALL) == 2);
  assert(heat_map_get_board_count_max(hm, 0, 0, HEAT_MAP_TYPE_BINGO) == 1);
  assert_heat_map_values(hm, 16, 0, 0, 7, 7, 0, 2);
  assert_heat_map_values(hm, 16, 0, 0, 7, 8, 0, 2);
  assert_heat_map_values(hm, 16, 0, 0, 7, 9, 0, 1);
  assert_heat_map_values(hm, 16, 0, 0, 7, 10, 0, 1);
  assert_heat_map_values(hm, 16, 0, 0, 7, 11, 0, 1);
  assert_heat_map_values(hm, 16, 0, 0, 7, 12, 0, 1);
  assert_heat_map_values(hm, 16, 0, 0, 7, 13, 0, 1);
  assert_heat_map_add_move(game, "h8.PRETZEL", hm, 0, 0);
  assert(heat_map_get_board_total_moves(hm, 0, 0, HEAT_MAP_TYPE_ALL) == 5);
  assert(heat_map_get_board_total_moves(hm, 0, 0, HEAT_MAP_TYPE_BINGO) == 2);
  assert(heat_map_get_board_count_max(hm, 0, 0, HEAT_MAP_TYPE_ALL) == 3);
  assert(heat_map_get_board_count_max(hm, 0, 0, HEAT_MAP_TYPE_BINGO) == 2);
  assert_heat_map_values(hm, 30, 0, 0, 7, 7, 0, 3);
  assert_heat_map_values(hm, 30, 0, 0, 8, 7, 0, 1);
  assert_heat_map_values(hm, 30, 0, 0, 9, 7, 0, 1);
  assert_heat_map_values(hm, 30, 0, 0, 10, 7, 0, 1);
  assert_heat_map_values(hm, 30, 0, 0, 11, 7, 0, 1);
  assert_heat_map_values(hm, 30, 0, 0, 12, 7, 0, 1);
  assert_heat_map_values(hm, 30, 0, 0, 13, 7, 0, 1);
  assert_heat_map_add_move(game, "h7.FAV", hm, 0, 1);
  assert(heat_map_get_board_total_moves(hm, 0, 1, HEAT_MAP_TYPE_ALL) == 1);
  assert(heat_map_get_board_total_moves(hm, 0, 1, HEAT_MAP_TYPE_BINGO) == 0);
  assert(heat_map_get_board_total_moves(hm, 0, 0, HEAT_MAP_TYPE_ALL) == 5);
  assert(heat_map_get_board_total_moves(hm, 0, 0, HEAT_MAP_TYPE_BINGO) == 2);
  assert(heat_map_get_board_count_max(hm, 0, 1, HEAT_MAP_TYPE_ALL) == 1);
  assert(heat_map_get_board_count_max(hm, 0, 1, HEAT_MAP_TYPE_BINGO) == 0);
  assert(heat_map_get_board_count_max(hm, 0, 0, HEAT_MAP_TYPE_ALL) == 3);
  assert(heat_map_get_board_count_max(hm, 0, 0, HEAT_MAP_TYPE_BINGO) == 2);
  assert_heat_map_values(hm, 33, 0, 1, 6, 7, 0, 1);
  assert_heat_map_values(hm, 33, 0, 1, 7, 7, 0, 1);
  assert_heat_map_values(hm, 33, 0, 1, 8, 7, 0, 1);
  heat_map_reset(hm, 6, 6);
  assert(heat_map_get_total_counts_sum(hm) == 0);

  load_and_exec_config_or_die(
      config,
      "cgp 15/1AI9IA1/1E11E1/15/15/15/15/15/15/15/15/15/1E11E1/1AI9IA1/15 / "
      "0/0 0");

  assert_heat_map_single_move(game, "1A.RETINAS", hm, 0, 0);
  assert_heat_map_single_move(game, "1A.RETINA", hm, 0, 0);
  assert_heat_map_single_move(game, "A1.RETINAS", hm, 1, 3);
  assert_heat_map_single_move(game, "A1.RETINA", hm, 1, 3);
  assert_heat_map_single_move(game, "O1.FRAWZEY", hm, 1, 0);
  assert_heat_map_single_move(game, "O1.FROZEN", hm, 1, 0);
  assert_heat_map_single_move(game, "1H.FRAWZEY", hm, 0, 3);
  assert_heat_map_single_move(game, "1H.FROZEN", hm, 0, 3);
  assert_heat_map_single_move(game, "O8.BUSUUTI", hm, 0, 3);
  assert_heat_map_single_move(game, "O8.BUSTIS", hm, 0, 3);
  assert_heat_map_single_move(game, "15H.BUSUUTI", hm, 5, 5);
  assert_heat_map_single_move(game, "15H.BUSTIS", hm, 5, 5);
  assert_heat_map_single_move(game, "15A.SHORTEN", hm, 5, 0);
  assert_heat_map_single_move(game, "15A.SHORTS", hm, 5, 0);
  assert_heat_map_single_move(game, "A8.SHORTEN", hm, 5, 4);
  assert_heat_map_single_move(game, "A8.SHORTS", hm, 5, 4);

  load_and_exec_config_or_die(config, "newgame");
  load_and_exec_config_or_die(config, "r ABCDEFG");

  assert_config_exec_status(config, "heat 1", ERROR_STATUS_NO_HEAT_MAP_TO_SHOW);

  load_and_exec_config_or_die(
      config,
      "gsim -iter 1000 -minp 10 -numplays 10 "
      "-useh true -plies 5 -boardcolor ansi -threads 12 -sr rr -thres none");

  assert_config_exec_status(config, "heat 0",
                            ERROR_STATUS_HEAT_MAP_MOVE_INDEX_OUT_OF_RANGE);
  assert_config_exec_status(config, "heat 11",
                            ERROR_STATUS_HEAT_MAP_MOVE_INDEX_OUT_OF_RANGE);
  assert_config_exec_status(config, "heat 1 a b",
                            ERROR_STATUS_EXTRANEOUS_HEAT_MAP_ARG);
  assert_config_exec_status(config, "heat 2 6",
                            ERROR_STATUS_HEAT_MAP_PLY_INDEX_OUT_OF_RANGE);
  assert_config_exec_status(config, "heat 2 0",
                            ERROR_STATUS_HEAT_MAP_PLY_INDEX_OUT_OF_RANGE);
  assert_config_exec_status(config, "heat 2 c",
                            ERROR_STATUS_HEAT_MAP_UNRECOGNIZED_TYPE);
  assert_config_exec_status(config, "heat 2 alls",
                            ERROR_STATUS_HEAT_MAP_UNRECOGNIZED_TYPE);

  printf("starting heat map loop\n");
  for (int i = 0; i < 2; i++) {
    printf("testing heat 1\n");
    load_and_exec_config_or_die(config, "heat 1");
    printf("testing heat 2\n");
    load_and_exec_config_or_die(config, "heat 2");
    printf("testing heat 3\n");
    load_and_exec_config_or_die(config, "heat 3");
    load_and_exec_config_or_die(config, "heat 2 a");
    load_and_exec_config_or_die(config, "heat 3 b");
    load_and_exec_config_or_die(config, "heat 5 1");
    load_and_exec_config_or_die(config, "heat 3 1 a");
    load_and_exec_config_or_die(config, "heat 3 2 b");
    load_and_exec_config_or_die(config, "heat 3 3 a");
    load_and_exec_config_or_die(config, "heat 3 4 b");
    load_and_exec_config_or_die(config, "heat 3 5 b");
    load_and_exec_config_or_die(config, "set -boardcolor none");
  }

  load_and_exec_config_or_die(config, "gsim -iter 1000 -minp 10 -numplays 10 "
                                      "-useh false -plies 5 -boardcolor ansi");
  assert_config_exec_status(config, "heat 1", ERROR_STATUS_NO_HEAT_MAP_TO_SHOW);

  heat_map_destroy(hm);
  config_destroy(config);
}
