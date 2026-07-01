#include "../src/compat/ctime.h"
#include "../src/def/kwg_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/ent/dictionary_word.h"
#include "../src/ent/game.h"
#include "../src/ent/kwg.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/player.h"
#include "../src/impl/config.h"
#include "../src/impl/kwg_maker.h"
#include "../src/impl/word_prune.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void add_test_word(const LetterDistribution *ld, DictionaryWordList *words,
                   const char *human_readable_word) {
  int length = (int)string_length(human_readable_word);
  MachineLetter word[MAX_KWG_STRING_LENGTH];
  ld_str_to_mls(ld, human_readable_word, false, word, length);
  dictionary_word_list_add_word(words, word, length);
}

void assert_word_lists_are_equal(const DictionaryWordList *expected,
                                 const DictionaryWordList *actual) {
  assert(dictionary_word_list_get_count(expected) ==
         dictionary_word_list_get_count(actual));
  for (int i = 0; i < dictionary_word_list_get_count(expected); i++) {
    const DictionaryWord *expected_word =
        dictionary_word_list_get_word(expected, i);
    const DictionaryWord *actual_word =
        dictionary_word_list_get_word(actual, i);
    assert(dictionary_word_get_length(expected_word) ==
           dictionary_word_get_length(actual_word));
    assert(memcmp(dictionary_word_get_word(expected_word),
                  dictionary_word_get_word(actual_word),
                  dictionary_word_get_length(expected_word)) == 0);
  }
}

uint32_t kwg_prefix_arc_aux(const KWG *kwg, uint32_t node_index,
                            const DictionaryWord *prefix, int pos) {
  const MachineLetter ml = dictionary_word_get_word(prefix)[pos];
  const uint32_t next_node_index = kwg_get_next_node_index(kwg, node_index, ml);
  if (pos + 1 == dictionary_word_get_length(prefix)) {
    return next_node_index;
  }
  if (next_node_index == 0) {
    return 0;
  }
  return kwg_prefix_arc_aux(kwg, next_node_index, prefix, pos + 1);
}

uint32_t kwg_dawg_prefix_arc(const KWG *kwg, const LetterDistribution *ld,
                             const char *human_readable_prefix) {
  // Make a list with one word in it just because I don't have a handy function
  // to create a DictionaryWord by itself.
  DictionaryWordList *prefix_list = dictionary_word_list_create();
  uint8_t prefix_bytes[MAX_KWG_STRING_LENGTH];
  ld_str_to_mls(ld, human_readable_prefix, false, prefix_bytes,
                string_length(human_readable_prefix));
  dictionary_word_list_add_word(prefix_list, prefix_bytes,
                                (int)string_length(human_readable_prefix));
  const DictionaryWord *prefix = dictionary_word_list_get_word(prefix_list, 0);
  const uint32_t arc =
      kwg_prefix_arc_aux(kwg, kwg_get_dawg_root_node_index(kwg), prefix, 0);
  dictionary_word_list_destroy(prefix_list);
  return arc;
}

uint32_t kwg_gaddag_prefix_arc(const KWG *kwg, const LetterDistribution *ld,
                               const char *human_readable_prefix) {
  char string_for_conversion[MAX_KWG_STRING_LENGTH];
  strncpy(string_for_conversion, human_readable_prefix,
          sizeof(string_for_conversion));
  for (size_t i = 0; i < string_length(human_readable_prefix); i++) {
    if (string_for_conversion[i] == '@') {
      string_for_conversion[i] = '?';
    }
  }
  // Make a list with one word in it just because I don't have a handy function
  // to create a DictionaryWord by itself.
  DictionaryWordList *prefix_list = dictionary_word_list_create();
  uint8_t prefix_bytes[MAX_KWG_STRING_LENGTH];
  ld_str_to_mls(ld, string_for_conversion, false, prefix_bytes,
                string_length(string_for_conversion));
  dictionary_word_list_add_word(prefix_list, prefix_bytes,
                                (int)string_length(string_for_conversion));
  const DictionaryWord *prefix = dictionary_word_list_get_word(prefix_list, 0);
  const uint32_t arc =
      kwg_prefix_arc_aux(kwg, kwg_get_root_node_index(kwg), prefix, 0);
  dictionary_word_list_destroy(prefix_list);
  return arc;
}

