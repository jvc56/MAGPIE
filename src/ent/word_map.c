#include <limits.h>
#include <stdint.h>

#include "word_map.h"
#include "bit_rack.h"
#include "dictionary_word.h"

#include "../util/util.h"
#include "letter_distribution.h"
#include "rack.h"

#include "../str/rack_string.h"

uint32_t next_prime(uint32_t n) {
  if (n <= 2) {
    return 2;
  }
  if (n % 2 == 0) {
    n++;
  }
  for (int i = n;; i += 2) {
    bool is_prime = true;
    for (int j = 3; j * j <= i; j += 2) {
      if (i % j == 0) {
        is_prime = false;
        break;
      }
    }
    if (is_prime) {
      return i;
    }
  }
  return -1;
}

void mutable_word_map_bucket_insert_word(MutableWordMapBucket *bucket,
                                         BitRack quotient,
                                         const DictionaryWord *word) {
  for (uint32_t i = 0; i < bucket->num_entries; i++) {
    MutableWordMapEntry *entry = &bucket->entries[i];
    if (bit_rack_equals(&entry->quotient, &quotient)) {
      dictionary_word_list_add_word(entry->letters,
                                    dictionary_word_get_word(word),
                                    dictionary_word_get_length(word));
      return;
    }
  }
  if (bucket->num_entries == bucket->capacity) {
    bucket->capacity *= 2;
    bucket->entries = realloc_or_die(
        bucket->entries, sizeof(MutableWordMapEntry) * bucket->capacity);
  }
  MutableWordMapEntry *entry = &bucket->entries[bucket->num_entries];
  entry->quotient = quotient;
  entry->letters = dictionary_word_list_create_with_capacity(1);
  dictionary_word_list_add_word(entry->letters, dictionary_word_get_word(word),
                                dictionary_word_get_length(word));
  bucket->num_entries++;
}

MutableWordMap *word_map_create_anagram_sets(const DictionaryWordList *words) {
  int num_words_by_length[BOARD_DIM + 1] = {0};
  for (int i = 0; i < dictionary_word_list_get_count(words); i++) {
    const DictionaryWord *word = dictionary_word_list_get_word(words, i);
    num_words_by_length[dictionary_word_get_length(word)]++;
  }
  MutableWordMap *word_map = malloc_or_die(sizeof(MutableWordMap));
  for (int i = 0; i <= BOARD_DIM; i++) {
    MutableWordsOfSameLengthMap *map = &word_map->maps[i];
    printf("num words by length %d: %d\n", i, num_words_by_length[i]);
    if (num_words_by_length[i] == 0) {
      map->num_word_buckets = 0;
      map->word_buckets = NULL;
      continue;
    }
    map->num_word_buckets = next_prime(num_words_by_length[i]);
    map->word_buckets =
        malloc_or_die(sizeof(MutableWordMapBucket) * map->num_word_buckets);
    for (uint32_t j = 0; j < map->num_word_buckets; j++) {
      MutableWordMapBucket *bucket = &map->word_buckets[j];
      bucket->num_entries = 0;
      bucket->capacity = 1;
      bucket->entries =
          malloc_or_die(sizeof(MutableWordMapEntry) * bucket->capacity);
    }
  }
  for (int i = 0; i < dictionary_word_list_get_count(words); i++) {
    const DictionaryWord *word = dictionary_word_list_get_word(words, i);
    const uint8_t length = dictionary_word_get_length(word);
    MutableWordsOfSameLengthMap *map = &word_map->maps[length];
    const BitRack bit_rack = bit_rack_create_from_dictionary_word(word);
    BitRack quotient;
    uint32_t bucket_index;
    bit_rack_div_mod(&bit_rack, map->num_word_buckets, &quotient,
                     &bucket_index);
    MutableWordMapBucket *bucket = &map->word_buckets[bucket_index];
    mutable_word_map_bucket_insert_word(bucket, quotient, word);
  }
  return word_map;
}

