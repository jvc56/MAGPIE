#ifndef DICTIONARY_WORD_H
#define DICTIONARY_WORD_H

#include "../def/kwg_defs.h"
#include "../util/io_util.h"
#include "letter_distribution.h"
#include <stdint.h>

typedef struct DictionaryWord {
  MachineLetter word[MAX_KWG_STRING_LENGTH];
  uint8_t length;
} DictionaryWord;

typedef struct DictionaryWordList {
  DictionaryWord *dictionary_words;
  int count;
  int capacity;
} DictionaryWordList;

const MachineLetter *
dictionary_word_get_word(const DictionaryWord *dictionary_word);
uint8_t dictionary_word_get_length(const DictionaryWord *dictionary_word);

typedef struct DictionaryWordList DictionaryWordList;

DictionaryWordList *dictionary_word_list_create(void);
DictionaryWordList *dictionary_word_list_create_with_capacity(int capacity);

void dictionary_word_list_clear(DictionaryWordList *dictionary_word_list);

static inline void
dictionary_word_list_add_word(DictionaryWordList *dictionary_word_list,
                              const MachineLetter *word, int word_length) {
  if (dictionary_word_list->count == dictionary_word_list->capacity) {

    int new_capacity =
        dictionary_word_list->capacity ? dictionary_word_list->capacity * 2 : 1;
    dictionary_word_list->dictionary_words = (DictionaryWord *)realloc_or_die(
        dictionary_word_list->dictionary_words,
        sizeof(DictionaryWord) * (size_t)new_capacity);
    dictionary_word_list->capacity = new_capacity;
  }
  DictionaryWord *dictionary_word =
      &dictionary_word_list->dictionary_words[dictionary_word_list->count];
  memcpy(dictionary_word->word, word,
         (size_t)word_length * sizeof(MachineLetter));
  dictionary_word->length = (uint8_t)word_length;
  dictionary_word_list->count++;
}

int dictionary_word_list_get_count(
    const DictionaryWordList *dictionary_word_list);
DictionaryWord *
dictionary_word_list_get_word(const DictionaryWordList *dictionary_word_list,
                              int index);
void dictionary_word_list_sort(DictionaryWordList *dictionary_word_list);
void dictionary_word_list_unique(DictionaryWordList *sorted,
                                 DictionaryWordList *unique);
void dictionary_word_list_destroy(DictionaryWordList *dictionary_word_list);

void dictionary_word_list_write_to_file(
    const DictionaryWordList *dictionary_word_list,
    const LetterDistribution *ld, const char *data_paths,
    const char *output_name, ErrorStack *error_stack);

#endif // DICTIONARY_WORD_H
