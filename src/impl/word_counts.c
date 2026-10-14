#include "word_counts.h"

#include "../def/letter_distribution_defs.h"
#include "../ent/board.h"
#include "../ent/letter_distribution.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "../util/xs_bits.h"
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { WORD_COUNTS_CAPACITY = 1 << 20, WORD_COUNTS_LIMIT = 300000 };

WordCounts *word_counts_create(void) {
  WordCounts *counts = calloc_or_die(1, sizeof(*counts));
  counts->entries = calloc_or_die(WORD_COUNTS_CAPACITY, sizeof(WordCount));
  return counts;
}

void word_counts_destroy(WordCounts *counts) {
  if (counts) {
    free(counts->entries);
    free(counts);
  }
}

bool word_counts_add(WordCounts *counts, const char *word, uint64_t count) {
  size_t length = strlen(word);
  if (length < 2 || length > 15 || !count) {
    return false;
  }
  uint32_t hash = 2166136261U;
  for (size_t index = 0; index < length; index++) {
    if (word[index] < 'A' || word[index] > 'Z') {
      return false;
    }
    hash = (hash ^ (unsigned char)word[index]) * 16777619U;
  }
  unsigned slot = hash & (WORD_COUNTS_CAPACITY - 1);
  while (counts->entries[slot].word[0] &&
         strcmp(counts->entries[slot].word, word) != 0) {
    slot = (slot + 1) & (WORD_COUNTS_CAPACITY - 1);
  }
  WordCount *entry = &counts->entries[slot];
  if (count > UINT64_MAX - entry->count ||
      (!entry->count && counts->size == WORD_COUNTS_LIMIT)) {
    return false;
  }
  if (!entry->count) {
    counts->size++;
    memcpy(entry->word, word, length + 1);
  }
  entry->count += count;
  return true;
}

bool word_counts_record(WordCounts *counts, const Board *board,
                        const unsigned char before[225]) {
  for (int direction = 0; direction < 2; direction++) {
    for (int line = 0; line < 15; line++) {
      char word[16];
      int length = 0;
      bool changed = false;
      for (int offset = 0; offset <= 15; offset++) {
        int row = direction ? offset : line;
        int col = direction ? line : offset;
        MachineLetter letter =
            offset < 15 ? board_get_letter(board, row, col) : 0;
        if (letter) {
          word[length++] =
              (char)('A' + get_unblanked_machine_letter(letter) - 1);
          changed |= !before[(row * 15) + col];
        } else {
          if (length > 1 && changed) {
            word[length] = '\0';
            if (!word_counts_add(counts, word, 1)) {
              return false;
            }
          }
          length = 0;
          changed = false;
        }
      }
    }
  }
  return true;
}

bool word_counts_merge(WordCounts *counts, const char *filename,
                       ErrorStack *errors) {
  size_t length;
  char *text = xs_read_file(filename, &length, errors);
  if (!text) {
    return false;
  }
  bool valid = strlen(text) == length;
  StringSplitter *lines = split_string_by_newline(text, true);
  for (int index = 0;
       valid && index < string_splitter_get_number_of_items(lines); index++) {
    StringSplitter *fields =
        split_string(string_splitter_get_item(lines, index), '\t', false);
    valid = string_splitter_get_number_of_items(fields) == 2;
    if (valid) {
      const char *frequency = string_splitter_get_item(fields, 1);
      char *end = NULL;
      errno = 0;
      unsigned long long count = strtoull(frequency, &end, 10);
      valid =
          *frequency >= '0' && *frequency <= '9' && !errno && end && !*end &&
          word_counts_add(counts, string_splitter_get_item(fields, 0), count);
    }
    string_splitter_destroy(fields);
  }
  string_splitter_destroy(lines);
  free(text);
  if (!valid) {
    xs_error(errors, "invalid count shard or frequency overflow");
  }
  return valid;
}

static int word_counts_compare(const void *left, const void *right) {
  return strcmp(((const WordCount *)left)->word,
                ((const WordCount *)right)->word);
}

bool word_counts_write(const WordCounts *counts, const char *filename,
                       ErrorStack *errors) {
  WordCount *sorted = malloc_or_die((counts->size + 1) * sizeof(WordCount));
  unsigned size = 0;
  for (unsigned slot = 0; slot < WORD_COUNTS_CAPACITY; slot++) {
    if (counts->entries[slot].count) {
      sorted[size++] = counts->entries[slot];
    }
  }
  qsort(sorted, size, sizeof(WordCount), word_counts_compare);
  StringBuilder *output = string_builder_create();
  for (unsigned index = 0; index < size; index++) {
    string_builder_add_formatted_string(
        output, "%s\t%llu\n", sorted[index].word,
        (unsigned long long)sorted[index].count);
  }
  bool valid = xs_write_file(filename, string_builder_peek(output),
                             string_builder_length(output), errors);
  string_builder_destroy(output);
  free(sorted);
  return valid;
}
