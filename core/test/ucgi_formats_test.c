#include <assert.h>

#include "../src/config.h"
#include "../src/game.h"
#include "../src/move.h"
#include "../src/string_util.h"
#include "../src/ucgi_formats.h"
#include "test_constants.h"

void test_score_move_1() {
  Config *config = NULL;
  load_config_from_lexargs(&config, JOSH2_CGP, "CSW21", "english");
  Game *game = create_game(config);
  load_cgp(game, JOSH2_CGP);
  Move *move = create_move();
  char desc[] = "d9.GALLIC";
  to_move(game->gen->letter_distribution, desc, move, game->gen->board);
  assert(move->move_type == GAME_EVENT_TILE_PLACEMENT_MOVE);
  assert(move->col_start == 3);
  assert(move->row_start == 8);
  assert(move->vertical == 1);
  assert(move->score == 18);
  assert(memory_compare(move->tiles, (uint8_t[]){7, 0, 12, 12, 9, 3}, 6) == 0);
  assert(move->tiles_length == 6);
  assert(move->tiles_played == 5);

  destroy_game(game);
  destroy_move(move);
  destroy_config(config);
}

void test_score_move_2() {
  Config *config = NULL;
  load_config_from_lexargs(&config, GUY_VS_BOT_CGP, "CSW21", "english");
  Game *game = create_game(config);
  load_cgp(game, GUY_VS_BOT_CGP);
  Move *move = create_move();
  char desc[] = "k6.pAVONINE";
  to_move(game->gen->letter_distribution, desc, move, game->gen->board);
  assert(move->move_type == GAME_EVENT_TILE_PLACEMENT_MOVE);
  assert(move->col_start == 10);
  assert(move->row_start == 5);
  assert(move->vertical == 1);
  assert(move->score == 70);
  assert(memory_compare(move->tiles,
                        (uint8_t[]){16 | 0x80, 1, 0, 15, 14, 9, 14, 5},
                        7) == 0);
  assert(move->tiles_length == 8);
  assert(move->tiles_played == 7);

  destroy_game(game);
  destroy_move(move);
  destroy_config(config);
}

void test_score_move_3() {
  Config *config = NULL;
  load_config_from_lexargs(&config, GUY_VS_BOT_CGP, "CSW21", "english");
  Game *game = create_game(config);
  load_cgp(game, GUY_VS_BOT_CGP);
  Move *move = create_move();
  char desc[] = "8g.NAGAVEION"; // some insane phony
  to_move(game->gen->letter_distribution, desc, move, game->gen->board);
  assert(move->move_type == GAME_EVENT_TILE_PLACEMENT_MOVE);
  assert(move->col_start == 6);
  assert(move->row_start == 7);
  assert(move->vertical == 0);
  assert(move->score == 41);
  assert(memory_compare(move->tiles, (uint8_t[]){14, 0, 0, 0, 0, 0, 9, 15, 14},
                        9) == 0);
  assert(move->tiles_length == 9);
  assert(move->tiles_played == 4);

  destroy_game(game);
  destroy_move(move);
  destroy_config(config);
}

void test_exch_move() {
  Config *config = NULL;
  load_config_from_lexargs(&config, GUY_VS_BOT_CGP, "CSW21", "english");
  Game *game = create_game(config);
  load_cgp(game, GUY_VS_BOT_CGP);
  Move *move = create_move();
  char desc[] = "ex.?A";
  to_move(game->gen->letter_distribution, desc, move, game->gen->board);
  assert(move->move_type == GAME_EVENT_EXCHANGE);
  assert(move->score == 0);
  assert(memory_compare(move->tiles, (uint8_t[]){0, 1}, 2) == 0);
  assert(move->tiles_length == 2);
  assert(move->tiles_played == 2);

  destroy_game(game);
  destroy_move(move);
  destroy_config(config);
}

void test_ucgi_formats() {
  test_score_move_1();
  test_score_move_2();
  test_score_move_3();
  test_exch_move();
}