int mutable_words_of_same_length_get_num_sets(
    const MutableWordsOfSameLengthMap *map) {
  int num_sets = 0;
  for (uint32_t i = 0; i < map->num_word_buckets; i++) {
    const MutableWordMapBucket *bucket = &map->word_buckets[i];
    num_sets += bucket->num_entries;
  }
  return num_sets;
}

int mutable_blanks_for_same_length_get_num_sets(
    const MutableBlanksForSameLengthMap *map) {
  int num_sets = 0;
  for (uint32_t i = 0; i < map->num_blank_buckets; i++) {
    const MutableBlankMapBucket *bucket = &map->blank_buckets[i];
    num_sets += bucket->num_entries;
  }
  return num_sets;
}

void set_blank_map_bits(MutableBlanksForSameLengthMap *blank_map,
                        const Rack *rack, const LetterDistribution *ld,
                        uint32_t blank_letters) {
  BitRack bit_rack = bit_rack_create_from_rack(ld, rack);
  BitRack quotient;
  uint32_t bucket_index;
  bit_rack_div_mod(&bit_rack, blank_map->num_blank_buckets, &quotient,
                   &bucket_index);
  MutableBlankMapBucket *bucket = &blank_map->blank_buckets[bucket_index];
  if (bucket->num_entries == bucket->capacity) {
    bucket->capacity *= 2;
    bucket->entries = realloc_or_die(
        bucket->entries, sizeof(MutableBlankMapEntry) * bucket->capacity);
  }
  MutableBlankMapEntry *entry = &bucket->entries[bucket->num_entries];
  entry->quotient = quotient;
  entry->blank_letters = blank_letters;
  bucket->num_entries++;
}

void set_blank_map_bit(MutableBlanksForSameLengthMap *blank_map,
                       const Rack *rack, const LetterDistribution *ld,
                       uint8_t ml) {
  BitRack bit_rack = bit_rack_create_from_rack(ld, rack);
  BitRack quotient;
  uint32_t bucket_index;
  bit_rack_div_mod(&bit_rack, blank_map->num_blank_buckets, &quotient,
                   &bucket_index);
  MutableBlankMapBucket *bucket = &blank_map->blank_buckets[bucket_index];
  for (uint32_t i = 0; i < bucket->num_entries; i++) {
    MutableBlankMapEntry *entry = &bucket->entries[i];
    if (bit_rack_equals(&entry->quotient, &quotient)) {
      entry->blank_letters |= 1 << ml;
      return;
    }
  }
  if (bucket->num_entries == bucket->capacity) {
    bucket->capacity *= 2;
    bucket->entries = realloc_or_die(
        bucket->entries, sizeof(MutableBlankMapEntry) * bucket->capacity);
  }
  MutableBlankMapEntry *entry = &bucket->entries[bucket->num_entries];
  entry->quotient = quotient;
  entry->blank_letters = 1 << ml;
  bucket->num_entries++;
}

MutableBlanksForSameLengthMap
blanks_for_same_length(const MutableWordsOfSameLengthMap *word_map, int length,
                       const LetterDistribution *ld) {
  MutableBlanksForSameLengthMap blank_map;
  blank_map.num_blank_buckets = next_prime(word_map->num_word_buckets * length);
  blank_map.blank_buckets =
      malloc_or_die(sizeof(MutableWordMapBucket) * blank_map.num_blank_buckets);
  for (uint32_t i = 0; i < blank_map.num_blank_buckets; i++) {
    MutableBlankMapBucket *bucket = &blank_map.blank_buckets[i];
    bucket->num_entries = 0;
    bucket->capacity = 1;
    bucket->entries =
        malloc_or_die(sizeof(MutableBlankMapEntry) * bucket->capacity);
  }
  for (uint32_t i = 0; i < word_map->num_word_buckets; i++) {
    const MutableWordMapBucket *word_bucket = &word_map->word_buckets[i];
    for (uint32_t j = 0; j < word_bucket->num_entries; j++) {
      const MutableWordMapEntry *entry = &word_bucket->entries[j];
      BitRack full_bit_rack =
          bit_rack_mul(&entry->quotient, word_map->num_word_buckets);
      bit_rack_add_uint32(&full_bit_rack, i);
      Rack *rack = bit_rack_to_rack(&full_bit_rack, ld);
      for (uint8_t ml = 1; ml < ld->size; ml++) {
        if (rack_get_letter(rack, ml) > 0) {
          if (rack_get_letter(rack, ml) > 0) {
            rack_take_letter(rack, ml);
            rack_add_letter(rack, BLANK_MACHINE_LETTER);
            set_blank_map_bit(&blank_map, rack, ld, ml);
            rack_take_letter(rack, BLANK_MACHINE_LETTER);
            rack_add_letter(rack, ml);
          }
        }
      }
      rack_destroy(rack);
    }
  }
  return blank_map;
}

