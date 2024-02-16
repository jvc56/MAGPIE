#include "dictionary_word.h"

#include <stdint.h>
#include <stdio.h>

#include "../def/dictionary_word_defs.h"
#include "../def/kwg_defs.h"

#include "../util/string_util.h"
#include "../util/util.h"

struct DictionaryWord {
  uint8_t *word;
  uint8_t length;
};

struct DictionaryWordList {
  DictionaryWord *dictionary_words;
  int count;
  int capacity;
  int max_word_length;
};

const uint8_t *dictionary_word_get_word(const DictionaryWord *dictionary_word) {
  return dictionary_word->word;
}

uint8_t dictionary_word_get_length(const DictionaryWord *dictionary_word) {
  return dictionary_word->length;
}

int dictionary_word_list_get_max_word_length(
    const DictionaryWordList *dictionary_word_list) {
  return dictionary_word_list->max_word_length;
}

DictionaryWordList *dictionary_word_list_create(int max_word_length) {
  DictionaryWordList *dictionary_word_list =
      malloc_or_die(sizeof(DictionaryWordList));
  dictionary_word_list->capacity = INITIAL_DICTIONARY_WORD_LIST_CAPACITY;
  dictionary_word_list->dictionary_words =
      malloc_or_die(sizeof(DictionaryWord) * dictionary_word_list->capacity);

  for (int i = 0; i < dictionary_word_list->capacity; i++) {
    dictionary_word_list->dictionary_words[i].word =
        malloc_or_die(sizeof(uint8_t) * max_word_length);
  }
  dictionary_word_list->max_word_length = max_word_length;

  dictionary_word_list->count = 0;
  return dictionary_word_list;
}

void dictionary_word_list_clear(DictionaryWordList *dictionary_word_list) {
  dictionary_word_list->count = 0;
}

void dictionary_word_list_add_word(DictionaryWordList *dictionary_word_list,
                                   const uint8_t *word, int word_length) {
  if (dictionary_word_list->count == dictionary_word_list->capacity) {
    dictionary_word_list->dictionary_words = realloc_or_die(
        dictionary_word_list->dictionary_words,
        sizeof(DictionaryWord) * dictionary_word_list->capacity * 2);
    for (int i = dictionary_word_list->capacity;
         i < dictionary_word_list->capacity * 2; i++) {
      dictionary_word_list->dictionary_words[i].word = malloc_or_die(
          sizeof(uint8_t) * dictionary_word_list->max_word_length);
    }
    dictionary_word_list->capacity *= 2;
  }

  DictionaryWord *dictionary_word =
      &dictionary_word_list->dictionary_words[dictionary_word_list->count];
  memory_copy(dictionary_word->word, word, word_length);
  dictionary_word->length = word_length;
  dictionary_word_list->count++;
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

int dictionary_word_compare(const void *a, const void *b) {
  const DictionaryWord *word_a = (const DictionaryWord *)a;
  const DictionaryWord *word_b = (const DictionaryWord *)b;
  const int length_a = dictionary_word_get_length(word_a);
  const int length_b = dictionary_word_get_length(word_b);

  // Compare the words lexicographically
  const int min_length = length_a < length_b ? length_a : length_b;
  for (int i = 0; i < min_length; i++) {
    if (dictionary_word_get_word(word_a)[i] <
        dictionary_word_get_word(word_b)[i]) {
      return -1;
    } else if (dictionary_word_get_word(word_a)[i] >
               dictionary_word_get_word(word_b)[i]) {
      return 1;
    }
  }

  // If the words are the same up to the length of the shorter word,
  // the shorter word is considered "less" than the longer one
  if (length_a < length_b) {
    return -1;
  } else if (length_a > length_b) {
    return 1;
  }
  return 0;
}

void dictionary_word_list_sort(DictionaryWordList *dictionary_word_list) {
  qsort(dictionary_word_list->dictionary_words, dictionary_word_list->count,
        sizeof(DictionaryWord), dictionary_word_compare);
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
  for (int i = 0; i < dictionary_word_list->capacity; i++) {
    free(dictionary_word_list->dictionary_words[i].word);
  }
  free(dictionary_word_list->dictionary_words);
  free(dictionary_word_list);
}