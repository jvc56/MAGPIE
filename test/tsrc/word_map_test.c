#include <assert.h>

#include "../../src/def/letter_distribution_defs.h"
#include "../../src/ent/dictionary_word.h"
#include "../../src/ent/kwg.h"
#include "../../src/ent/word_map.h"
#include "../../src/impl/config.h"
#include "../../src/impl/kwg_maker.h"

#include "test_constants.h"
#include "test_util.h"

void test_csw(void) {
  Config *config = config_create_or_die("set -lex CSW21");
  Game *game = config_game_create(config);
  const Player *player = game_get_player(game, 0);
  const KWG *csw_kwg = player_get_kwg(player);
  DictionaryWordList *words = dictionary_word_list_create();
  kwg_write_words(csw_kwg, kwg_get_dawg_root_node_index(csw_kwg), words, NULL);
  printf("words: %d\n", dictionary_word_list_get_count(words));

  dictionary_word_list_destroy(words);
  game_destroy(game);
  config_destroy(config);
}

void test_anagram_sets(void) {
  Config *config = config_create_or_die("set -lex CSW21");
  Game *game = config_game_create(config);
  const Player *player = game_get_player(game, 0);
  const KWG *csw_kwg = player_get_kwg(player);
  DictionaryWordList *words = dictionary_word_list_create();
  kwg_write_words(csw_kwg, kwg_get_dawg_root_node_index(csw_kwg), words, NULL);

  MutableWordMap *word_map = word_map_create_anagram_sets(words);
  assert(word_map->maps[2].num_word_buckets == 127);
  assert(mutable_words_of_same_length_get_num_sets(&word_map->maps[2]) == 93);
  assert(word_map->maps[3].num_word_buckets == 1361);
  /*
  for (uint32_t i = 0; i < word_map->maps[3].num_word_buckets; i++) {
    const MutableWordMapBucket *bucket = &word_map->maps[3].word_buckets[i];
    printf("bucket %d: size %d", i, bucket->num_entries);
    for (uint32_t j = 0; j < bucket->num_entries; j++) {
      const MutableWordMapEntry *entry = &bucket->entries[j];
      printf(" (");
      for (int k = 0; k < dictionary_word_list_get_count(entry->letters); k++) {
        if (k > 0) {
          printf(", ");
        }
        const DictionaryWord *word =
  dictionary_word_list_get_word(entry->letters, k); for (int l = 0; l <
  dictionary_word_get_length(word); l++) { printf("%c",
  'A'-1+dictionary_word_get_word(word)[l]);
        }
      }
      printf(")");
    }
    printf("\n");
  }
  */
  assert(mutable_words_of_same_length_get_num_sets(&word_map->maps[3]) == 879);
  assert(mutable_words_of_same_length_get_num_sets(&word_map->maps[4]) == 3484);
  assert(mutable_words_of_same_length_get_num_sets(&word_map->maps[5]) == 8647);
  assert(mutable_words_of_same_length_get_num_sets(&word_map->maps[6]) ==
         17011);
  assert(mutable_words_of_same_length_get_num_sets(&word_map->maps[7]) ==
         27485);
  assert(mutable_words_of_same_length_get_num_sets(&word_map->maps[8]) ==
         36497);
  assert(mutable_words_of_same_length_get_num_sets(&word_map->maps[9]) ==
         39317);
  assert(mutable_words_of_same_length_get_num_sets(&word_map->maps[10]) ==
         35415);
  assert(mutable_words_of_same_length_get_num_sets(&word_map->maps[11]) ==
         28237);
  assert(mutable_words_of_same_length_get_num_sets(&word_map->maps[12]) ==
         20669);
  assert(mutable_words_of_same_length_get_num_sets(&word_map->maps[13]) ==
         14211);
  assert(mutable_words_of_same_length_get_num_sets(&word_map->maps[14]) ==
         9334);
  assert(mutable_words_of_same_length_get_num_sets(&word_map->maps[15]) ==
         5889);

  dictionary_word_list_destroy(words);
  game_destroy(game);
  config_destroy(config);
}

