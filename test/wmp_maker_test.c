#include "../src/def/board_defs.h"
#include "../src/def/kwg_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/wmp_defs.h"
#include "../src/ent/bit_rack.h"
#include "../src/ent/dictionary_word.h"
#include "../src/ent/game.h"
#include "../src/ent/kwg.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/player.h"
#include "../src/ent/wmp.h"
#include "../src/impl/config.h"
#include "../src/impl/kwg_maker.h"
#include "../src/impl/wmp_maker.h"
#include "../src/util/io_util.h"
#include "test_util.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

void test_make_wmp_from_kwg(void) {
  Config *config = config_create_or_die("set -lex CSW21");
  const LetterDistribution *ld = config_get_ld(config);
  Game *game = config_game_create(config);
  const Player *player = game_get_player(game, 0);
  const KWG *csw_kwg = player_get_kwg(player);

  // Create WMP directly from KWG (uses DAWG only)
  WMP *wmp = make_wmp_from_kwg(csw_kwg, ld, 0);
  assert(wmp != NULL);
  assert(wmp->version == WMP_VERSION);
  assert(wmp->board_dim == BOARD_DIM);

  // Verify it can look up words
  MachineLetter *buffer = malloc_or_die(wmp->max_word_lookup_bytes);

  // Test basic word lookup
  BitRack aa = string_to_bit_rack(ld, "AA");
  int bytes_written = wmp_write_words_to_buffer(wmp, &aa, 2, buffer);
  assert(bytes_written == 2);
  assert_word_in_buffer(buffer, "AA", ld, 0, 2);

  // Test with blanks
  BitRack a_blank = string_to_bit_rack(ld, "A?");
  bytes_written = wmp_write_words_to_buffer(wmp, &a_blank, 2, buffer);
  // Should find AA, AB, AD, AE, AG, AH, AI, AL, AM, AN, AR, AS, AT, AW, AX, AY,
  // BA, DA, EA, FA, HA, JA, KA, LA, MA, NA, PA, TA, YA, ZA
  assert(bytes_written > 0);
  // AA is one of the possibilities
  bool found_aa = false;
  for (int i = 0; i < bytes_written / 2; i++) {
    const MachineLetter *word = buffer + (size_t)i * 2;
    if (word[0] == ld_hl_to_ml(ld, "A") && word[1] == ld_hl_to_ml(ld, "A")) {
      found_aa = true;
      break;
    }
  }
  assert(found_aa);

  // Compare with WMP created from words to ensure they produce identical
  // results
  DictionaryWordList *words = dictionary_word_list_create();
  kwg_write_words(csw_kwg, kwg_get_dawg_root_node_index(csw_kwg), words, NULL);
  WMP *wmp_from_words = make_wmp_from_words(words, ld, 0);

  // Both WMPs should have the same structure
  assert(wmp->version == wmp_from_words->version);
  assert(wmp->board_dim == wmp_from_words->board_dim);
  assert(wmp->max_word_lookup_bytes == wmp_from_words->max_word_lookup_bytes);

  // Verify same lookup results for a few test cases
  BitRack test_rack = string_to_bit_rack(ld, "RETINAS");
  int bytes1 = wmp_write_words_to_buffer(wmp, &test_rack, 7, buffer);
  MachineLetter *buffer2 = malloc_or_die(wmp_from_words->max_word_lookup_bytes);
  int bytes2 =
      wmp_write_words_to_buffer(wmp_from_words, &test_rack, 7, buffer2);
  assert(bytes1 == bytes2);

  free(buffer2);
  wmp_destroy(wmp_from_words);
  dictionary_word_list_destroy(words);
  free(buffer);
  wmp_destroy(wmp);
  game_destroy(game);
  config_destroy(config);
}