void test_qi_xi_xu_word_trie(void) {
  Config *config = config_create_or_die("set -lex CSW21");
  const LetterDistribution *ld = config_get_ld(config);
  DictionaryWordList *words = dictionary_word_list_create();
  add_test_word(ld, words, "QI");
  add_test_word(ld, words, "XI");
  add_test_word(ld, words, "XU");

  KWG *kwg =
      make_kwg_from_words(words, KWG_MAKER_OUTPUT_DAWG, KWG_MAKER_MERGE_NONE);

  // Q and X prefixes are not empty
  assert(kwg_dawg_prefix_arc(kwg, ld, "Q") != 0);
  assert(kwg_dawg_prefix_arc(kwg, ld, "X") != 0);
  // The full words are empty, none of them can be extended
  assert(kwg_dawg_prefix_arc(kwg, ld, "QI") == 0);
  assert(kwg_dawg_prefix_arc(kwg, ld, "XI") == 0);
  assert(kwg_dawg_prefix_arc(kwg, ld, "XU") == 0);

  // Q and X prefixes are not merged, Z is not found and thus returning empty
  assert(kwg_dawg_prefix_arc(kwg, ld, "Q") !=
         kwg_dawg_prefix_arc(kwg, ld, "X"));
  assert(kwg_dawg_prefix_arc(kwg, ld, "Z") == 0);

  DictionaryWordList *encoded_words = dictionary_word_list_create();
  kwg_write_words(kwg, kwg_get_dawg_root_node_index(kwg), encoded_words, NULL);

  assert_word_lists_are_equal(words, encoded_words);

  dictionary_word_list_destroy(encoded_words);
  dictionary_word_list_destroy(words);
  kwg_destroy(kwg);
  config_destroy(config);
}

void test_egg_unmerged_gaddag(void) {
  Config *config = config_create_or_die("set -lex CSW21");
  const LetterDistribution *ld = config_get_ld(config);
  DictionaryWordList *words = dictionary_word_list_create();
  add_test_word(ld, words, "EGG");

  KWG *kwg =
      make_kwg_from_words(words, KWG_MAKER_OUTPUT_GADDAG, KWG_MAKER_MERGE_NONE);

  assert(kwg_gaddag_prefix_arc(kwg, ld, "G") != 0);
  assert(kwg_gaddag_prefix_arc(kwg, ld, "GG") != 0);

  dictionary_word_list_destroy(words);
  kwg_destroy(kwg);
  config_destroy(config);
}

void test_careen_career_unmerged_gaddag(void) {
  Config *config = config_create_or_die("set -lex CSW21");
  const LetterDistribution *ld = config_get_ld(config);
  DictionaryWordList *words = dictionary_word_list_create();
  add_test_word(ld, words, "CAREEN");
  add_test_word(ld, words, "CAREER");

  KWG *kwg =
      make_kwg_from_words(words, KWG_MAKER_OUTPUT_GADDAG, KWG_MAKER_MERGE_NONE);

  // Reversed words are gaddag entries. Check that the last letters of the words
  // are nonempty prefixes. However, they are not merged.
  assert(kwg_gaddag_prefix_arc(kwg, ld, "N") != 0);
  assert(kwg_gaddag_prefix_arc(kwg, ld, "R") != 0);
  assert(kwg_gaddag_prefix_arc(kwg, ld, "N") !=
         kwg_gaddag_prefix_arc(kwg, ld, "R"));

  // Some other checks
  assert(kwg_gaddag_prefix_arc(kwg, ld, "NEERA") != 0);
  assert(kwg_gaddag_prefix_arc(kwg, ld, "C@AREE") != 0);
  assert(kwg_gaddag_prefix_arc(kwg, ld, "AC@RE") != 0);
  assert(kwg_gaddag_prefix_arc(kwg, ld, "RAC@E") != 0);

  dictionary_word_list_destroy(words);
  kwg_destroy(kwg);
  config_destroy(config);
}

