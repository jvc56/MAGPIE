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

static inline int dictionary_word_compare(const DictionaryWord *word_a,
                                          const DictionaryWord *word_b) {
  const int length_a = word_a->length;
  const int length_b = word_b->length;
  const int min_length = length_a < length_b ? length_a : length_b;

  int cmp_result =
      memcmp(word_a->word, word_b->word, min_length * sizeof(MachineLetter));
  if (cmp_result != 0) {
    return cmp_result;
  }

  // If the words are the same up to the length of the shorter word,
  // the shorter word is considered "less" than the longer one
  if (length_a < length_b) {
    return -1;
  }
  if (length_a > length_b) {
    return 1;
  }
  return 0;
}

static inline void insertion_sort(DictionaryWord *arr, int left, int right) {
  for (int i = left + 1; i <= right; i++) {
    DictionaryWord tmp = arr[i];
    int j = i - 1;
    while (j >= left && dictionary_word_compare(&arr[j], &tmp) > 0) {
      arr[j + 1] = arr[j];
      j--;
    }
    arr[j + 1] = tmp;
  }
}

// Merge two sorted subarrays into a single sorted array using a buffer
static inline void merge(DictionaryWord *arr, int left, int mid, int right,
                         DictionaryWord *temp) {
  int n1 = mid - left + 1;
  int n2 = right - mid;

  // Copy data to temporary arrays in temp[]
  for (int i = 0; i < n1; i++) {
    temp[i] = arr[left + i];
  }
  for (int j = 0; j < n2; j++) {
    temp[n1 + j] = arr[mid + 1 + j];
  }

  // Merge the temp arrays back into arr[left..right]
  int i = 0;
  int j = n1;
  int k = left;
  while (i < n1 && j < n1 + n2) {
    if (dictionary_word_compare(&temp[i], &temp[j]) <= 0) {
      arr[k++] = temp[i++];
    } else {
      arr[k++] = temp[j++];
    }
  }

  // Copy any remaining elements of L[] (if any)
  while (i < n1) {
    arr[k++] = temp[i++];
  }

  // Copy any remaining elements of R[] (if any)
  while (j < n1 + n2) {
    arr[k++] = temp[j++];
  }
}

static inline void tim_sort(DictionaryWord *arr, int n) {
  // Step 1: Sort individual runs using insertion sort
  for (int i = 0; i < n; i += DICTIONARY_TIM_SORT_MIN_RUN_LENGTH) {
    int right = (i + DICTIONARY_TIM_SORT_MIN_RUN_LENGTH - 1 < n)
                    ? i + DICTIONARY_TIM_SORT_MIN_RUN_LENGTH - 1
                    : n - 1;
    insertion_sort(arr, i, right);
  }

  // Step 2: Allocate a temporary array to be reused in the merging phase
  DictionaryWord *temp =
      (DictionaryWord *)malloc_or_die(sizeof(DictionaryWord) * n);

  // Step 3: Start merging runs
  for (int size = DICTIONARY_TIM_SORT_MIN_RUN_LENGTH; size < n;
       size = 2 * size) {
    // Merge runs in pairs of size 'size'
    for (int left = 0; left < n; left += 2 * size) {
      // Find the right index for merging
      int mid = (left + size - 1 < n) ? left + size - 1 : n - 1;
      int right = (left + 2 * size - 1 < n) ? left + 2 * size - 1 : n - 1;

      // Merge the two subarrays arr[left..mid] and arr[mid+1..right]
      if (mid < right) {
        merge(arr, left, mid, right, temp);
      }
    }
  }

  // Free the temporary array after use
  free(temp);
}

void dictionary_word_list_sort(DictionaryWordList *dictionary_word_list) {
  tim_sort(dictionary_word_list->dictionary_words, dictionary_word_list->count);
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