MutableBlankMap *word_map_create_blank_sets(const MutableWordMap *word_map,
                                            const LetterDistribution *ld) {
  MutableBlankMap *blank_map = malloc_or_die(sizeof(MutableWordMap));
  for (int i = 0; i <= BOARD_DIM; i++) {
    blank_map->maps[i] = blanks_for_same_length(&word_map->maps[i], i, ld);
  }
  return blank_map;
}

double average_word_sets_per_nonempty_bucket(
    const MutableWordsOfSameLengthMap *word_map) {
  int num_buckets = 0;
  int num_sets = 0;
  for (uint32_t j = 0; j < word_map->num_word_buckets; j++) {
    const MutableWordMapBucket *bucket = &word_map->word_buckets[j];
    if (bucket->num_entries > 0) {
      num_buckets++;
      num_sets += bucket->num_entries;
    }
  }
  return (double)num_sets / num_buckets;
}

double average_blank_sets_per_nonempty_bucket(
    const MutableBlanksForSameLengthMap *blank_map) {
  int num_buckets = 0;
  int num_sets = 0;
  for (uint32_t j = 0; j < blank_map->num_blank_buckets; j++) {
    const MutableBlankMapBucket *bucket = &blank_map->blank_buckets[j];
    if (bucket->num_entries > 0) {
      num_buckets++;
      num_sets += bucket->num_entries;
    }
  }
  return (double)num_sets / num_buckets;
}

void mutable_double_blank_map_bucket_insert_word(
    MutableDoubleBlankMapBucket *bucket, BitRack quotient,
    const uint8_t *blanks_as_word) {
  for (uint32_t i = 0; i < bucket->num_entries; i++) {
    MutableDoubleBlankMapEntry *entry = &bucket->entries[i];
    if (bit_rack_equals(&entry->quotient, &quotient)) {
      dictionary_word_list_add_word(entry->letter_pairs, blanks_as_word, 2);
      return;
    }
  }
  if (bucket->num_entries == bucket->capacity) {
    bucket->capacity *= 2;
    bucket->entries = realloc_or_die(
        bucket->entries, sizeof(MutableDoubleBlankMapEntry) * bucket->capacity);
  }
  MutableDoubleBlankMapEntry *entry = &bucket->entries[bucket->num_entries];
  entry->quotient = quotient;
  entry->letter_pairs = dictionary_word_list_create_with_capacity(1);
  dictionary_word_list_add_word(entry->letter_pairs, blanks_as_word, 2);
  bucket->num_entries++;
}