void test_careen_career_exact_merged_gaddag(void) {
  Config *config = config_create_or_die("set -lex CSW21");
  const LetterDistribution *ld = config_get_ld(config);
  DictionaryWordList *words = dictionary_word_list_create();
  add_test_word(ld, words, "CAREEN");
  add_test_word(ld, words, "CAREER");

  KWG *kwg = make_kwg_from_words(words, KWG_MAKER_OUTPUT_GADDAG,
                                 KWG_MAKER_MERGE_EXACT);

  assert(kwg_gaddag_prefix_arc(kwg, ld, "N") != 0);
  assert(kwg_gaddag_prefix_arc(kwg, ld, "R") != 0);
  // N does not exactly equal R because R also has suffixes starting from the R
  // in the middle of the word.
  assert(kwg_gaddag_prefix_arc(kwg, ld, "N") !=
         kwg_gaddag_prefix_arc(kwg, ld, "R"));

  // But all of these merge to the same node.
  assert(kwg_gaddag_prefix_arc(kwg, ld, "C@AREE") != 0);
  assert(kwg_gaddag_prefix_arc(kwg, ld, "AC@REE") ==
         kwg_gaddag_prefix_arc(kwg, ld, "C@AREE"));
  assert(kwg_gaddag_prefix_arc(kwg, ld, "RAC@EE") ==
         kwg_gaddag_prefix_arc(kwg, ld, "C@AREE"));
  assert(kwg_gaddag_prefix_arc(kwg, ld, "ERAC@E") ==
         kwg_gaddag_prefix_arc(kwg, ld, "C@AREE"));
  assert(kwg_gaddag_prefix_arc(kwg, ld, "EERAC@") ==
         kwg_gaddag_prefix_arc(kwg, ld, "C@AREE"));

  dictionary_word_list_destroy(words);
  kwg_destroy(kwg);
  config_destroy(config);
}

void test_two_letter_trie(void) {
  Config *config = config_create_or_die("set -lex CSW21");
  const LetterDistribution *ld = config_get_ld(config);
  Game *game = config_game_create(config);
  const Player *player = game_get_player(game, 0);
  const KWG *csw_kwg = player_get_kwg(player);
  DictionaryWordList *words = dictionary_word_list_create();
  kwg_write_words(csw_kwg, kwg_get_dawg_root_node_index(csw_kwg), words, NULL);

  DictionaryWordList *two_letter_words = dictionary_word_list_create();
  for (int i = 0; i < dictionary_word_list_get_count(words); i++) {
    const DictionaryWord *word = dictionary_word_list_get_word(words, i);
    if (dictionary_word_get_length(word) == 2) {
      dictionary_word_list_add_word(two_letter_words,
                                    dictionary_word_get_word(word),
                                    dictionary_word_get_length(word));
    }
  }
  dictionary_word_list_destroy(words);

  KWG *kwg = make_kwg_from_words(two_letter_words, KWG_MAKER_OUTPUT_DAWG,
                                 KWG_MAKER_MERGE_NONE);

  // not merged: DA/DE/DI/DO, PA/PE/PI/PO, TA/TE/TI/TO
  assert(kwg_dawg_prefix_arc(kwg, ld, "D") !=
         kwg_dawg_prefix_arc(kwg, ld, "P"));
  assert(kwg_dawg_prefix_arc(kwg, ld, "D") !=
         kwg_dawg_prefix_arc(kwg, ld, "T"));
  assert(kwg_dawg_prefix_arc(kwg, ld, "P") !=
         kwg_dawg_prefix_arc(kwg, ld, "T"));

  DictionaryWordList *encoded_words = dictionary_word_list_create();
  kwg_write_words(kwg, kwg_get_dawg_root_node_index(kwg), encoded_words, NULL);
  assert_word_lists_are_equal(two_letter_words, encoded_words);
  dictionary_word_list_destroy(encoded_words);
  dictionary_word_list_destroy(two_letter_words);

  game_destroy(game);
  kwg_destroy(kwg);
  config_destroy(config);
}

