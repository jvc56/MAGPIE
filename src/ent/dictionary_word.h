#ifndef DICTIONARY_WORD_H
#define DICTIONARY_WORD_H

#include "letter_distribution.h"
#include <stdint.h>

typedef struct DictionaryWord DictionaryWord;

const MachineLetter *
dictionary_word_get_word(const DictionaryWord *dictionary_word);
uint8_t dictionary_word_get_length(const DictionaryWord *dictionary_word);

typedef struct DictionaryWordList DictionaryWordList;

DictionaryWordList *dictionary_word_list_create(void);
DictionaryWordList *dictionary_word_list_create_with_capacity(int capacity);

void dictionary_word_list_clear(DictionaryWordList *dictionary_word_list);
void dictionary_word_list_add_word(DictionaryWordList *dictionary_word_list,
                                   const MachineLetter *word, int word_length);
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
