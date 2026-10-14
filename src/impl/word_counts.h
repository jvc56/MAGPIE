#ifndef WORD_COUNTS_H
#define WORD_COUNTS_H

#include "../ent/board.h"
#include "../util/io_util.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
  char word[16];
  uint64_t count;
} WordCount;

typedef struct {
  WordCount *entries;
  unsigned size;
} WordCounts;

WordCounts *word_counts_create(void);
void word_counts_destroy(WordCounts *counts);
bool word_counts_add(WordCounts *counts, const char *word, uint64_t count);
// Count newly formed maximal words, including crosses; ignore unchanged words.
bool word_counts_record(WordCounts *counts, const Board *board,
                        const unsigned char before[225]);
bool word_counts_merge(WordCounts *counts, const char *filename,
                       ErrorStack *errors);
bool word_counts_write(const WordCounts *counts, const char *filename,
                       ErrorStack *errors);

#endif