void test_two_letter_merged_dawg(void) {
  Config *config = config_create_or_die("set -lex CSW21");
  const LetterDistribution *ld = config_get_ld(config);
  Game *game = config_game_create(config);
  const Player *player = game_get_player(game, 0);
  const KWG *csw_kwg = player_get_kwg(player);
  DictionaryWordList *words = dictionary_word_list_create();
  kwg_write_words(csw_kwg, kwg_get_dawg_root_node_index(csw_kwg), words, NULL);

  DictionaryWordList *two_letter_words = dictionary_word_list_create();
  for (int i = 0; i < dictionary_word_list_get_count(words); i++) {
    const DictionaryWord *word = dictionary_word_list_get_word(words, i);
    if (dictionary_word_get_length(word) == 2) {
      dictionary_word_list_add_word(two_letter_words,
                                    dictionary_word_get_word(word),
                                    dictionary_word_get_length(word));
    }
  }
  dictionary_word_list_destroy(words);

  KWG *kwg = make_kwg_from_words(two_letter_words, KWG_MAKER_OUTPUT_DAWG,
                                 KWG_MAKER_MERGE_EXACT);

  // merged: DA/DE/DI/DO, PA/PE/PI/PO, TA/TE/TI/TO
  assert(kwg_dawg_prefix_arc(kwg, ld, "D") ==
         kwg_dawg_prefix_arc(kwg, ld, "P"));
  assert(kwg_dawg_prefix_arc(kwg, ld, "D") ==
         kwg_dawg_prefix_arc(kwg, ld, "T"));

  DictionaryWordList *encoded_words = dictionary_word_list_create();
  kwg_write_words(kwg, kwg_get_dawg_root_node_index(kwg), encoded_words, NULL);
  assert_word_lists_are_equal(two_letter_words, encoded_words);
  dictionary_word_list_destroy(encoded_words);
  dictionary_word_list_destroy(two_letter_words);

  game_destroy(game);
  kwg_destroy(kwg);
  config_destroy(config);
}

void test_polish_gaddag(void) {
  Config *config = config_create_or_die("set -lex OSPS49 -wmp false");
  Game *game = config_game_create(config);
  const LetterDistribution *ld = config_get_ld(config);
  const Player *player = game_get_player(game, 0);
  const KWG *csw_kwg = player_get_kwg(player);
  DictionaryWordList *words = dictionary_word_list_create();
  kwg_write_words(csw_kwg, kwg_get_dawg_root_node_index(csw_kwg), words, NULL);

  DictionaryWordList *ziet_words = dictionary_word_list_create();
  const MachineLetter ziet = ld_hl_to_ml(ld, "Ź");
  for (int i = 0; i < dictionary_word_list_get_count(words); i++) {
    const DictionaryWord *word = dictionary_word_list_get_word(words, i);
    const uint8_t length = dictionary_word_get_length(word);
    for (int j = 0; j < length; j++) {
      if (dictionary_word_get_word(word)[j] == ziet) {
        dictionary_word_list_add_word(ziet_words,
                                      dictionary_word_get_word(word), length);
        break;
      }
    }
  }
  dictionary_word_list_destroy(words);
  DictionaryWordList *ziet_gaddag_strings = dictionary_word_list_create();
  add_gaddag_strings(ziet_words, ziet_gaddag_strings);

  KWG *kwg = make_kwg_from_words(ziet_words, KWG_MAKER_OUTPUT_DAWG_AND_GADDAG,
                                 KWG_MAKER_MERGE_EXACT);

  DictionaryWordList *encoded_words = dictionary_word_list_create();
  kwg_write_words(kwg, kwg_get_dawg_root_node_index(kwg), encoded_words, NULL);
  assert_word_lists_are_equal(ziet_words, encoded_words);
  dictionary_word_list_destroy(encoded_words);
  dictionary_word_list_destroy(ziet_words);

  DictionaryWordList *encoded_gaddag_strings = dictionary_word_list_create();
  kwg_write_gaddag_strings(kwg, kwg_get_root_node_index(kwg),
                           encoded_gaddag_strings, NULL);
  assert_word_lists_are_equal(ziet_gaddag_strings, encoded_gaddag_strings);
  dictionary_word_list_destroy(encoded_gaddag_strings);
  dictionary_word_list_destroy(ziet_gaddag_strings);

  game_destroy(game);
  kwg_destroy(kwg);
  config_destroy(config);
}