void mutable_double_blank_map_insert_word(
    MutableDoubleBlanksForSameLengthMap *map, Rack *word_rack,
    const LetterDistribution *ld) {
  StringBuilder *sb = string_builder_create();
  string_builder_add_rack(sb, word_rack, ld, false);
  //printf("mutable_double_blank_map_insert_word: %s\n", string_builder_peek(sb));
  string_builder_destroy(sb);
  uint8_t blanks_as_word[2];
  for (uint8_t ml1 = 1; ml1 < ld->size; ml1++) {
    if (rack_get_letter(word_rack, ml1) > 0) {
      rack_take_letter(word_rack, ml1);
      rack_add_letter(word_rack, BLANK_MACHINE_LETTER);
      blanks_as_word[0] = ml1;
      for (uint8_t ml2 = ml1; ml2 < ld->size; ml2++) {
        if (rack_get_letter(word_rack, ml2) > 0) {
          blanks_as_word[1] = ml2;
          rack_take_letter(word_rack, ml2);
          rack_add_letter(word_rack, BLANK_MACHINE_LETTER);
          sb = string_builder_create();
          string_builder_add_rack(sb, word_rack, ld, false);
          //printf("word_rack %s\n", string_builder_peek(sb));
          string_builder_destroy(sb);
          BitRack bit_rack = bit_rack_create_from_rack(ld, word_rack);
          BitRack quotient;
          uint32_t bucket_index;
          bit_rack_div_mod(&bit_rack, map->num_double_blank_buckets, &quotient,
                           &bucket_index);
          MutableDoubleBlankMapBucket *bucket =
              &map->double_blank_buckets[bucket_index];
          mutable_double_blank_map_bucket_insert_word(bucket, quotient,
                                                      blanks_as_word);
          rack_take_letter(word_rack, BLANK_MACHINE_LETTER);
          rack_add_letter(word_rack, ml2);
        }
      }
      rack_take_letter(word_rack, BLANK_MACHINE_LETTER);
      rack_add_letter(word_rack, ml1);
    }
  }
}

MutableDoubleBlanksForSameLengthMap
double_blanks_for_same_length(const MutableWordsOfSameLengthMap *word_map,
                              int length, const LetterDistribution *ld) {
  MutableDoubleBlanksForSameLengthMap double_blank_map;
  double_blank_map.num_double_blank_buckets =
      next_prime(word_map->num_word_buckets * length);
  double_blank_map.double_blank_buckets =
      malloc_or_die(sizeof(MutableDoubleBlankMapBucket) *
                    double_blank_map.num_double_blank_buckets);
  for (uint32_t i = 0; i < double_blank_map.num_double_blank_buckets; i++) {
    MutableDoubleBlankMapBucket *bucket =
        &double_blank_map.double_blank_buckets[i];
    bucket->num_entries = 0;
    bucket->capacity = 1;
    bucket->entries =
        malloc_or_die(sizeof(MutableDoubleBlankMapEntry) * bucket->capacity);
  }
  for (uint32_t i = 0; i < word_map->num_word_buckets; i++) {
    const MutableWordMapBucket *word_bucket = &word_map->word_buckets[i];
    for (uint32_t j = 0; j < word_bucket->num_entries; j++) {
      const MutableWordMapEntry *entry = &word_bucket->entries[j];
      BitRack full_bit_rack =
          bit_rack_mul(&entry->quotient, word_map->num_word_buckets);
      bit_rack_add_uint32(&full_bit_rack, i);
      Rack *rack = bit_rack_to_rack(&full_bit_rack, ld);
      mutable_double_blank_map_insert_word(&double_blank_map, rack, ld);
    }
  }
  return double_blank_map;
}

MutableDoubleBlankMap *
word_map_create_double_blank_sets(const MutableWordMap *word_map,
                                  const LetterDistribution *ld) {
  MutableDoubleBlankMap *double_blank_map =
      malloc_or_die(sizeof(MutableDoubleBlankMap));
  for (int i = 0; i <= BOARD_DIM; i++) {
    double_blank_map->maps[i] =
        double_blanks_for_same_length(&word_map->maps[i], i, ld);
  }
  return double_blank_map;
}

int mutable_double_blanks_for_same_length_get_num_sets(
    const MutableDoubleBlanksForSameLengthMap *map) {
  int num_sets = 0;
  for (uint32_t i = 0; i < map->num_double_blank_buckets; i++) {
    const MutableDoubleBlankMapBucket *bucket = &map->double_blank_buckets[i];
    num_sets += bucket->num_entries;
  }
  return num_sets;
}

