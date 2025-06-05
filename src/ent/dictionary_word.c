#include "dictionary_word.h"

#include <stdint.h>
#include <stdio.h>

#include "../def/dictionary_word_defs.h"
#include "../def/kwg_defs.h"

#include "../ent/letter_distribution.h"

#include "../util/io_util.h"
#include "../util/string_util.h"

struct DictionaryWord {
  uint8_t word[MAX_KWG_STRING_LENGTH];
  uint8_t length;
};

struct DictionaryWordList {
  DictionaryWord *dictionary_words;
  int count;
  int capacity;
};

const uint8_t *dictionary_word_get_word(const DictionaryWord *dictionary_word) {
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

void dictionary_word_list_add_word(DictionaryWordList *dictionary_word_list,
                                   const uint8_t *word, int word_length) {
  if (dictionary_word_list->count == dictionary_word_list->capacity) {
    dictionary_word_list->dictionary_words = realloc_or_die(
        dictionary_word_list->dictionary_words,
        sizeof(DictionaryWord) * dictionary_word_list->capacity * 2);
    dictionary_word_list->capacity *= 2;
  }
  DictionaryWord *dictionary_word =
      &dictionary_word_list->dictionary_words[dictionary_word_list->count];
  memcpy(dictionary_word->word, word, word_length);
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

void dictionary_word_list_copy(const DictionaryWordList *src,
                               DictionaryWordList **dst) {
  *dst = dictionary_word_list_create_with_capacity(src->count);
  for (int i = 0; i < src->count; i++) {
    dictionary_word_list_add_word(*dst, src->dictionary_words[i].word,
                                  src->dictionary_words[i].length);
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