void test_large_gaddag(void) {
  Config *config = config_create_or_die("set -lex CSW21");
  Game *game = config_game_create(config);
  const LetterDistribution *ld = config_get_ld(config);
  const Player *player = game_get_player(game, 0);
  const KWG *csw_kwg = player_get_kwg(player);
  DictionaryWordList *words = dictionary_word_list_create();
  kwg_write_words(csw_kwg, kwg_get_dawg_root_node_index(csw_kwg), words, NULL);

  DictionaryWordList *q_words = dictionary_word_list_create();
  const MachineLetter q = ld_hl_to_ml(ld, "Q");
  for (int i = 0; i < dictionary_word_list_get_count(words); i++) {
    const DictionaryWord *word = dictionary_word_list_get_word(words, i);
    const uint8_t length = dictionary_word_get_length(word);
    for (int j = 0; j < length; j++) {
      if (dictionary_word_get_word(word)[j] == q) {
        dictionary_word_list_add_word(q_words, dictionary_word_get_word(word),
                                      length);
        break;
      }
    }
  }
  dictionary_word_list_destroy(words);
  DictionaryWordList *q_gaddag_strings = dictionary_word_list_create();
  add_gaddag_strings(q_words, q_gaddag_strings);

  KWG *kwg = make_kwg_from_words(q_words, KWG_MAKER_OUTPUT_DAWG_AND_GADDAG,
                                 KWG_MAKER_MERGE_EXACT);

  DictionaryWordList *encoded_words = dictionary_word_list_create();
  kwg_write_words(kwg, kwg_get_dawg_root_node_index(kwg), encoded_words, NULL);
  assert_word_lists_are_equal(q_words, encoded_words);
  dictionary_word_list_destroy(encoded_words);
  dictionary_word_list_destroy(q_words);

  DictionaryWordList *encoded_gaddag_strings = dictionary_word_list_create();
  kwg_write_gaddag_strings(kwg, kwg_get_root_node_index(kwg),
                           encoded_gaddag_strings, NULL);
  assert_word_lists_are_equal(q_gaddag_strings, encoded_gaddag_strings);
  dictionary_word_list_destroy(encoded_gaddag_strings);
  dictionary_word_list_destroy(q_gaddag_strings);

  game_destroy(game);
  kwg_destroy(kwg);
  config_destroy(config);
}