void test_make_wmp_from_dawg_only_kwg(void) {
  Config *config = config_create_or_die("set -lex CSW21");
  const LetterDistribution *ld = config_get_ld(config);

  // Create a small word list for testing
  DictionaryWordList *words = dictionary_word_list_create();
  const MachineLetter word_aa[] = {ld_hl_to_ml(ld, "A"), ld_hl_to_ml(ld, "A")};
  const MachineLetter word_ab[] = {ld_hl_to_ml(ld, "A"), ld_hl_to_ml(ld, "B")};
  const MachineLetter word_ba[] = {ld_hl_to_ml(ld, "B"), ld_hl_to_ml(ld, "A")};
  const MachineLetter word_cat[] = {ld_hl_to_ml(ld, "C"), ld_hl_to_ml(ld, "A"),
                                    ld_hl_to_ml(ld, "T")};
  const MachineLetter word_act[] = {ld_hl_to_ml(ld, "A"), ld_hl_to_ml(ld, "C"),
                                    ld_hl_to_ml(ld, "T")};
  dictionary_word_list_add_word(words, word_aa, 2);
  dictionary_word_list_add_word(words, word_ab, 2);
  dictionary_word_list_add_word(words, word_ba, 2);
  dictionary_word_list_add_word(words, word_cat, 3);
  dictionary_word_list_add_word(words, word_act, 3);

  // Create a DAWG-only KWG (no GADDAG)
  KWG *dawg_only_kwg =
      make_kwg_from_words(words, KWG_MAKER_OUTPUT_DAWG, KWG_MAKER_MERGE_EXACT);

  // Create WMP from the DAWG-only KWG
  WMP *wmp = make_wmp_from_kwg(dawg_only_kwg, ld, 0);
  assert(wmp != NULL);
  assert(wmp->version == WMP_VERSION);
  assert(wmp->board_dim == BOARD_DIM);

  MachineLetter *buffer = malloc_or_die(wmp->max_word_lookup_bytes);

  // Test basic word lookup - AA
  BitRack aa = string_to_bit_rack(ld, "AA");
  int bytes_written = wmp_write_words_to_buffer(wmp, &aa, 2, buffer);
  assert(bytes_written == 2);
  assert_word_in_buffer(buffer, "AA", ld, 0, 2);

  // Test word lookup - AB (should find AB)
  BitRack ab = string_to_bit_rack(ld, "AB");
  bytes_written = wmp_write_words_to_buffer(wmp, &ab, 2, buffer);
  assert(bytes_written == 4); // AB and BA are anagrams
  // Both AB and BA should be found
  bool found_ab = false;
  bool found_ba = false;
  for (int i = 0; i < 2; i++) {
    const MachineLetter *word = buffer + (size_t)i * 2;
    if (word[0] == ld_hl_to_ml(ld, "A") && word[1] == ld_hl_to_ml(ld, "B")) {
      found_ab = true;
    }
    if (word[0] == ld_hl_to_ml(ld, "B") && word[1] == ld_hl_to_ml(ld, "A")) {
      found_ba = true;
    }
  }
  assert(found_ab);
  assert(found_ba);

  // Test word lookup - CAT/ACT (anagrams)
  BitRack act = string_to_bit_rack(ld, "ACT");
  bytes_written = wmp_write_words_to_buffer(wmp, &act, 3, buffer);
  assert(bytes_written == 6); // CAT and ACT
  bool found_cat = false;
  bool found_act = false;
  for (int i = 0; i < 2; i++) {
    const MachineLetter *word = buffer + (size_t)i * 3;
    if (word[0] == ld_hl_to_ml(ld, "C") && word[1] == ld_hl_to_ml(ld, "A") &&
        word[2] == ld_hl_to_ml(ld, "T")) {
      found_cat = true;
    }
    if (word[0] == ld_hl_to_ml(ld, "A") && word[1] == ld_hl_to_ml(ld, "C") &&
        word[2] == ld_hl_to_ml(ld, "T")) {
      found_act = true;
    }
  }
  assert(found_cat);
  assert(found_act);

  // Test with blank - A? should find AA, AB, BA
  BitRack a_blank = string_to_bit_rack(ld, "A?");
  bytes_written = wmp_write_words_to_buffer(wmp, &a_blank, 2, buffer);
  assert(bytes_written == 6); // AA, AB, BA

  free(buffer);
  wmp_destroy(wmp);
  kwg_destroy(dawg_only_kwg);
  dictionary_word_list_destroy(words);
  config_destroy(config);
}

