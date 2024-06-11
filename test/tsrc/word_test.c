#include <assert.h>
#include <stdint.h>

#include "../../src/def/validated_move_defs.h"

#include "../../src/ent/board.h"
#include "../../src/ent/game.h"
#include "../../src/ent/kwg.h"
#include "../../src/ent/player.h"
#include "../../src/ent/validated_move.h"
#include "../../src/ent/words.h"
#include "../../src/impl/config.h"

#include "../../src/util/string_util.h"

#include "../../src/impl/cgp.h"

#include "test_constants.h"
#include "test_util.h"

void test_words_played() {
  Config *config = create_config_or_die(
      "lex NWL20 s1 score s2 score r1 all r2 all numplays 1");
  Game *game = config_game_create(config);
  game_load_cgp(game, VS_ED);

  ValidatedMoves *vms_pent =
      validated_moves_create(game, 0, "N11.PeNT", false, true, false);

  assert(validated_moves_get_validation_status(vms_pent) ==
         MOVE_VALIDATION_STATUS_SUCCESS);

  const FormedWords *fw = validated_moves_get_formed_words(vms_pent, 0);

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
  validated_moves_destroy(vms_pent);

  // Play some random phoney making a lot of words 6G DI(PET)AZ
  ValidatedMoves *vms_dipetaz =
      validated_moves_create(game, 0, "6G.DIPETAZ", true, true, false);

  assert(validated_moves_get_validation_status(vms_dipetaz) ==
         MOVE_VALIDATION_STATUS_SUCCESS);

  fw = validated_moves_get_formed_words(vms_dipetaz, 0);

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

  validated_moves_destroy(vms_dipetaz);

  // play a single tile that makes two words. 9F (BOY)S
  ValidatedMoves *vms_boys =
      validated_moves_create(game, 0, "9F.BOYS", false, true, false);

  assert(validated_moves_get_validation_status(vms_boys) ==
         MOVE_VALIDATION_STATUS_SUCCESS);

  fw = validated_moves_get_formed_words(vms_boys, 0);

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

  validated_moves_destroy(vms_boys);

  // same as above but dir - I5 SPAY(S)
  ValidatedMoves *vms_spays =
      validated_moves_create(game, 0, "I5.SPAYS", false, true, false);

  assert(validated_moves_get_validation_status(vms_spays) ==
         MOVE_VALIDATION_STATUS_SUCCESS);

  fw = validated_moves_get_formed_words(vms_spays, 0);

  // generates BOYS and SPAYS
  assert(formed_words_get_word_length(fw, 0) == 4);
  assert(formed_words_get_word_valid(fw, 0) == 1);
  assert(memory_compare(formed_words_get_word(fw, 0),
                        (uint8_t[]){2, 15, 25, 19}, 4) == 0);
  assert(formed_words_get_word_length(fw, 1) == 5);
  assert(formed_words_get_word_valid(fw, 1) == 1);
  assert(memory_compare(formed_words_get_word(fw, 1),
                        (uint8_t[]){19, 16, 1, 25, 19}, 5) == 0);

  validated_moves_destroy(vms_spays);

  game_destroy(game);
  config_destroy(config);
}

void test_words() { test_words_played(); }