#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../src/config.h"
#include "../src/game.h"
#include "../src/string_util.h"
#include "../src/words.h"

#include "testconfig.h"
#include "test_constants.h"

void test_words_played(TestConfig *testconfig) {
  Config *config = get_nwl_config(testconfig);
  Game *game = create_game(config);
  load_cgp(game, VS_ED);

  // Play PeNT vertically at N11 (col 14, row 11)
  uint8_t PeNT[] = {16, 5 | 0x80, 14, 20};
  // use 0-based indexing.
  FormedWords *fw = words_played(game->gen->board, PeNT, 0, 3, 10, 13, 1);
  populate_word_validities(fw, game->players[0]->strategy_params->kwg);
  assert(fw->num_words == 4);
  // Should generate 4 words: PIP, ONE, HEN, and the main word PENT
  assert(fw->words[0].word_length == 3);
  assert(fw->words[0].valid == 1);
  assert(memory_compare(fw->words[0].word, (uint8_t[]){16, 9, 16}, 3) == 0);
  assert(fw->words[1].word_length == 3);
  assert(fw->words[1].valid == 1);
  assert(memory_compare(fw->words[1].word, (uint8_t[]){15, 14, 5}, 3) == 0);
  assert(fw->words[2].word_length == 3);
  assert(fw->words[2].valid == 1);
  assert(memory_compare(fw->words[2].word, (uint8_t[]){8, 5, 14}, 3) == 0);
  assert(fw->words[3].word_length == 4);
  assert(fw->words[3].valid == 1);
  assert(memory_compare(fw->words[3].word, (uint8_t[]){16, 5, 14, 20}, 4) == 0);
  free(fw);

  // Play some random phoney making a lot of words 6G DI(PET)AZ
  uint8_t DIPETAZ[] = {4, 9, 0, 0, 0, 1, 26};
  fw = words_played(game->gen->board, DIPETAZ, 0, 6, 5, 6, 0);
  populate_word_validities(fw, game->players[0]->strategy_params->kwg);

  assert(fw->num_words == 5);
  // should generate 5 "words":
  // OD, WIFAY*, ANE, ZGENUINE*, and the main word DIPETAZ*
  assert(fw->words[0].word_length == 2);
  assert(fw->words[0].valid == 1);
  assert(memory_compare(fw->words[0].word, (uint8_t[]){15, 4}, 2) == 0);
  assert(fw->words[1].word_length == 5);
  assert(fw->words[1].valid == 0);
  assert(memory_compare(fw->words[1].word, (uint8_t[]){23, 9, 6, 1, 25}, 5) ==
         0);
  assert(fw->words[2].word_length == 3);
  assert(fw->words[2].valid == 1);
  assert(memory_compare(fw->words[2].word, (uint8_t[]){1, 14, 5}, 3) == 0);
  assert(fw->words[3].word_length == 8);
  assert(fw->words[3].valid == 0);
  assert(memory_compare(fw->words[3].word,
                        (uint8_t[]){26, 7, 5, 14, 21, 9, 14, 5}, 8) == 0);
  assert(fw->words[4].word_length == 7);
  assert(fw->words[4].valid == 0);
  assert(memory_compare(fw->words[4].word, (uint8_t[]){4, 9, 16, 5, 20, 1, 26},
                        7) == 0);

  free(fw);

  // play a single tile that makes two words. 9F (BOY)S
  uint8_t BOYS[] = {0, 0, 0, 19};
  fw = words_played(game->gen->board, BOYS, 0, 3, 8, 5, 0);
  populate_word_validities(fw, game->players[0]->strategy_params->kwg);

  assert(fw->num_words == 2);
  // generates SPAYS and BOYS
  assert(fw->words[0].word_length == 5);
  assert(fw->words[0].valid == 1);
  assert(memory_compare(fw->words[0].word, (uint8_t[]){19, 16, 1, 25, 19}, 5) ==
         0);
  assert(fw->words[1].word_length == 4);
  assert(fw->words[1].valid == 1);
  assert(memory_compare(fw->words[1].word, (uint8_t[]){2, 15, 25, 19}, 4) == 0);

  free(fw);

  // same as above but vertical - I5 SPAY(S)
  uint8_t SPAYS[] = {0, 0, 0, 0, 19};
  fw = words_played(game->gen->board, SPAYS, 0, 4, 4, 8, 1);
  populate_word_validities(fw, game->players[0]->strategy_params->kwg);

  // generates BOYS and SPAYS
  assert(fw->words[0].word_length == 4);
  assert(fw->words[0].valid == 1);
  assert(memory_compare(fw->words[0].word, (uint8_t[]){2, 15, 25, 19}, 4) == 0);
  assert(fw->words[1].word_length == 5);
  assert(fw->words[1].valid == 1);
  assert(memory_compare(fw->words[1].word, (uint8_t[]){19, 16, 1, 25, 19}, 5) ==
         0);

  free(fw);
  destroy_game(game);
}

void test_words(TestConfig *testconfig) { test_words_played(testconfig); }