void test_make_wmp_from_words(void) {
  Config *config = config_create_or_die("set -lex CSW21");
  const LetterDistribution *ld = config_get_ld(config);
  Game *game = config_game_create(config);
  const Player *player = game_get_player(game, 0);
  const KWG *csw_kwg = player_get_kwg(player);
  DictionaryWordList *words = dictionary_word_list_create();
  kwg_write_words(csw_kwg, kwg_get_dawg_root_node_index(csw_kwg), words, NULL);

  DictionaryWordList *q_words_2to8 = dictionary_word_list_create();
  const MachineLetter q = ld_hl_to_ml(ld, "Q");
  for (int word_idx = 0; word_idx < dictionary_word_list_get_count(words);
       word_idx++) {
    const DictionaryWord *word = dictionary_word_list_get_word(words, word_idx);
    const uint8_t length = dictionary_word_get_length(word);
    if (length > 8) {
      continue;
    }
    for (int letter_idx = 0; letter_idx < length; letter_idx++) {
      if (dictionary_word_get_word(word)[letter_idx] == q) {
        dictionary_word_list_add_word(q_words_2to8,
                                      dictionary_word_get_word(word), length);
        break;
      }
    }
  }
  dictionary_word_list_destroy(words);

  WMP *wmp = make_wmp_from_words(q_words_2to8, ld, 0);
  assert(wmp != NULL);
  assert(wmp->version == WMP_VERSION);
  assert(wmp->board_dim == BOARD_DIM);
  assert(wmp->max_word_lookup_bytes == 8 * 47); // EIQSTU??

  MachineLetter *buffer = malloc_or_die(wmp->max_word_lookup_bytes);

  BitRack iq = string_to_bit_rack(ld, "IQ");
  int bytes_written = wmp_write_words_to_buffer(wmp, &iq, 2, buffer);
  assert(bytes_written == 2);
  assert_word_in_buffer(buffer, "QI", ld, 0, 2);

  BitRack q_blank = string_to_bit_rack(ld, "Q?");
  bytes_written = wmp_write_words_to_buffer(wmp, &q_blank, 2, buffer);
  assert(bytes_written == 2);
  assert_word_in_buffer(buffer, "QI", ld, 0, 2);

  BitRack square_blank = string_to_bit_rack(ld, "SQUARE?");
  bytes_written = wmp_write_words_to_buffer(wmp, &square_blank, 7, buffer);
  assert(bytes_written == 7 * 13);
  assert_word_in_buffer(buffer, "BARQUES", ld, 0, 7);
  assert_word_in_buffer(buffer, "SQUARED", ld, 1, 7);
  assert_word_in_buffer(buffer, "QUAERES", ld, 2, 7);
  assert_word_in_buffer(buffer, "QUASHER", ld, 3, 7);
  assert_word_in_buffer(buffer, "QUAKERS", ld, 4, 7);
  assert_word_in_buffer(buffer, "MARQUES", ld, 5, 7);
  assert_word_in_buffer(buffer, "MASQUER", ld, 6, 7);
  assert_word_in_buffer(buffer, "SQUARER", ld, 7, 7);
  assert_word_in_buffer(buffer, "SQUARES", ld, 8, 7);
  assert_word_in_buffer(buffer, "QUAREST", ld, 9, 7);
  assert_word_in_buffer(buffer, "QUARTES", ld, 10, 7);
  assert_word_in_buffer(buffer, "QUATRES", ld, 11, 7);
  assert_word_in_buffer(buffer, "QUAVERS", ld, 12, 7);

  BitRack quebracho = string_to_bit_rack(ld, "QUEBRACHO");
  bytes_written = wmp_write_words_to_buffer(wmp, &quebracho, 9, buffer);
  assert(bytes_written == 0);

  BitRack quu_blank_blank = string_to_bit_rack(ld, "QUU??");
  bytes_written = wmp_write_words_to_buffer(wmp, &quu_blank_blank, 5, buffer);
  assert(bytes_written == 5 * 4);
  assert_word_in_buffer(buffer, "QUEUE", ld, 0, 5);
  assert_word_in_buffer(buffer, "USQUE", ld, 1, 5);
  assert_word_in_buffer(buffer, "TUQUE", ld, 2, 5);
  assert_word_in_buffer(buffer, "QUIPU", ld, 3, 5);

  free(buffer);
  wmp_destroy(wmp);
  dictionary_word_list_destroy(q_words_2to8);
  game_destroy(game);
  config_destroy(config);
}

void test_wmp_maker(void) {
  test_make_wmp_from_kwg();
  test_make_wmp_from_dawg_only_kwg();
  test_make_wmp_from_words();
}