double average_double_blank_sets_per_nonempty_bucket(
    const MutableDoubleBlanksForSameLengthMap *double_blank_map) {
  int num_buckets = 0;
  int num_sets = 0;
  for (uint32_t j = 0; j < double_blank_map->num_double_blank_buckets; j++) {
    const MutableDoubleBlankMapBucket *bucket =
        &double_blank_map->double_blank_buckets[j];
    if (bucket->num_entries > 0) {
      num_buckets++;
      num_sets += bucket->num_entries;
    }
  }
  return (double)num_sets / num_buckets;
}

int get_min_word_length(const MutableWordMap *word_map) {
  for (int i = 0; i <= BOARD_DIM; i++) {
    if (word_map->maps[i].num_word_buckets > 0) {
      return i;
    }
  }
  return INT_MAX;
}

int get_max_word_length(const MutableWordMap *word_map) {
  for (int i = BOARD_DIM; i >= 0; i--) {
    if (word_map->maps[i].num_word_buckets > 0) {
      return i;
    }
  }
  return -1;
}

uint32_t compute_min_num_buckets(const LetterDistribution *ld) {
  const BitRack bit_rack = largest_bit_rack_for_ld(ld);
  const uint64_t high64 = bit_rack_get_high64(&bit_rack);
  const int num_quotient_bits = WORD_MAP_QUOTIENT_BYTES * 8 - 1;
  const uint64_t max_quotient_high = (1ULL << (num_quotient_bits - 64)) - 1;
  const int divisor = (high64 / max_quotient_high) + 1;

  BitRack actual_quotient;
  uint32_t remainder;
  bit_rack_div_mod(&bit_rack, divisor, &actual_quotient, &remainder);
  const uint64_t actual_quotient_high = bit_rack_get_high64(&actual_quotient);
  assert(actual_quotient_high <= max_quotient_high);

  return next_prime(divisor);
}

void resize_words_of_same_length_map(const MutableWordsOfSameLengthMap *map,
                                     uint32_t min_num_buckets,
                                     MutableWordsOfSameLengthMap *new_map) {
  const int num_sets = mutable_words_of_same_length_get_num_sets(map);
  new_map->num_word_buckets = next_prime(num_sets * 2);
  if (new_map->num_word_buckets < min_num_buckets) {
    new_map->num_word_buckets = min_num_buckets;
  }
  new_map->word_buckets =
      malloc_or_die(sizeof(MutableWordMapBucket) * new_map->num_word_buckets);
  for (uint32_t i = 0; i < new_map->num_word_buckets; i++) {
    MutableWordMapBucket *bucket = &new_map->word_buckets[i];
    bucket->num_entries = 0;
    bucket->capacity = 1;
    bucket->entries =
        malloc_or_die(sizeof(MutableWordMapEntry) * bucket->capacity);
  }
  for (uint32_t i = 0; i < map->num_word_buckets; i++) {
    const MutableWordMapBucket *word_bucket = &map->word_buckets[i];
    for (uint32_t j = 0; j < word_bucket->num_entries; j++) {
      const MutableWordMapEntry *entry = &word_bucket->entries[j];
      BitRack quotient = entry->quotient;
      const DictionaryWordList *letters = entry->letters;
      for (int k = 0; k < dictionary_word_list_get_count(letters); k++) {
        const DictionaryWord *word = dictionary_word_list_get_word(letters, k);
        MutableWordMapBucket *bucket = &new_map->word_buckets[i];
        mutable_word_map_bucket_insert_word(bucket, quotient, word);
      }
    }
  }
}

