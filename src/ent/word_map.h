#ifndef WORD_MAP_H
#define WORD_MAP_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../def/board_defs.h"
#include "bit_rack.h"
#include "dictionary_word.h"
#include "letter_distribution.h"

// WordMap binary format:
// 1 byte: major version number
// 1 byte: minor version number
// 
typedef struct WordsOfSameLengthMap {
  uint32_t *word_bucket_starts;
  uint32_t num_word_buckets;
  uint32_t *blank_bucket_starts;
  uint32_t num_blank_buckets;
  uint32_t *double_blank_bucket_starts;
  uint32_t double_num_blank_buckets;
  BitRack *word_map_entries;
  BitRack *blank_map_entries;
  BitRack *double_blank_map_entries;
  uint8_t *word_letters;
  uint8_t *double_blank_letters;
} WordsOfSameLengthMap;

typedef struct WordMap {
  WordsOfSameLengthMap maps[BOARD_DIM + 1];
} WordMap;

typedef struct MutableWordsMapEntry {
  BitRack quotient;
  DictionaryWordList *letters;
} MutableWordMapEntry;

typedef struct MutableWordMapBucket {
  MutableWordMapEntry *entries;
  uint32_t num_entries;
  uint32_t capacity;
} MutableWordMapBucket;

typedef struct MutableWordsOfSameLengthMap {
  MutableWordMapBucket *word_buckets;
  uint32_t num_word_buckets;
} MutableWordsOfSameLengthMap;

typedef struct MutableWordMap {
  MutableWordsOfSameLengthMap maps[BOARD_DIM + 1];
} MutableWordMap;

typedef struct MutableBlankMapEntry {
  BitRack quotient;
  uint32_t blank_letters;
} MutableBlankMapEntry;

typedef struct MutableBlankMapBucket {
  MutableBlankMapEntry *entries;
  uint32_t num_entries;
  uint32_t capacity;
} MutableBlankMapBucket;

typedef struct MutableBlanksForSameLengthMap {
  MutableBlankMapBucket *blank_buckets;
  uint32_t num_blank_buckets;
} MutableBlanksForSameLengthMap;

typedef struct MutableBlankMap {
  MutableBlanksForSameLengthMap maps[BOARD_DIM + 1];
} MutableBlankMap;

typedef struct MutableDoubleBlankMapEntry {
  BitRack quotient;

  // Stored as if they were two-letter words, not yet in any significant order.
  DictionaryWordList *letter_pairs;
} MutableDoubleBlankMapEntry;

typedef struct MutableDoubleBlankMapBucket {
  MutableDoubleBlankMapEntry *entries;
  uint32_t num_entries;
  uint32_t capacity;
} MutableDoubleBlankMapBucket;

typedef struct MutableDoubleBlanksForSameLengthMap {
  MutableDoubleBlankMapBucket *double_blank_buckets;
  uint32_t num_double_blank_buckets;
} MutableDoubleBlanksForSameLengthMap;

typedef struct MutableDoubleBlankMap {
  MutableDoubleBlanksForSameLengthMap maps[BOARD_DIM + 1];
} MutableDoubleBlankMap;

MutableWordMap *word_map_create_anagram_sets(const DictionaryWordList *words);

int mutable_words_of_same_length_get_num_sets(
    const MutableWordsOfSameLengthMap *map);

double average_word_sets_per_nonempty_bucket(
    const MutableWordsOfSameLengthMap *word_map);

MutableBlankMap *word_map_create_blank_sets(const MutableWordMap *word_map,
                                            const LetterDistribution *ld);

int mutable_blanks_for_same_length_get_num_sets(
    const MutableBlanksForSameLengthMap *map);

double average_blank_sets_per_nonempty_bucket(
    const MutableBlanksForSameLengthMap *blank_map);

MutableDoubleBlankMap *
word_map_create_double_blank_sets(const MutableWordMap *word_map,
                                  const LetterDistribution *ld);

int mutable_double_blanks_for_same_length_get_num_sets(
    const MutableDoubleBlanksForSameLengthMap *map);

double average_double_blank_sets_per_nonempty_bucket(
    const MutableDoubleBlanksForSameLengthMap *double_blank_map);                                  

#endif