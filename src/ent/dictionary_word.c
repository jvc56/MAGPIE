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

  int cmp_result = memcmp(word_a->word, word_b->word, min_length * sizeof(MachineLetter));
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

static inline void dict_quicksort(DictionaryWord *arr, int left, int right) {
  while (left < right) {
    int i = left;
    int j = right;
    // Not worrying about overflow, these are 64-bit integers and the size will never be more than ~millions.
     int mid = (left + right) / 2;
    DictionaryWord tmp; // Used for all swaps

    // Median-of-three
    if (dictionary_word_compare(&arr[left], &arr[mid]) > 0) {
      tmp = arr[left];
      arr[left] = arr[mid];
      arr[mid] = tmp;
    }
    if (dictionary_word_compare(&arr[left], &arr[right]) > 0) {
      tmp = arr[left];
      arr[left] = arr[right];
      arr[right] = tmp;
    }
    if (dictionary_word_compare(&arr[mid], &arr[right]) > 0) {
      tmp = arr[mid];
      arr[mid] = arr[right];
      arr[right] = tmp;
    }

    DictionaryWord pivot = arr[mid];

    while (i <= j) {
      while (dictionary_word_compare(&arr[i], &pivot) < 0) {
        i++;
      }
      while (dictionary_word_compare(&arr[j], &pivot) > 0) {
        j--;
      }
      if (i <= j) {
        tmp = arr[i];
        arr[i] = arr[j];
        arr[j] = tmp;
        i++;
        j--;
      }
    }

    if (j - left < right - i) {
      if (left < j) {
        dict_quicksort(arr, left, j);
      }
      left = i;
    } else {
      if (i < right) {
        dict_quicksort(arr, i, right);
      }
      right = j;
    }
  }
}

void dictionary_word_list_sort(DictionaryWordList *dictionary_word_list) {
  if (dictionary_word_list->count <= 1) {
    return;
  }
  dict_quicksort(dictionary_word_list->dictionary_words, 0,
                 dictionary_word_list->count - 1);
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