MutableWordMap *mutable_word_map_resize(const MutableWordMap *word_map,
                                        uint32_t min_num_buckets) {
  MutableWordMap *new_word_map = malloc_or_die(sizeof(MutableWordMap));
  for (int i = 0; i <= BOARD_DIM; i++) {
    const MutableWordsOfSameLengthMap *map = &word_map->maps[i];
    resize_words_of_same_length_map(map, min_num_buckets,
                                    &new_word_map->maps[i]);
  }
  return new_word_map;
}

void resize_blanks_for_same_length_map(const MutableBlanksForSameLengthMap *map,
                                       uint32_t min_num_buckets,
                                       const LetterDistribution *ld,
                                       MutableBlanksForSameLengthMap *new_map) {
  const int num_sets = mutable_blanks_for_same_length_get_num_sets(map);
  new_map->num_blank_buckets = next_prime(num_sets * 2);
  if (new_map->num_blank_buckets < min_num_buckets) {
    new_map->num_blank_buckets = min_num_buckets;
  }
  new_map->blank_buckets =
      malloc_or_die(sizeof(MutableBlankMapBucket) * new_map->num_blank_buckets);
  for (uint32_t i = 0; i < new_map->num_blank_buckets; i++) {
    MutableBlankMapBucket *bucket = &new_map->blank_buckets[i];
    bucket->num_entries = 0;
    bucket->capacity = 1;
    bucket->entries =
        malloc_or_die(sizeof(MutableBlankMapEntry) * bucket->capacity);
  }
  for (uint32_t i = 0; i < map->num_blank_buckets; i++) {
    const MutableBlankMapBucket *blank_bucket = &map->blank_buckets[i];
    for (uint32_t entry_index = 0; entry_index < blank_bucket->num_entries;
         entry_index++) {
      const MutableBlankMapEntry *entry = &blank_bucket->entries[entry_index];
      BitRack full_bit_rack =
          bit_rack_mul(&entry->quotient, map->num_blank_buckets);
      bit_rack_add_uint32(&full_bit_rack, i);
      const Rack *rack = bit_rack_to_rack(&full_bit_rack, ld);
      set_blank_map_bits(new_map, rack, ld, entry->blank_letters);
    }
  }
}

MutableBlankMap *mutable_blank_map_resize(const MutableBlankMap *blank_map,
                                          const LetterDistribution *ld,
                                          uint32_t min_num_buckets) {
  MutableBlankMap *new_blank_map = malloc_or_die(sizeof(MutableBlankMap));
  for (int i = 0; i <= BOARD_DIM; i++) {
    const MutableBlanksForSameLengthMap *map = &blank_map->maps[i];
    resize_blanks_for_same_length_map(map, min_num_buckets, ld,
                                      &new_blank_map->maps[i]);
  }
  return new_blank_map;
}

void set_double_blank_map_letter_pairs(MutableDoubleBlanksForSameLengthMap *map,
                                       const Rack *word_rack,
                                       const LetterDistribution *ld,
                                       const DictionaryWordList *letter_pairs) {
  BitRack bit_rack = bit_rack_create_from_rack(ld, word_rack);
  BitRack quotient;
  uint32_t bucket_index;
  bit_rack_div_mod(&bit_rack, map->num_double_blank_buckets, &quotient,
                   &bucket_index);
  MutableDoubleBlankMapBucket *bucket =
      &map->double_blank_buckets[bucket_index];
  if (bucket->num_entries == bucket->capacity) {
    bucket->capacity *= 2;
    bucket->entries = realloc_or_die(
        bucket->entries, sizeof(MutableDoubleBlankMapEntry) * bucket->capacity);
  }
  MutableDoubleBlankMapEntry *entry = &bucket->entries[bucket->num_entries];
  entry->quotient = quotient;
  dictionary_word_list_copy(letter_pairs, &entry->letter_pairs);
  //printf("entry->letter_pairs %d\n", dictionary_word_list_get_count(entry->letter_pairs));
  bucket->num_entries++;
}