void test_full_csw_gaddag(void) {
  Config *config = config_create_or_die("set -lex CSW21");
  Game *game = config_game_create(config);
  const Player *player = game_get_player(game, 0);
  const KWG *csw_kwg = player_get_kwg(player);
  DictionaryWordList *words = dictionary_word_list_create();
  kwg_write_words(csw_kwg, kwg_get_dawg_root_node_index(csw_kwg), words, NULL);

  DictionaryWordList *gaddag_strings = dictionary_word_list_create();
  add_gaddag_strings(words, gaddag_strings);

  KWG *kwg = make_kwg_from_words(words, KWG_MAKER_OUTPUT_DAWG_AND_GADDAG,
                                 KWG_MAKER_MERGE_EXACT);

  bool *nodes_reached =
      calloc_or_die(kwg_get_number_of_nodes(kwg), sizeof(bool));
  DictionaryWordList *encoded_words = dictionary_word_list_create();
  kwg_write_words(kwg, kwg_get_dawg_root_node_index(kwg), encoded_words,
                  nodes_reached);
  assert_word_lists_are_equal(words, encoded_words);
  dictionary_word_list_destroy(encoded_words);
  dictionary_word_list_destroy(words);

  DictionaryWordList *encoded_gaddag_strings = dictionary_word_list_create();
  kwg_write_gaddag_strings(kwg, kwg_get_root_node_index(kwg),
                           encoded_gaddag_strings, nodes_reached);
  assert_word_lists_are_equal(gaddag_strings, encoded_gaddag_strings);
  dictionary_word_list_destroy(encoded_gaddag_strings);
  dictionary_word_list_destroy(gaddag_strings);

  // first two nodes point to the dawg root and gaddag
  for (int i = 2; i < kwg_get_number_of_nodes(kwg); i++) {
    assert(nodes_reached[i]);
  }
  free(nodes_reached);

  game_destroy(game);
  kwg_destroy(kwg);
  config_destroy(config);
}

// Builds CSW21 with both KWG_MAKER_MERGE_EXACT and KWG_MAKER_MERGE_TAIL,
// confirms the tail-merged graph round-trips the identical word set and
// GADDAG strings, and reports the node-count savings.
void test_kwg_tail_merge(void) {
  Config *config = config_create_or_die("set -lex CSW21");
  Game *game = config_game_create(config);
  const Player *player = game_get_player(game, 0);
  const KWG *csw_kwg = player_get_kwg(player);
  DictionaryWordList *words = dictionary_word_list_create();
  kwg_write_words(csw_kwg, kwg_get_dawg_root_node_index(csw_kwg), words, NULL);

  DictionaryWordList *gaddag_strings = dictionary_word_list_create();
  add_gaddag_strings(words, gaddag_strings);

  KWG *exact_kwg = make_kwg_from_words(words, KWG_MAKER_OUTPUT_DAWG_AND_GADDAG,
                                       KWG_MAKER_MERGE_EXACT);
  KWG *tail_kwg = make_kwg_from_words(words, KWG_MAKER_OUTPUT_DAWG_AND_GADDAG,
                                      KWG_MAKER_MERGE_TAIL);

  const int exact_nodes = kwg_get_number_of_nodes(exact_kwg);
  const int tail_nodes = kwg_get_number_of_nodes(tail_kwg);
  printf("kwg tail merge: exact=%d nodes, tail=%d nodes (saved %d, %.2f%%)\n",
         exact_nodes, tail_nodes, exact_nodes - tail_nodes,
         100.0 * (exact_nodes - tail_nodes) / exact_nodes);
  assert(tail_nodes < exact_nodes);

  // The tail-merged graph must encode the identical DAWG word set...
  bool *nodes_reached =
      calloc_or_die(kwg_get_number_of_nodes(tail_kwg), sizeof(bool));
  DictionaryWordList *encoded_words = dictionary_word_list_create();
  kwg_write_words(tail_kwg, kwg_get_dawg_root_node_index(tail_kwg),
                  encoded_words, nodes_reached);
  assert_word_lists_are_equal(words, encoded_words);

  // ...and the identical GADDAG strings.
  DictionaryWordList *encoded_gaddag_strings = dictionary_word_list_create();
  kwg_write_gaddag_strings(tail_kwg, kwg_get_root_node_index(tail_kwg),
                           encoded_gaddag_strings, nodes_reached);
  assert_word_lists_are_equal(gaddag_strings, encoded_gaddag_strings);

  // Every node (past the two root pointers) must be reachable: no orphans.
  for (int i = 2; i < kwg_get_number_of_nodes(tail_kwg); i++) {
    assert(nodes_reached[i]);
  }

  free(nodes_reached);
  dictionary_word_list_destroy(encoded_gaddag_strings);
  dictionary_word_list_destroy(encoded_words);
  dictionary_word_list_destroy(gaddag_strings);
  dictionary_word_list_destroy(words);
  kwg_destroy(tail_kwg);
  kwg_destroy(exact_kwg);
  game_destroy(game);
  config_destroy(config);
}