uint32_t blanks_to_bits(const LetterDistribution *ld, const char *rack_string) {
  Rack *rack = rack_create(ld_get_size(ld));
  rack_set_to_string(ld, rack, rack_string);
  uint32_t bits = 0;
  for (uint8_t ml = 0; ml < ld_get_size(ld); ml++) {
    bits |= (rack_get_letter(rack, ml) > 0) << ml;
  }
  return bits;
}

BitRack string_to_bit_rack(const LetterDistribution *ld,
                           const char *rack_string) {
  Rack *rack = rack_create(ld_get_size(ld));
  rack_set_to_string(ld, rack, rack_string);
  return bit_rack_create_from_rack(ld, rack);
}

uint32_t look_up_blank_set(const MutableBlankMap *blank_map,
                           const LetterDistribution *ld,
                           const char *rack_string) {
  const BitRack bit_rack = string_to_bit_rack(ld, rack_string);
  const int length = string_length(rack_string);
  const MutableBlanksForSameLengthMap *blank_map_for_length =
      &blank_map->maps[length];
  BitRack quotient;
  uint32_t bucket_index;
  bit_rack_div_mod(&bit_rack, blank_map_for_length->num_blank_buckets,
                   &quotient, &bucket_index);
  for (uint32_t i = 0;
       i < blank_map_for_length->blank_buckets[bucket_index].num_entries; i++) {
    const MutableBlankMapEntry *entry =
        &blank_map_for_length->blank_buckets[bucket_index].entries[i];
    if (bit_rack_equals(&entry->quotient, &quotient)) {
      return entry->blank_letters;
    }
  }
  return 0;
}

void assert_mutable_blank_map_set(const MutableBlankMap *blank_map,
                                  const LetterDistribution *ld,
                                  const char *rack_string,
                                  const char *expected_string) {
  assert(look_up_blank_set(blank_map, ld, rack_string) ==
         blanks_to_bits(ld, expected_string));
}

const DictionaryWordList *
look_up_double_blank_set(const MutableDoubleBlankMap *double_blank_map,
                         const LetterDistribution *ld,
                         const char *rack_string) {
  const BitRack bit_rack = string_to_bit_rack(ld, rack_string);
  const int length = string_length(rack_string);
  const MutableDoubleBlanksForSameLengthMap *double_blank_map_for_length =
      &double_blank_map->maps[length];
  BitRack quotient;
  uint32_t bucket_index;
  bit_rack_div_mod(&bit_rack,
                   double_blank_map_for_length->num_double_blank_buckets,
                   &quotient, &bucket_index);
  printf("double_blank_map_for_length->num_double_blank_buckets %d\n",
         double_blank_map_for_length->num_double_blank_buckets);
  printf("bucket index %d\n", bucket_index);
  printf("double_blank_map_for_length->double_blank_buckets[bucket_index].num_"
         "entries: %d\n",
         double_blank_map_for_length->double_blank_buckets[bucket_index]
             .num_entries);
  for (uint32_t i = 0;
       i < double_blank_map_for_length->double_blank_buckets[bucket_index]
               .num_entries;
       i++) {
    const MutableDoubleBlankMapEntry *entry =
        &double_blank_map_for_length->double_blank_buckets[bucket_index]
             .entries[i];
    if (bit_rack_equals(&entry->quotient, &quotient)) {
      return entry->letter_pairs;
    }
  }
  return NULL;
}

void assert_mutable_double_blank_set_count(
    const MutableDoubleBlankMap *double_blank_map, const LetterDistribution *ld,
    const char *rack_string, int expected_count) {
  const DictionaryWordList *double_blanks =
      look_up_double_blank_set(double_blank_map, ld, rack_string);
  if (expected_count > 0) {
    assert(double_blanks != NULL);
  } else {
    assert(double_blanks == NULL);
    return;
  }
  assert(dictionary_word_list_get_count(double_blanks) == expected_count);
}