void resize_double_blanks_for_same_length_map(
    const MutableDoubleBlanksForSameLengthMap *map, uint32_t min_num_buckets,
    const LetterDistribution *ld,
    MutableDoubleBlanksForSameLengthMap *new_map) {
  const int num_sets = mutable_double_blanks_for_same_length_get_num_sets(map);
  new_map->num_double_blank_buckets = next_prime(num_sets * 2);
  if (new_map->num_double_blank_buckets < min_num_buckets) {
    new_map->num_double_blank_buckets = min_num_buckets;
  }
  new_map->double_blank_buckets = malloc_or_die(
      sizeof(MutableDoubleBlankMapBucket) * new_map->num_double_blank_buckets);
  for (uint32_t i = 0; i < new_map->num_double_blank_buckets; i++) {
    MutableDoubleBlankMapBucket *bucket = &new_map->double_blank_buckets[i];
    bucket->num_entries = 0;
    bucket->capacity = 1;
    bucket->entries =
        malloc_or_die(sizeof(MutableDoubleBlankMapEntry) * bucket->capacity);
  }
  for (uint32_t i = 0; i < map->num_double_blank_buckets; i++) {
    const MutableDoubleBlankMapBucket *double_blank_bucket =
        &map->double_blank_buckets[i];
    for (uint32_t entry_index = 0; entry_index < double_blank_bucket->num_entries;
         entry_index++) {
      const MutableDoubleBlankMapEntry *entry =
          &double_blank_bucket->entries[entry_index];
      BitRack full_bit_rack =
          bit_rack_mul(&entry->quotient, map->num_double_blank_buckets);
      bit_rack_add_uint32(&full_bit_rack, i);
      Rack *rack = bit_rack_to_rack(&full_bit_rack, ld);
      set_double_blank_map_letter_pairs(new_map, rack, ld, entry->letter_pairs);
    }
  }
}

MutableDoubleBlankMap *
mutable_double_blank_map_resize(const MutableDoubleBlankMap *double_blank_map,
                                const LetterDistribution *ld,
                                uint32_t min_num_buckets) {
  MutableDoubleBlankMap *new_double_blank_map =
      malloc_or_die(sizeof(MutableDoubleBlankMap));
  for (int i = 0; i <= BOARD_DIM; i++) {
    printf("resizing double blank map for length %d\n", i);
    const MutableDoubleBlanksForSameLengthMap *map = &double_blank_map->maps[i];
    resize_double_blanks_for_same_length_map(map, min_num_buckets, ld,
                                            &new_double_blank_map->maps[i]);
  }
  return new_double_blank_map;      
}

uint32_t
max_blank_pair_result_size(const MutableDoubleBlankMap *double_blank_map) {
  uint32_t max_size = 0;
  for (int len = 0; len <= BOARD_DIM; len++) {
    for (uint32_t bucket_index = 0;
         bucket_index < double_blank_map->maps[len].num_double_blank_buckets;
         bucket_index++) {
      const MutableDoubleBlankMapBucket *bucket =
          &double_blank_map->maps[len].double_blank_buckets[bucket_index];
      for (uint32_t entry_index = 0; entry_index < bucket->num_entries;
           entry_index++) {
        const MutableDoubleBlankMapEntry *entry = &bucket->entries[entry_index];
        const uint32_t size =
            2 * dictionary_word_list_get_count(entry->letter_pairs);
        if (size > max_size) {
          max_size = size;
          BitRack bit_rack = bit_rack_mul(
              &entry->quotient,
              double_blank_map->maps[len].num_double_blank_buckets);
          bit_rack_add_uint32(&bit_rack, bucket_index);
          bit_rack_set_letter_count(&bit_rack, BLANK_MACHINE_LETTER, 0);
          for (uint8_t ml = 0; ml <= 26; ml++) {
            for (int i = 0; i < bit_rack_get_letter(&bit_rack, ml); i++) {
              printf("%c", 'A' + ml - 1);
            }
          }
          printf(": %d\n", size);
        }
      }
    }
  }
  return max_size;
}

