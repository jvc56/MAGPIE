#include <assert.h>
#include <stdio.h>
#include <time.h>

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
  // printf("double_blank_map_for_length->num_double_blank_buckets %d\n",
  //        double_blank_map_for_length->num_double_blank_buckets);
  // printf("bucket index %d\n", bucket_index);
  // printf("double_blank_map_for_length->double_blank_buckets[bucket_index].num_"
  //        "entries: %d\n",
  //        double_blank_map_for_length->double_blank_buckets[bucket_index]
  //            .num_entries);
  for (uint32_t i = 0;
       i < double_blank_map_for_length->double_blank_buckets[bucket_index]
               .num_entries;
       i++) {
    const MutableDoubleBlankMapEntry *entry =
        &double_blank_map_for_length->double_blank_buckets[bucket_index]
             .entries[i];
    if (bit_rack_equals(&entry->quotient, &quotient)) {
      //printf("matching quotient, returning letter_pairs\n");
      return entry->letter_pairs;
    }
  }
  //printf("no matching quotient, returning NULL\n");
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
    printf("length %d num buckets %d num sets %d average bucket %f\n", i,
           blank_map->maps[i].num_blank_buckets,
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

void test_resize_words(void) {
  Config *config = config_create_or_die("set -lex CSW21");
  const LetterDistribution *ld = config_get_ld(config);
  Game *game = config_game_create(config);
  const Player *player = game_get_player(game, 0);
  const KWG *csw_kwg = player_get_kwg(player);
  DictionaryWordList *words = dictionary_word_list_create();
  kwg_write_words(csw_kwg, kwg_get_dawg_root_node_index(csw_kwg), words, NULL);
  const MutableWordMap *word_map = word_map_create_anagram_sets(words);
  uint32_t min_num_buckets = compute_min_num_buckets(ld);
  assert(min_num_buckets == 587);
  const MutableWordMap *resized_word_map =
      mutable_word_map_resize(word_map, min_num_buckets);
  assert(resized_word_map != NULL);
  for (int i = 0; i <= BOARD_DIM; i++) {
    assert(
        mutable_words_of_same_length_get_num_sets(&resized_word_map->maps[i]) ==
        mutable_words_of_same_length_get_num_sets(&word_map->maps[i]));
  }
  dictionary_word_list_destroy(words);
  game_destroy(game);
  config_destroy(config);
}

void test_resize_blanks(void) {
  Config *config = config_create_or_die("set -lex CSW21");
  const LetterDistribution *ld = config_get_ld(config);
  Game *game = config_game_create(config);
  const Player *player = game_get_player(game, 0);
  const KWG *csw_kwg = player_get_kwg(player);
  DictionaryWordList *words = dictionary_word_list_create();
  kwg_write_words(csw_kwg, kwg_get_dawg_root_node_index(csw_kwg), words, NULL);
  MutableWordMap *word_map = word_map_create_anagram_sets(words);
  MutableBlankMap *blank_map = word_map_create_blank_sets(word_map, ld);

  uint32_t min_num_buckets = compute_min_num_buckets(ld);
  assert(min_num_buckets == 587);
  MutableBlankMap *resized_blank_map =
      mutable_blank_map_resize(blank_map, ld, min_num_buckets);
  assert(resized_blank_map != NULL);
  for (int i = 0; i <= BOARD_DIM; i++) {
    assert(mutable_blanks_for_same_length_get_num_sets(
               &resized_blank_map->maps[i]) ==
           mutable_blanks_for_same_length_get_num_sets(&blank_map->maps[i]));
  }
  for (int i = 2; i <= 15; i++) {
    printf("(resized) length %d num buckets %d num sets %d average bucket %f\n",
           i, resized_blank_map->maps[i].num_blank_buckets,
           mutable_blanks_for_same_length_get_num_sets(
               &resized_blank_map->maps[i]),
           average_blank_sets_per_nonempty_bucket(&resized_blank_map->maps[i]));
  }

  assert_mutable_blank_map_set(resized_blank_map, ld, "Q?", "I");
  assert_mutable_blank_map_set(resized_blank_map, ld, "APNOEAL?", "J");
  assert_mutable_blank_map_set(resized_blank_map, ld, "AUTHORISE?", "DRSZ");
  assert_mutable_blank_map_set(resized_blank_map, ld, "SATINE?",
                               "ABCDEFGHIJKLMNOPRSTUVWXZ");
  assert_mutable_blank_map_set(resized_blank_map, ld, "COMPUTERI?ATION", "SZ");

  dictionary_word_list_destroy(words);
  game_destroy(game);
  config_destroy(config);
}

void test_resize_double_blanks(void) {
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

  uint32_t min_num_buckets = compute_min_num_buckets(ld);
  assert(min_num_buckets == 587);
  MutableDoubleBlankMap *resized_double_blank_map =
      mutable_double_blank_map_resize(double_blank_map, ld, min_num_buckets);
  assert(resized_double_blank_map != NULL);
  for (int i = 0; i <= BOARD_DIM; i++) {
    assert(mutable_double_blanks_for_same_length_get_num_sets(
               &resized_double_blank_map->maps[i]) ==
           mutable_double_blanks_for_same_length_get_num_sets(
               &double_blank_map->maps[i]));
  }
  for (int i = 2; i <= 15; i++) {
    printf("(resized) length %d num sets %d\n", i,
           mutable_double_blanks_for_same_length_get_num_sets(
               &resized_double_blank_map->maps[i]));
  }
  assert_mutable_double_blank_set_count(resized_double_blank_map, ld, "??", 93);
  // AB / BA
  assert_mutable_double_blank_set_contains_once(resized_double_blank_map, ld,
                                                "??", "AB");

  assert_mutable_double_blank_set_count(resized_double_blank_map, ld,
                                        "EUREKA??", 3);
  // HEUREKAS
  assert_mutable_double_blank_set_contains_once(resized_double_blank_map, ld,
                                                "EUREKA??", "HS");
  // REUPTAKE
  assert_mutable_double_blank_set_contains_once(resized_double_blank_map, ld,
                                                "EUREKA??", "PT");
  // SQUEAKER
  assert_mutable_double_blank_set_contains_once(resized_double_blank_map, ld,
                                                "EUREKA??", "QS");

  dictionary_word_list_destroy(words);
  game_destroy(game);
  config_destroy(config);                                                
}

void assert_word_in_buffer(uint8_t *buffer, const char *expected_word,
                           const LetterDistribution *ld, int start, int length) {
  char hl[2] = {0, 0};                            
  for (int i = 0; i < length; i++) {
    hl[0] = expected_word[i];
    assert(buffer[start + i] == ld_hl_to_ml(ld, hl));
  }
}

void test_create_from_mutables(void) {
  Config *config = config_create_or_die("set -lex CSW21");
  const LetterDistribution *ld = config_get_ld(config);
  Game *game = config_game_create(config);
  const Player *player = game_get_player(game, 0);
  const KWG *csw_kwg = player_get_kwg(player);
  DictionaryWordList *words = dictionary_word_list_create();
  kwg_write_words(csw_kwg, kwg_get_dawg_root_node_index(csw_kwg), words, NULL);

  DictionaryWordList *q_words = dictionary_word_list_create();
  const uint8_t q = ld_hl_to_ml(ld, "Q");
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

  MutableWordMap *word_map = word_map_create_anagram_sets(q_words);
  MutableBlankMap *blank_map = word_map_create_blank_sets(word_map, ld);
  MutableDoubleBlankMap *double_blank_map =
      word_map_create_double_blank_sets(word_map, ld);
  WordMap *map = word_map_create_from_mutables(word_map, blank_map,
                                               double_blank_map);
  assert(map != NULL);
  assert(map->major_version == WORD_MAP_MAJOR_VERSION);
  assert(map->minor_version == WORD_MAP_MINOR_VERSION);
  assert(map->min_word_length == 2);
  assert(map->max_word_length == 15);
  assert(map->max_blank_pair_bytes == 2 * 42); // EIQSTU??
  assert(map->max_word_lookup_bytes == 47 * 8); // EIQSTU??

  uint8_t *buffer = malloc_or_die(map->max_word_lookup_bytes);
  BitRack iq = string_to_bit_rack(ld, "IQ");
  int bytes_written =
      word_map_write_blankless_words_to_buffer(map, &iq, buffer);
  assert(bytes_written == 2);
  assert_word_in_buffer(buffer, "QI", ld, 0, 2);

  BitRack cv = string_to_bit_rack(ld, "CV");
  bytes_written = word_map_write_blankless_words_to_buffer(map, &cv, buffer);
  assert(bytes_written == 0);

  BitRack torque = string_to_bit_rack(ld, "TORQUE");
  bytes_written = word_map_write_blankless_words_to_buffer(map, &torque, buffer);
  assert(bytes_written == 6*3);
  assert_word_in_buffer(buffer, "QUOTER", ld, 6*0, 6);
  assert_word_in_buffer(buffer, "ROQUET", ld, 6*1, 6);
  assert_word_in_buffer(buffer, "TORQUE", ld, 6*2, 6);

  BitRack questor = string_to_bit_rack(ld, "QUESTOR");
  bytes_written = word_map_write_blankless_words_to_buffer(map, &questor, buffer);
  assert(bytes_written == 7*4);
  assert_word_in_buffer(buffer, "QUESTOR", ld, 7*0, 7);
  assert_word_in_buffer(buffer, "QUOTERS", ld, 7*1, 7);
  assert_word_in_buffer(buffer, "ROQUETS", ld, 7*2, 7);
  assert_word_in_buffer(buffer, "TORQUES", ld, 7*3, 7);

  BitRack docquets = string_to_bit_rack(ld, "DOCQUETS");
  bytes_written = word_map_write_blankless_words_to_buffer(map, &docquets, buffer);
  assert(bytes_written == 8);
  assert_word_in_buffer(buffer, "DOCQUETS", ld, 8*0, 8);

  BitRack requoted = string_to_bit_rack(ld, "REQUOTED");
  bytes_written = word_map_write_blankless_words_to_buffer(map, &requoted, buffer);
  assert(bytes_written == 8*2);
  assert_word_in_buffer(buffer, "REQUOTED", ld, 8*0, 8);
  assert_word_in_buffer(buffer, "ROQUETED", ld, 8*1, 8);

  BitRack unquoted = string_to_bit_rack(ld, "UNQUOTED");
  bytes_written = word_map_write_blankless_words_to_buffer(map, &unquoted, buffer);
  assert(bytes_written == 8);
  assert_word_in_buffer(buffer, "UNQUOTED", ld, 8*0, 8);

  BitRack q_blank = string_to_bit_rack(ld, "Q?");
  bytes_written = word_map_write_blanks_to_buffer(map, &q_blank, buffer);
  assert(bytes_written == 2);
  assert_word_in_buffer(buffer, "QI", ld, 2*0, 2);

  BitRack square_blank = string_to_bit_rack(ld, "SQUARE?");
  bytes_written = word_map_write_blanks_to_buffer(map, &square_blank, buffer);
  assert(bytes_written == 7 * 13);
  assert_word_in_buffer(buffer, "BARQUES", ld, 7 * 0, 7);
  assert_word_in_buffer(buffer, "SQUARED", ld, 7 * 1, 7);
  assert_word_in_buffer(buffer, "QUAERES", ld, 7 * 2, 7);
  assert_word_in_buffer(buffer, "QUASHER", ld, 7 * 3, 7);
  assert_word_in_buffer(buffer, "QUAKERS", ld, 7 * 4, 7);
  assert_word_in_buffer(buffer, "MARQUES", ld, 7 * 5, 7);
  assert_word_in_buffer(buffer, "MASQUER", ld, 7 * 6, 7);
  assert_word_in_buffer(buffer, "SQUARER", ld, 7 * 7, 7);
  assert_word_in_buffer(buffer, "SQUARES", ld, 7 * 8, 7);
  assert_word_in_buffer(buffer, "QUAREST", ld, 7 * 9, 7);
  assert_word_in_buffer(buffer, "QUARTES", ld, 7 * 10, 7);
  assert_word_in_buffer(buffer, "QUATRES", ld, 7 * 11, 7);
  assert_word_in_buffer(buffer, "QUAVERS", ld, 7 * 12, 7);

  BitRack trongle_blank = string_to_bit_rack(ld, "TRONGLE?");
  bytes_written = word_map_write_blanks_to_buffer(map, &trongle_blank, buffer);
  assert(bytes_written == 0);

  BitRack double_blank = string_to_bit_rack(ld, "??");
  bytes_written =
      word_map_write_double_blanks_to_buffer(map, &double_blank, buffer);
  assert(bytes_written == 2);
  assert_word_in_buffer(buffer, "QI", ld, 0, 2);

  BitRack quoted_double_blank = string_to_bit_rack(ld, "QUOTED??");
  bytes_written =
      word_map_write_double_blanks_to_buffer(map, &quoted_double_blank, buffer);
  assert(bytes_written == 8*4);
  assert_word_in_buffer(buffer, "DOCQUETS", ld, 8*0, 8);
  assert_word_in_buffer(buffer, "UNQUOTED", ld, 8*1, 8);
  assert_word_in_buffer(buffer, "REQUOTED", ld, 8*2, 8);
  assert_word_in_buffer(buffer, "ROQUETED", ld, 8*3, 8);

  BitRack quarterbackin_double_blank = string_to_bit_rack(ld, "QUARTERBACKIN??");
  bytes_written =
      word_map_write_double_blanks_to_buffer(map, &quarterbackin_double_blank, buffer);
  assert(bytes_written == 15);
  assert_word_in_buffer(buffer, "QUARTERBACKINGS", ld, 0, 15);

  BitRack vxz_double_blank = string_to_bit_rack(ld, "VXZ??");
  bytes_written =
      word_map_write_double_blanks_to_buffer(map, &vxz_double_blank, buffer);
  assert(bytes_written == 0);

  bytes_written = word_map_write_words_to_buffer(map, &iq, buffer);
  assert(bytes_written == 2);
  assert_word_in_buffer(buffer, "QI", ld, 0, 2);

  bytes_written = word_map_write_words_to_buffer(map, &square_blank, buffer);
  assert(bytes_written == 7 * 13);
  assert_word_in_buffer(buffer, "BARQUES", ld, 0, 7);
  assert_word_in_buffer(buffer, "SQUARED", ld, 7, 7);
  assert_word_in_buffer(buffer, "QUAERES", ld, 14, 7);
  assert_word_in_buffer(buffer, "QUASHER", ld, 21, 7);
  assert_word_in_buffer(buffer, "QUAKERS", ld, 28, 7);
  assert_word_in_buffer(buffer, "MARQUES", ld, 35, 7);
  assert_word_in_buffer(buffer, "MASQUER", ld, 42, 7);
  assert_word_in_buffer(buffer, "SQUARER", ld, 49, 7);
  assert_word_in_buffer(buffer, "SQUARES", ld, 56, 7);
  assert_word_in_buffer(buffer, "QUAREST", ld, 63, 7);
  assert_word_in_buffer(buffer, "QUARTES", ld, 70, 7);
  assert_word_in_buffer(buffer, "QUATRES", ld, 77, 7);
  assert_word_in_buffer(buffer, "QUAVERS", ld, 84, 7);

  bytes_written =
      word_map_write_words_to_buffer(map, &quarterbackin_double_blank, buffer);
  assert(bytes_written == 15);
  assert_word_in_buffer(buffer, "QUARTERBACKINGS", ld, 0, 15);

  free(buffer);
  dictionary_word_list_destroy(q_words);
  dictionary_word_list_destroy(words);
  game_destroy(game);
  config_destroy(config);
}

void test_word_map(void) {
  //test_csw();
  //test_anagram_sets();
  //test_blanks();
  //test_double_blanks();
  //test_resize_words();
  //test_resize_blanks();
  //test_resize_double_blanks();
  test_create_from_mutables();
}