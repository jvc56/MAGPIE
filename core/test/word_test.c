#include <assert.h>
#include <stdint.h>
#include <stdlib.h>

#include "../src/ent/config.h"
#include "../src/ent/game.h"
#include "../src/ent/words.h"
#include "../src/util/string_util.h"

#include "test_constants.h"
#include "test_util.h"

void test_words_played() {
  Config *config = create_config_or_die(
      "setoptions lex NWL20 s1 score s2 score r1 all r2 all numplays 1");
  Game *game = game_create(config);
  game_load_cgp(game, VS_ED);

  Board *board = game_get_board(game);
  Player *player0 = game_get_player(game, 0);
  const KWG *kwg = player_get_kwg(player0);

  // Play PeNT vertically at N11 (col 14, row 11)
  uint8_t PeNT[] = {16, 5 | 0x80, 14, 20};
  // use 0-based indexing.
  FormedWords *fw = words_played(board, PeNT, 0, 3, 10, 13, 1);
  populate_word_validities(kwg, fw);
  assert(formed_words_get_num_words(fw) == 4);
  // Should generate 4 words: PIP, ONE, HEN, and the main word PENT
  assert(formed_words_get_word_length(fw, 0) == 3);
  assert(formed_words_get_word_valid(fw, 0) == 1);
  assert(memory_compare(formed_words_get_word(fw, 0), (uint8_t[]){16, 9, 16},
                        3) == 0);
  assert(formed_words_get_word_length(fw, 1) == 3);
  assert(formed_words_get_word_valid(fw, 1) == 1);
  assert(memory_compare(formed_words_get_word(fw, 1), (uint8_t[]){15, 14, 5},
                        3) == 0);
  assert(formed_words_get_word_length(fw, 2) == 3);
  assert(formed_words_get_word_valid(fw, 2) == 1);
  assert(memory_compare(formed_words_get_word(fw, 2), (uint8_t[]){8, 5, 14},
                        3) == 0);
  assert(formed_words_get_word_length(fw, 3) == 4);
  assert(formed_words_get_word_valid(fw, 3) == 1);
  assert(memory_compare(formed_words_get_word(fw, 3),
                        (uint8_t[]){16, 5, 14, 20}, 4) == 0);
  free(fw);

  // Play some random phoney making a lot of words 6G DI(PET)AZ
  uint8_t DIPETAZ[] = {4, 9, 0, 0, 0, 1, 26};
  fw = words_played(board, DIPETAZ, 0, 6, 5, 6, 0);
  populate_word_validities(kwg, fw);

  assert(formed_words_get_num_words(fw) == 5);
  // should generate 5 "words":
  // OD, WIFAY*, ANE, ZGENUINE*, and the main word DIPETAZ*
  assert(formed_words_get_word_length(fw, 0) == 2);
  assert(formed_words_get_word_valid(fw, 0) == 1);
  assert(memory_compare(formed_words_get_word(fw, 0), (uint8_t[]){15, 4}, 2) ==
         0);
  assert(formed_words_get_word_length(fw, 1) == 5);
  assert(formed_words_get_word_valid(fw, 1) == 0);
  assert(memory_compare(formed_words_get_word(fw, 1),
                        (uint8_t[]){23, 9, 6, 1, 25}, 5) == 0);
  assert(formed_words_get_word_length(fw, 2) == 3);
  assert(formed_words_get_word_valid(fw, 2) == 1);
  assert(memory_compare(formed_words_get_word(fw, 2), (uint8_t[]){1, 14, 5},
                        3) == 0);
  assert(formed_words_get_word_length(fw, 3) == 8);
  assert(formed_words_get_word_valid(fw, 3) == 0);
  assert(memory_compare(formed_words_get_word(fw, 3),
                        (uint8_t[]){26, 7, 5, 14, 21, 9, 14, 5}, 8) == 0);
  assert(formed_words_get_word_length(fw, 4) == 7);
  assert(formed_words_get_word_valid(fw, 4) == 0);
  assert(memory_compare(formed_words_get_word(fw, 4),
                        (uint8_t[]){4, 9, 16, 5, 20, 1, 26}, 7) == 0);

  free(fw);

  // play a single tile that makes two words. 9F (BOY)S
  uint8_t BOYS[] = {0, 0, 0, 19};
  fw = words_played(board, BOYS, 0, 3, 8, 5, 0);
  populate_word_validities(kwg, fw);

  assert(formed_words_get_num_words(fw) == 2);
  // generates SPAYS and BOYS
  assert(formed_words_get_word_length(fw, 0) == 5);
  assert(formed_words_get_word_valid(fw, 0) == 1);
  assert(memory_compare(formed_words_get_word(fw, 0),
                        (uint8_t[]){19, 16, 1, 25, 19}, 5) == 0);
  assert(formed_words_get_word_length(fw, 1) == 4);
  assert(formed_words_get_word_valid(fw, 1) == 1);
  assert(memory_compare(formed_words_get_word(fw, 1),
                        (uint8_t[]){2, 15, 25, 19}, 4) == 0);

  free(fw);

  // same as above but dir - I5 SPAY(S)
  uint8_t SPAYS[] = {0, 0, 0, 0, 19};
  fw = words_played(board, SPAYS, 0, 4, 4, 8, 1);
  populate_word_validities(kwg, fw);

  // generates BOYS and SPAYS
  assert(formed_words_get_word_length(fw, 0) == 4);
  assert(formed_words_get_word_valid(fw, 0) == 1);
  assert(memory_compare(formed_words_get_word(fw, 0),
                        (uint8_t[]){2, 15, 25, 19}, 4) == 0);
  assert(formed_words_get_word_length(fw, 1) == 5);
  assert(formed_words_get_word_valid(fw, 1) == 1);
  assert(memory_compare(formed_words_get_word(fw, 1),
                        (uint8_t[]){19, 16, 1, 25, 19}, 5) == 0);

  free(fw);
  game_destroy(game);
  config_destroy(config);
}

void test_words() { test_words_played(); }