uint32_t get_number_of_words(const MutableWordsOfSameLengthMap *map,
                             const BitRack *bit_rack) {
  BitRack quotient;
  uint32_t bucket_index;
  bit_rack_div_mod(bit_rack, map->num_word_buckets, &quotient, &bucket_index);
  const MutableWordMapBucket *bucket = &map->word_buckets[bucket_index];
  for (uint32_t i = 0; i < bucket->num_entries; i++) {
    const MutableWordMapEntry *entry = &bucket->entries[i];
    if (bit_rack_equals(&entry->quotient, &quotient)) {
      return dictionary_word_list_get_count(entry->letters);
    }
  }
  return 0;
}

uint32_t number_of_double_blank_words(const MutableWordMap *word_map,
                                      int word_length,
                                      const BitRack *bit_rack_without_blanks,
                                      const DictionaryWordList *blank_pairs) {
  int num_words = 0;
  const MutableWordsOfSameLengthMap *map = &word_map->maps[word_length];
  for (int i = 0; i < dictionary_word_list_get_count(blank_pairs); i++) {
    BitRack bit_rack = *bit_rack_without_blanks;
    const DictionaryWord *blank_pair =
        dictionary_word_list_get_word(blank_pairs, i);
    const uint8_t ml1 = dictionary_word_get_word(blank_pair)[0];
    const uint8_t ml2 = dictionary_word_get_word(blank_pair)[1];
    bit_rack_add_letter(&bit_rack, ml1);
    bit_rack_add_letter(&bit_rack, ml2);
    num_words += get_number_of_words(map, &bit_rack);
  }
  return num_words;
}

uint32_t
max_word_lookup_result_size(const MutableWordMap *word_map,
                            const MutableDoubleBlankMap *double_blank_map) {
  uint32_t max_size = 0;
  for (int len = 15; len <= BOARD_DIM; len++) {
    for (uint32_t bucket_index = 0;
         bucket_index < double_blank_map->maps[len].num_double_blank_buckets;
         bucket_index++) {
      const MutableDoubleBlanksForSameLengthMap *map =
          &double_blank_map->maps[len];
      const MutableDoubleBlankMapBucket *bucket =
          &map->double_blank_buckets[bucket_index];
      for (uint32_t entry_index = 0; entry_index < bucket->num_entries;
           entry_index++) {
        const MutableDoubleBlankMapEntry *entry = &bucket->entries[entry_index];
        BitRack bit_rack =
            bit_rack_mul(&entry->quotient, map->num_double_blank_buckets);
        bit_rack_add_uint32(&bit_rack, bucket_index);
        bit_rack_set_letter_count(&bit_rack, BLANK_MACHINE_LETTER, 0);
        const uint32_t size =
            len * number_of_double_blank_words(word_map, len, &bit_rack,
                                               entry->letter_pairs);
        if (size > max_size) {
          max_size = size;
          for (uint8_t ml = 0; ml <= 26; ml++) {
            for (int i = 0; i < bit_rack_get_letter(&bit_rack, ml); i++) {
              printf("%c", 'A' + ml - 1);
            }
          }
          printf(": %d\n", size);
        }
      }
    }
  }
  return max_size;
}

WordMap *
word_map_create_from_mutables(const MutableWordMap *word_map,
                              const MutableBlankMap *blank_map,
                              const MutableDoubleBlankMap *double_blank_map) {
  assert(word_map != NULL);
  assert(blank_map != NULL);
  assert(double_blank_map != NULL);
  WordMap *map = malloc_or_die(sizeof(WordMap));
  map->major_version = WORD_MAP_MAJOR_VERSION;
  map->minor_version = WORD_MAP_MINOR_VERSION;
  map->min_word_length = get_min_word_length(word_map);
  map->max_word_length = get_max_word_length(word_map);
  map->max_blank_pair_bytes = max_blank_pair_result_size(double_blank_map);
  map->max_word_lookup_bytes =
      max_word_lookup_result_size(word_map, double_blank_map);
  return map;
}