// Builds the CSW21 DAWG with KWG_MAKER_MERGE_TAIL and the child-reordering
// KWG_MAKER_MERGE_TAIL_REORDER, confirms the reordered DAWG accepts the exact
// same word SET (child order now differs by design, so it is compared as a
// sorted set, not position-by-position), that every node is reachable, and
// reports the extra node-count savings from reordering.
void test_kwg_tail_reorder(void) {
  Config *config = config_create_or_die("set -lex CSW21");
  Game *game = config_game_create(config);
  const Player *player = game_get_player(game, 0);
  const KWG *csw_kwg = player_get_kwg(player);
  DictionaryWordList *words = dictionary_word_list_create();
  kwg_write_words(csw_kwg, kwg_get_dawg_root_node_index(csw_kwg), words, NULL);

  KWG *tail_kwg =
      make_kwg_from_words(words, KWG_MAKER_OUTPUT_DAWG, KWG_MAKER_MERGE_TAIL);
  KWG *reorder_kwg = make_kwg_from_words(words, KWG_MAKER_OUTPUT_DAWG,
                                         KWG_MAKER_MERGE_TAIL_REORDER);

  const int tail_nodes = kwg_get_number_of_nodes(tail_kwg);
  const int reorder_nodes = kwg_get_number_of_nodes(reorder_kwg);
  printf("kwg tail reorder (DAWG): tail=%d nodes, reorder=%d nodes (saved %d, "
         "%.2f%%)\n",
         tail_nodes, reorder_nodes, tail_nodes - reorder_nodes,
         100.0 * (tail_nodes - reorder_nodes) / tail_nodes);
  assert(reorder_nodes <= tail_nodes);

  // Reordered DAWG must encode the identical word set.
  bool *nodes_reached = calloc_or_die(reorder_nodes, sizeof(bool));
  DictionaryWordList *encoded = dictionary_word_list_create();
  kwg_write_words(reorder_kwg, kwg_get_dawg_root_node_index(reorder_kwg),
                  encoded, nodes_reached);
  // Compare as sets: reordering changes the DFS emission order.
  dictionary_word_list_sort(words);
  dictionary_word_list_sort(encoded);
  assert_word_lists_are_equal(words, encoded);

  // Every node (past the two root pointers) must be reachable: no orphans.
  for (int node_idx = 2; node_idx < reorder_nodes; node_idx++) {
    assert(nodes_reached[node_idx]);
  }

  free(nodes_reached);
  dictionary_word_list_destroy(encoded);
  dictionary_word_list_destroy(words);
  kwg_destroy(reorder_kwg);
  kwg_destroy(tail_kwg);
  game_destroy(game);
  config_destroy(config);
}

