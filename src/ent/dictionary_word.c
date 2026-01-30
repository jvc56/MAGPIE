#include "dictionary_word.h"

#include "../def/dictionary_word_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../ent/data_filepaths.h"
#include "../ent/letter_distribution.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

const MachineLetter *
dictionary_word_get_word(const DictionaryWord *dictionary_word) {
  return dictionary_word->word;
}

uint8_t dictionary_word_get_length(const DictionaryWord *dictionary_word) {
  return dictionary_word->length;
}

DictionaryWordList *dictionary_word_list_create(void) {
  return dictionary_word_list_create_with_capacity(
      INITIAL_DICTIONARY_WORD_LIST_CAPACITY);
}

DictionaryWordList *dictionary_word_list_create_with_capacity(int capacity) {
  DictionaryWordList *dictionary_word_list =
      malloc_or_die(sizeof(DictionaryWordList));
  dictionary_word_list->capacity = capacity;
  dictionary_word_list->dictionary_words =
      malloc_or_die(sizeof(DictionaryWord) * dictionary_word_list->capacity);
  dictionary_word_list->count = 0;
  return dictionary_word_list;
}

void dictionary_word_list_clear(DictionaryWordList *dictionary_word_list) {
  dictionary_word_list->count = 0;
}

int dictionary_word_list_get_count(
    const DictionaryWordList *dictionary_word_list) {
  return dictionary_word_list->count;
}

DictionaryWord *
dictionary_word_list_get_word(const DictionaryWordList *dictionary_word_list,
                              int index) {
  return &dictionary_word_list->dictionary_words[index];
}

// Compare two dictionary words lexicographically
static inline int dictionary_word_compare(const DictionaryWord *word_a,
                                          const DictionaryWord *word_b) {
  const int len_a = word_a->length;
  const int len_b = word_b->length;
  const int min_len = len_a < len_b ? len_a : len_b;

  int cmp = memcmp(word_a->word, word_b->word, min_len);
  if (cmp != 0) {
    return cmp;
  }
  return len_a - len_b;
}

// LSD Radix Sort for DictionaryWordList
// Optimized for machine letters (small integers)
// Uses stable counting sort at each position, processing right to left
// Variable-length strings are handled by treating positions past the end as -1

// Alphabet size for radix sort - must cover all machine letters (0 to
// MAX_ALPHABET_SIZE) For most languages: 27 (English), 33 (Polish), etc. We use
// MAX_ALPHABET_SIZE + 1 to cover separator (0) and all letters (1 to 50).
#define RADIX_SIZE (MAX_ALPHABET_SIZE + 1)

// Get the sort key for a character at a given position
// Returns 0 for positions past the string end (sorts before all letters)
// Returns letter + 1 for actual letters (so they sort after end-of-string)
static inline int get_radix_key(const DictionaryWord *word, int pos) {
  if (pos >= word->length) {
    return 0; // End-of-string marker, sorts first
  }
  return word->word[pos] + 1;
}

// Single pass of counting sort on position 'pos'
// Uses double buffering: reads from src, writes to dst
static inline void radix_sort_pass(const DictionaryWord *src, DictionaryWord *dst,
                                   int n, int pos, int *count) {
  // Count phase - count occurrences of each key
  memset(count, 0, (RADIX_SIZE + 2) * sizeof(int));
  for (int i = 0; i < n; i++) {
    int key = get_radix_key(&src[i], pos);
    count[key + 1]++;
  }

  // Prefix sum - convert counts to starting positions
  for (int r = 0; r < RADIX_SIZE; r++) {
    count[r + 1] += count[r];
  }

  // Distribute phase - place elements in sorted order
  for (int i = 0; i < n; i++) {
    int key = get_radix_key(&src[i], pos);
    dst[count[key]++] = src[i];
  }
}

// LSD Radix Sort - O(n * max_length) time complexity
// Much faster than O(n * log(n) * avg_length) comparison sort for small alphabets
static inline void radix_sort(DictionaryWord *arr, int n) {
  if (n <= 1) {
    return;
  }

  // Find maximum string length
  int max_len = 0;
  for (int i = 0; i < n; i++) {
    if (arr[i].length > max_len) {
      max_len = arr[i].length;
    }
  }

  if (max_len == 0) {
    return;
  }

  // Allocate temp array for double buffering
  DictionaryWord *temp = malloc_or_die(sizeof(DictionaryWord) * (size_t)n);
  // Need RADIX_SIZE + 2 because count phase uses count[key + 1] and max key is RADIX_SIZE
  int count[RADIX_SIZE + 2];

  // LSD: process from rightmost position to leftmost
  // Double buffering: alternate between arr and temp to avoid copying
  DictionaryWord *src = arr;
  DictionaryWord *dst = temp;

  for (int pos = max_len - 1; pos >= 0; pos--) {
    radix_sort_pass(src, dst, n, pos, count);
    // Swap buffers for next pass
    DictionaryWord *swap = src;
    src = dst;
    dst = swap;
  }

  // If final result is in temp, copy back to arr
  if (src != arr) {
    memcpy(arr, src, sizeof(DictionaryWord) * (size_t)n);
  }

  free(temp);
}

void dictionary_word_list_sort(DictionaryWordList *dictionary_word_list) {
  if (dictionary_word_list->count <= 1) {
    return;
  }
  radix_sort(dictionary_word_list->dictionary_words,
             dictionary_word_list->count);
}

void dictionary_word_list_unique(DictionaryWordList *sorted,
                                 DictionaryWordList *unique) {
  for (int i = 0; i < sorted->count; i++) {
    if (i == 0 ||
        dictionary_word_compare(&sorted->dictionary_words[i],
                                &sorted->dictionary_words[i - 1]) != 0) {
      dictionary_word_list_add_word(unique, sorted->dictionary_words[i].word,
                                    sorted->dictionary_words[i].length);
    }
  }
}

void dictionary_word_list_destroy(DictionaryWordList *dictionary_word_list) {
  free(dictionary_word_list->dictionary_words);
  free(dictionary_word_list);
}

void dictionary_word_list_write_to_file(
    const DictionaryWordList *dictionary_word_list,
    const LetterDistribution *ld, const char *data_paths,
    const char *output_name, ErrorStack *error_stack) {
  char *filename = data_filepaths_get_writable_filename(
      data_paths, output_name, DATA_FILEPATH_TYPE_LEXICON, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
  StringBuilder *sb = string_builder_create();
  for (int word_idx = 0; word_idx < dictionary_word_list->count; word_idx++) {
    const DictionaryWord *word =
        &dictionary_word_list->dictionary_words[word_idx];
    for (int letter_idx = 0; letter_idx < word->length; letter_idx++) {
      char *hl = ld_ml_to_hl(ld, word->word[letter_idx]);
      string_builder_add_string(sb, hl);
      free(hl);
    }
    string_builder_add_string(sb, "\n");
  }
  write_string_to_file(filename, "w", string_builder_peek(sb), error_stack);
  string_builder_destroy(sb);
  free(filename);
}