void assert_mutable_double_blank_set_contains_once(
    const MutableDoubleBlankMap *double_blank_map, const LetterDistribution *ld,
    const char *rack_string, const char *expected_pair_string) {
  const DictionaryWordList *double_blanks =
      look_up_double_blank_set(double_blank_map, ld, rack_string);
  assert(double_blanks != NULL);
  assert_word_count(ld, double_blanks, expected_pair_string, 1);
}

void test_blanks(void) {
  Config *config = config_create_or_die("set -lex CSW21");
  const LetterDistribution *ld = config_get_ld(config);
  Game *game = config_game_create(config);
  const Player *player = game_get_player(game, 0);
  const KWG *csw_kwg = player_get_kwg(player);
  DictionaryWordList *words = dictionary_word_list_create();
  kwg_write_words(csw_kwg, kwg_get_dawg_root_node_index(csw_kwg), words, NULL);
  MutableWordMap *word_map = word_map_create_anagram_sets(words);

  MutableBlankMap *blank_map = word_map_create_blank_sets(word_map, ld);
  assert(blank_map != NULL);
  for (int i = 2; i <= 15; i++) {
    printf("length %d num sets %d average bucket %f\n", i,
           mutable_blanks_for_same_length_get_num_sets(&blank_map->maps[i]),
           average_blank_sets_per_nonempty_bucket(&blank_map->maps[i]));
  }
  assert_mutable_blank_map_set(blank_map, ld, "Q?", "I");
  assert_mutable_blank_map_set(blank_map, ld, "APNOEAL?", "J");
  assert_mutable_blank_map_set(blank_map, ld, "AUTHORISE?", "DRSZ");
  assert_mutable_blank_map_set(blank_map, ld, "SATINE?",
                               "ABCDEFGHIJKLMNOPRSTUVWXZ");
  assert_mutable_blank_map_set(blank_map, ld, "COMPUTERI?ATION", "SZ");

  dictionary_word_list_destroy(words);
  game_destroy(game);
  config_destroy(config);
}

void test_double_blanks(void) {
  Config *config = config_create_or_die("set -lex CSW21");
  const LetterDistribution *ld = config_get_ld(config);
  Game *game = config_game_create(config);
  const Player *player = game_get_player(game, 0);
  const KWG *csw_kwg = player_get_kwg(player);
  DictionaryWordList *words = dictionary_word_list_create();
  kwg_write_words(csw_kwg, kwg_get_dawg_root_node_index(csw_kwg), words, NULL);
  MutableWordMap *word_map = word_map_create_anagram_sets(words);

  MutableDoubleBlankMap *double_blank_map =
      word_map_create_double_blank_sets(word_map, ld);
  assert(double_blank_map != NULL);
  for (int i = 2; i <= 15; i++) {
    printf("length %d num sets %d\n", i,
           mutable_double_blanks_for_same_length_get_num_sets(
               &double_blank_map->maps[i]));
  }
  int map2_num_buckets = double_blank_map->maps[2].num_double_blank_buckets;
  for (int i = 0; i < map2_num_buckets; i++) {
    int num_entries =
        double_blank_map->maps[2].double_blank_buckets[i].num_entries;
    if (num_entries > 0) {
      printf("bucket %d: %d\n", i, num_entries);
    }
  }
  assert_mutable_double_blank_set_count(double_blank_map, ld, "??", 93);
  // AB / BA
  assert_mutable_double_blank_set_contains_once(double_blank_map, ld, "??",
                                                "AB");

  assert_mutable_double_blank_set_count(double_blank_map, ld, "EUREKA??", 3);
  // HEUREKAS
  assert_mutable_double_blank_set_contains_once(double_blank_map, ld,
                                                "EUREKA??", "HS");
  // REUPTAKE
  assert_mutable_double_blank_set_contains_once(double_blank_map, ld,
                                                "EUREKA??", "PT");
  // SQUEAKER
  assert_mutable_double_blank_set_contains_once(double_blank_map, ld,
                                                "EUREKA??", "QS");

  dictionary_word_list_destroy(words);
  game_destroy(game);
  config_destroy(config);
}

void test_word_map(void) {
  test_csw();
  test_anagram_sets();
  test_blanks();
  test_double_blanks();
}