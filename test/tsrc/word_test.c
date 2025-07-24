#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "../../src/def/letter_distribution_defs.h"

#include "../../src/ent/game.h"
#include "../../src/ent/validated_move.h"
#include "../../src/ent/words.h"
#include "../../src/impl/config.h"

#include "../../src/util/io_util.h"

#include "test_constants.h"
#include "test_util.h"

void test_words_played(void) {
  Config *config = config_create_or_die(
      "set -lex NWL20 -s1 score -s2 score -r1 all -r2 all -numplays 1");
  Game *game = config_game_create(config);
  load_cgp_or_die(game, VS_ED);

  ValidatedMoves *vms_pent = validated_moves_create_and_assert_status(
      game, 0, "N11.PeNT", false, true, false, ERROR_STATUS_SUCCESS);

  const FormedWords *fw = validated_moves_get_formed_words(vms_pent, 0);

  assert(formed_words_get_num_words(fw) == 4);
  // Should generate 4 words: PIP, ONE, HEN, and the main word PENT
  assert(formed_words_get_word_length(fw, 0) == 3);
  assert(formed_words_get_word_valid(fw, 0));
  assert(memcmp(formed_words_get_word(fw, 0), (MachineLetter[]){16, 9, 16},
                3) == 0);
  assert(formed_words_get_word_length(fw, 1) == 3);
  assert(formed_words_get_word_valid(fw, 1));
  assert(memcmp(formed_words_get_word(fw, 1), (MachineLetter[]){15, 14, 5},
                3) == 0);
  assert(formed_words_get_word_length(fw, 2) == 3);
  assert(formed_words_get_word_valid(fw, 2));
  assert(memcmp(formed_words_get_word(fw, 2), (MachineLetter[]){8, 5, 14}, 3) ==
         0);
  assert(formed_words_get_word_length(fw, 3) == 4);
  assert(formed_words_get_word_valid(fw, 3));
  assert(memcmp(formed_words_get_word(fw, 3), (MachineLetter[]){16, 5, 14, 20},
                4) == 0);
  validated_moves_destroy(vms_pent);

  // Play some random phoney making a lot of words 6G DI(PET)AZ
  ValidatedMoves *vms_dipetaz = validated_moves_create_and_assert_status(
      game, 0, "6G.DIPETAZ", true, true, false, ERROR_STATUS_SUCCESS);

  fw = validated_moves_get_formed_words(vms_dipetaz, 0);

  assert(formed_words_get_num_words(fw) == 5);
  // should generate 5 "words":
  // OD, WIFAY*, ANE, ZGENUINE*, and the main word DIPETAZ*
  assert(formed_words_get_word_length(fw, 0) == 2);
  assert(formed_words_get_word_valid(fw, 0));
  assert(memcmp(formed_words_get_word(fw, 0), (MachineLetter[]){15, 4}, 2) ==
         0);
  assert(formed_words_get_word_length(fw, 1) == 5);
  assert(!formed_words_get_word_valid(fw, 1));
  assert(memcmp(formed_words_get_word(fw, 1),
                (MachineLetter[]){23, 9, 6, 1, 25}, 5) == 0);
  assert(formed_words_get_word_length(fw, 2) == 3);
  assert(formed_words_get_word_valid(fw, 2));
  assert(memcmp(formed_words_get_word(fw, 2), (MachineLetter[]){1, 14, 5}, 3) ==
         0);
  assert(formed_words_get_word_length(fw, 3) == 8);
  assert(!formed_words_get_word_valid(fw, 3));
  assert(memcmp(formed_words_get_word(fw, 3),
                (MachineLetter[]){26, 7, 5, 14, 21, 9, 14, 5}, 8) == 0);
  assert(formed_words_get_word_length(fw, 4) == 7);
  assert(!formed_words_get_word_valid(fw, 4));
  assert(memcmp(formed_words_get_word(fw, 4),
                (MachineLetter[]){4, 9, 16, 5, 20, 1, 26}, 7) == 0);

  validated_moves_destroy(vms_dipetaz);

  // play a single tile that makes two words. 9F (BOY)S
  ValidatedMoves *vms_boys = validated_moves_create_and_assert_status(
      game, 0, "9F.BOYS", false, true, false, ERROR_STATUS_SUCCESS);

  fw = validated_moves_get_formed_words(vms_boys, 0);

  assert(formed_words_get_num_words(fw) == 2);
  // generates SPAYS and BOYS
  assert(formed_words_get_word_length(fw, 0) == 5);
  assert(formed_words_get_word_valid(fw, 0));
  assert(memcmp(formed_words_get_word(fw, 0),
                (MachineLetter[]){19, 16, 1, 25, 19}, 5) == 0);
  assert(formed_words_get_word_length(fw, 1) == 4);
  assert(formed_words_get_word_valid(fw, 1));
  assert(memcmp(formed_words_get_word(fw, 1), (MachineLetter[]){2, 15, 25, 19},
                4) == 0);

  validated_moves_destroy(vms_boys);

  // same as above but dir - I5 SPAY(S)
  ValidatedMoves *vms_spays = validated_moves_create_and_assert_status(
      game, 0, "I5.SPAYS", false, true, false, ERROR_STATUS_SUCCESS);

  fw = validated_moves_get_formed_words(vms_spays, 0);

  // generates BOYS and SPAYS
  assert(formed_words_get_word_length(fw, 0) == 4);
  assert(formed_words_get_word_valid(fw, 0));
  assert(memcmp(formed_words_get_word(fw, 0), (MachineLetter[]){2, 15, 25, 19},
                4) == 0);
  assert(formed_words_get_word_length(fw, 1) == 5);
  assert(formed_words_get_word_valid(fw, 1));
  assert(memcmp(formed_words_get_word(fw, 1),
                (MachineLetter[]){19, 16, 1, 25, 19}, 5) == 0);

  validated_moves_destroy(vms_spays);

  // N1 ZA making ZE# and AN, testing crosswords at board edge
  ValidatedMoves *vms_za = validated_moves_create_and_assert_status(
      game, 0, "N1.ZA", true, true, false, ERROR_STATUS_SUCCESS);

  fw = validated_moves_get_formed_words(vms_za, 0);
  assert(formed_words_get_num_words(fw) == 3);

  // ZE# (phony because lexicon is NWL)
  assert(formed_words_get_word_length(fw, 0) == 2);
  assert(!formed_words_get_word_valid(fw, 0));
  assert(memcmp(formed_words_get_word(fw, 0), (MachineLetter[]){26, 5}, 2) ==
         0);
  // AN
  assert(formed_words_get_word_length(fw, 1) == 2);
  assert(formed_words_get_word_valid(fw, 1));
  assert(memcmp(formed_words_get_word(fw, 1), (MachineLetter[]){1, 14}, 2) ==
         0);
  // ZA
  assert(formed_words_get_word_length(fw, 2) == 2);
  assert(formed_words_get_word_valid(fw, 2));
  assert(memcmp(formed_words_get_word(fw, 2), (MachineLetter[]){26, 1}, 2) ==
         0);

  validated_moves_destroy(vms_za);

  game_destroy(game);
  config_destroy(config);
}

void test_words(void) { test_words_played(); }