// Builds a GADDAG from `word_list` with each merge style, timing build speed.
// This is the endgame/PEG word-prune code path (make_kwg_from_words_small,
// OUTPUT_GADDAG), where build time — not node count — is what matters.
static void bench_kwg_merge_build(const char *label,
                                  const DictionaryWordList *word_list) {
  const int word_count = dictionary_word_list_get_count(word_list);
  const kwg_maker_merge_t merges[3] = {
      KWG_MAKER_MERGE_NONE, KWG_MAKER_MERGE_EXACT, KWG_MAKER_MERGE_TAIL};
  const char *names[3] = {"none  ", "exact ", "tail  "};
  int reps = 4000000 / (word_count + 1);
  if (reps < 10) {
    reps = 10;
  }
  if (reps > 4000) {
    reps = 4000;
  }
  printf("[%-14s] %6d words, reps=%d\n", label, word_count, reps);
  for (int merge_idx = 0; merge_idx < 3; merge_idx++) {
    KWG *warm = make_kwg_from_words_small(word_list, KWG_MAKER_OUTPUT_GADDAG,
                                          merges[merge_idx]);
    const int nodes = kwg_get_number_of_nodes(warm);
    kwg_destroy(warm);
    Timer timer;
    ctimer_start(&timer);
    for (int rep = 0; rep < reps; rep++) {
      KWG *kwg = make_kwg_from_words_small(word_list, KWG_MAKER_OUTPUT_GADDAG,
                                           merges[merge_idx]);
      kwg_destroy(kwg);
    }
    const double secs = ctimer_elapsed_seconds(&timer);
    printf("    %s  %9.1f us/build   %8d nodes\n", names[merge_idx],
           1e6 * secs / reps, nodes);
  }
}

void test_kwg_merge_build_bench(void) {
  Config *config = config_create_or_die("set -lex CSW21");
  Game *game = config_game_create(config);
  const KWG *csw_kwg = player_get_kwg(game_get_player(game, 0));
  DictionaryWordList *all_words = dictionary_word_list_create();
  kwg_write_words(csw_kwg, kwg_get_dawg_root_node_index(csw_kwg), all_words,
                  NULL);
  const int total = dictionary_word_list_get_count(all_words);

  // Synthetic size sweep: evenly-sampled (still sorted, unique) sublists at
  // sizes spanning what an endgame/PEG word prune produces.
  const int sizes[] = {100, 500, 2000, 10000, 50000};
  for (size_t size_idx = 0; size_idx < sizeof(sizes) / sizeof(sizes[0]);
       size_idx++) {
    const int target = sizes[size_idx];
    DictionaryWordList *sub = dictionary_word_list_create();
    for (int word_idx = 0; word_idx < target; word_idx++) {
      const DictionaryWord *word = dictionary_word_list_get_word(
          all_words, (int)((int64_t)word_idx * total / target));
      dictionary_word_list_add_word(sub, dictionary_word_get_word(word),
                                    dictionary_word_get_length(word));
    }
    char label[32];
    (void)snprintf(label, sizeof(label), "sampled-%d", target);
    bench_kwg_merge_build(label, sub);
    dictionary_word_list_destroy(sub);
  }
  dictionary_word_list_destroy(all_words);
  game_destroy(game);
  config_destroy(config);

  // A real, nearly-full board: the genuine word-prune output an endgame faces.
  Config *pos_config = config_create_or_die("set -lex NWL20");
  load_and_exec_config_or_die(
      pos_config,
      "cgp AIDER2U7/b1E1E2N1Z5/AWN1T2M1ATT3/LI1COBLE2OW3/OP2U2E2AA3/"
      "NE2CUSTARDS1Q1/ER1OH5I2U1/S2K2FOB1ERGOT/5HEXYLS2I1/4JIN6N1/"
      "2GOOP2NAIVEsT/1DIRE10/2GAY10/15/15 AEFILMR/DIV 371/412 0");
  const Game *pos_game = config_get_game(pos_config);
  const KWG *full_kwg = player_get_kwg(game_get_player(pos_game, 0));
  DictionaryWordList *pruned = dictionary_word_list_create();
  generate_possible_words(pos_game, full_kwg, pruned);
  bench_kwg_merge_build("real-fullboard", pruned);
  dictionary_word_list_destroy(pruned);
  config_destroy(pos_config);
}

void test_kwg_maker(void) {
  test_qi_xi_xu_word_trie();
  test_egg_unmerged_gaddag();
  test_careen_career_unmerged_gaddag();
  test_careen_career_exact_merged_gaddag();
  test_two_letter_trie();
  test_two_letter_merged_dawg();
  test_polish_gaddag();
  test_large_gaddag();
  test_full_csw_gaddag();
}
