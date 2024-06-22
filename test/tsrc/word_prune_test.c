#include <assert.h>

#include "../../src/def/board_defs.h"
#include "../../src/def/letter_distribution_defs.h"
#include "../../src/ent/dictionary_word.h"
#include "../../src/ent/game.h"
#include "../../src/ent/player.h"
#include "../../src/impl/cgp.h"
#include "../../src/impl/config.h"
#include "../../src/impl/word_prune.h"
#include "../../src/util/string_util.h"
#include "../../src/util/util.h"

#include "test_util.h"

void assert_word_count(const LetterDistribution *ld, DictionaryWordList *words,
                       const char *human_readable_word, int expected_count) {
  int expected_length = string_length(human_readable_word);
  uint8_t expected[BOARD_DIM];
  ld_str_to_mls(ld, human_readable_word, false, expected, expected_length);
  int count = 0;
  for (int i = 0; i < dictionary_word_list_get_count(words); i++) {
    DictionaryWord *word = dictionary_word_list_get_word(words, i);
    if ((dictionary_word_get_length(word) == expected_length) &&
        (memory_compare(dictionary_word_get_word(word), expected,
                        expected_length) == 0)) {
      count++;
    }
  }
  assert(count == expected_count);
}

void test_possible_words() {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  // empty board
  Game *game = config_game_create(config);
  DictionaryWordList *possible_word_list = dictionary_word_list_create();
  generate_possible_words(game, NULL, possible_word_list);

  // all words except unplayable (PIZZAZZ, etc.)
  // 24 of the 279077 words in CSW21 are not playable using a standard English
  // tile set. 17 have >3 Z's, 2 have >3 K's, 5 have >6 S's.
  assert(dictionary_word_list_get_count(possible_word_list) == 279053);
  char zonule[300] = "ZONULE1B2APAID/1KY2RHANJA4/GAM4R2HUI2/7G6D/6FECIT3O/"
                     "6AE1TOWIES/6I7E/1EnGUARD6D/NAOI2W8/6AT7/5PYE7/5L1L7/"
                     "2COVE1L7/5X1E7/7N7 MOOORRT/BFQRTTV 340/419 0 lex CSW21;";
  game_load_cgp(game, zonule);

  dictionary_word_list_clear(possible_word_list);
  generate_possible_words(game, NULL, possible_word_list);

  assert(dictionary_word_list_get_count(possible_word_list) == 62702);
  assert_word_count(game_get_ld(game), possible_word_list, "ENGUARDING", 1);
  assert_word_count(game_get_ld(game), possible_word_list, "NONFACT", 1);
  assert_word_count(game_get_ld(game), possible_word_list, "ZOOMED", 1);
  assert_word_count(game_get_ld(game), possible_word_list, "SIGISBEO", 1);

  char taurine[300] =
      "15/3Q7U3/3U2TAURINE2/1CHANSONS2W3/2AI6JO3/DIRL1PO3IN3/E1D2EF3V4/"
      "F1I2p1TRAIK3/O1L2T4E4/ABy1PIT2BRIG2/ME1MOZELLE5/1GRADE1O1NOH3/WE3R1V7/"
      "AT5E7/G6D7 ENOSTXY/ACEISUY 356/378 0 lex NWL20;";

  game_load_cgp(game, taurine);
  dictionary_word_list_clear(possible_word_list);
  generate_possible_words(game, NULL, possible_word_list);

  assert(dictionary_word_list_get_count(possible_word_list) == 6396);
  assert_word_count(game_get_ld(game), possible_word_list, "CLOVE", 0);
  assert_word_count(game_get_ld(game), possible_word_list, "SLOVE", 0);

  char anoretic[300] =
      "AnORETIC7/7R7/2C1EMEU7/1JANNY1X7/2P12/1SIDELING6/2ZAG7Q2/3MOVED3AA2/"
      "6FEAL1IT2/2NEGATES2D3/3DOH2KEEFS2/WITH7UN2/I10LO2/LABOUR6B2/Y6POTTOS2 "
      "?AENORW/EIIIRUV 332/384 0 lex NWL20;";

  game_load_cgp(game, anoretic);
  dictionary_word_list_clear(possible_word_list);
  generate_possible_words(game, NULL, possible_word_list);

  assert(dictionary_word_list_get_count(possible_word_list) == 11161);
  assert_word_count(game_get_ld(game), possible_word_list, "AALII", 1);
  assert_word_count(game_get_ld(game), possible_word_list, "REVERBERANT", 1);
  assert_word_count(game_get_ld(game), possible_word_list, "ZIPWIRE", 1);
  dictionary_word_list_destroy(possible_word_list);
  game_destroy(game);
  config_destroy(config);
}

void test_word_prune() { test_possible_words(); }