#ifndef WORD_MAP_H
#define WORD_MAP_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../def/board_defs.h"
#include "../def/word_map_defs.h"
#include "bit_rack.h"
#include "dictionary_word.h"
#include "letter_distribution.h"

// WordMap binary format:
// ======================
// 1 byte: major version number
// 1 byte: minor version number
// 1 byte: minimum word length
// 1 byte: maximum word length
// Use the following either to dynamically allocate buffers for intermediate
// and final results, or to validate that sstatically allocated buffers are
// large enough.
// 4 bytes: maximum size in bytes of blank pair results
// 4 bytes: maximum size in bytes of word lookup results
// xxxxxx: repeated WordOfSameLengthMap binary data

// WordOfSameLengthMap binary format:
// ==================================
// 4 bytes: number of word buckets
// num_word_buckets * 4 bytes: word bucket starts
// 4 bytes: number of word entries
// 28 * number_of_word_entries bytes: word entries
// 4 bytes: number of words
// num_words * word_length bytes: uint8_t mls of words
// ----------------------------------
// 4 bytes: number of blank buckets
// num_blank_buckets * 4 bytes: blank bucket starts
// 4 bytes: number of blank entries
// 28 * number_of_blank_entries bytes: blank entries
// ----------------------------------
// 4 bytes: number of double blank buckets
// double_num_blank_buckets * 4 bytes: double blank bucket starts
// 4 bytes: number of double blank entries
// 28 * number_of_double_blank_entries bytes: double blank entries
// 4 bytes: number of blank letter pairs
// num_blank_letter_pairs * 2 bytes: uint8_t mls of blank letter pairs

// WordMapEntry binary format:
// ===========================
// 16 bytes: either word bucket index (last 4 bytes) or inline words
// 12 bytes: isInline bit | BitRack quotient (96 bits)
//           (number of word buckets must be high enough that maximum quotient
//           fits. largest_bit_rack_for_ld(ld) / num_word_buckets < (1 << 95)).

typedef struct WordEntry {
  uint8_t bucket_or_inline[WORD_MAP_INLINE_VALUE_BYTES];
  uint8_t quotient[WORD_MAP_QUOTIENT_BYTES];
} WordEntry;

typedef struct WordsOfSameLengthMap {
  uint32_t *word_bucket_starts;
  uint32_t num_word_buckets;
  uint32_t *blank_bucket_starts;
  uint32_t num_blank_buckets;
  uint32_t *double_blank_bucket_starts;
  uint32_t num_double_blank_buckets;
  uint32_t num_word_entries;
  WordEntry *word_map_entries;
  uint32_t num_blank_entries;
  WordEntry *blank_map_entries;
  uint32_t num_double_blank_entries;
  WordEntry *double_blank_map_entries;
  uint32_t num_words;
  uint8_t *word_letters;
  uint32_t num_blank_pairs;
  uint8_t *double_blank_letters;
} WordsOfSameLengthMap;

typedef struct WordMap {
  char *name;
  uint8_t major_version;
  uint8_t minor_version;
  uint8_t min_word_length;
  uint8_t max_word_length;
  uint32_t max_blank_pair_bytes;
  uint32_t max_word_lookup_bytes;
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

uint32_t compute_min_num_buckets(const LetterDistribution *ld);

MutableWordMap *mutable_word_map_resize(const MutableWordMap *word_map,
                                        uint32_t min_num_buckets);

MutableBlankMap *mutable_blank_map_resize(const MutableBlankMap *blank_map,
                                          const LetterDistribution *ld,
                                          uint32_t min_num_buckets);

MutableDoubleBlankMap *
mutable_double_blank_map_resize(const MutableDoubleBlankMap *double_blank_map,
                                const LetterDistribution *ld,
                                uint32_t min_num_buckets);

WordMap *
word_map_create_from_mutables(const MutableWordMap *word_map,
                              const MutableBlankMap *blank_map,
                              const MutableDoubleBlankMap *double_blank_map);

void word_map_destroy(WordMap *word_map);

int word_map_write_blankless_words_to_buffer(const WordMap *word_map,
                                             const BitRack *bit_rack,
                                             uint8_t *buffer);

int word_map_write_blanks_to_buffer(const WordMap *word_map, BitRack *bit_rack,
                                    uint8_t *buffer);

int word_map_write_double_blanks_to_buffer(const WordMap *word_map,
                                           BitRack *bit_rack,
                                           uint8_t *buffer);

int word_map_write_words_to_buffer(const WordMap *word_map, BitRack *bit_rack,
                                   uint8_t *buffer);

WordMap *
word_map_create_from_dictionary_word_list(const LetterDistribution *ld,
                                          const DictionaryWordList *words);

bool word_map_write_to_file(const WordMap *word_map, const char *filename);

void load_word_map(WordMap *wmp, const char *data_paths,
                   const char *wmp_name);

#endif