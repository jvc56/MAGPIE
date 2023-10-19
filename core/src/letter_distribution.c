#include <assert.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fileproxy.h"
#include "letter_distribution.h"
#include "log.h"
#include "string_util.h"
#include "util.h"

#define INVALID_LETTER (0x80 - 1)

extern inline uint8_t get_blanked_machine_letter(uint8_t ml);
extern inline uint8_t get_unblanked_machine_letter(uint8_t ml);
extern inline uint8_t is_blanked(uint8_t ml);

int get_letter_distribution_size(const char *filename) {
  FILE *file = stream_from_filename(filename);
  if (!file) {
    log_fatal("Error opening file to count lines: %s\n", filename);
  }

  char line[100];
  int letter_distribution_size = 0;
  while (fgets(line, sizeof(line), file)) {
    if (!is_all_whitespace_or_empty(line)) {
      letter_distribution_size++;
    }
  }
  fclose(file);
  return letter_distribution_size;
}

char *get_letter_distribution_filepath(const char *ld_name) {
  // Check for invalid inputs
  if (!ld_name) {
    log_fatal("letter distribution name is null");
  }
  return get_formatted_string("%s%s%s", LETTER_DISTRIBUTION_FILEPATH, ld_name,
                              LETTER_DISTRIBUTION_FILE_EXTENSION);
}

void load_letter_distribution(LetterDistribution *letter_distribution,
                              const char *ld_name) {
  // This function call opens and closes the file, so
  // call it before the fopen to prevent a nested file read
  char *letter_distribution_filename =
      get_letter_distribution_filepath(ld_name);

  letter_distribution->size =
      get_letter_distribution_size(letter_distribution_filename);

  FILE *file = stream_from_filename(letter_distribution_filename);
  if (!file) {
    log_fatal("Error opening letter distribution file: %s\n",
              letter_distribution_filename);
  }
  free(letter_distribution_filename);

  letter_distribution->distribution =
      (uint32_t *)malloc_or_die(letter_distribution->size * sizeof(uint32_t));
  letter_distribution->scores =
      (uint32_t *)malloc_or_die(letter_distribution->size * sizeof(uint32_t));
  letter_distribution->score_order =
      (uint32_t *)malloc_or_die(letter_distribution->size * sizeof(uint32_t));
  letter_distribution->is_vowel =
      (uint32_t *)malloc_or_die(letter_distribution->size * sizeof(uint32_t));

  for (int i = 0; i < MACHINE_LETTER_MAX_VALUE; i++) {
    letter_distribution->machine_letter_to_human_readable_letter[i][0] = '\0';
  }

  int machine_letter = 0;
  char line[100];
  int max_tile_length = 0;
  while (fgets(line, sizeof(line), file)) {
    if (is_all_whitespace_or_empty(line)) {
      continue;
    }
    StringSplitter *single_letter_info = split_string(line, ',', true);
    // letter, lower case, dist, score, is_vowel
    char *letter = string_splitter_get_item(single_letter_info, 0);
    char *lower_case_letter = string_splitter_get_item(single_letter_info, 1);
    int dist = string_to_int(string_splitter_get_item(single_letter_info, 2));
    int score = string_to_int(string_splitter_get_item(single_letter_info, 3));
    int is_vowel =
        string_to_int(string_splitter_get_item(single_letter_info, 4));

    int tile_length = string_length(letter);
    if (tile_length > max_tile_length) {
      max_tile_length = tile_length;
    }

    letter_distribution->distribution[machine_letter] = dist;
    letter_distribution->scores[machine_letter] = score;
    letter_distribution->is_vowel[machine_letter] = is_vowel;

    string_copy(letter_distribution
                    ->machine_letter_to_human_readable_letter[machine_letter],
                letter);

    if (machine_letter > 0) {
      uint8_t blanked_machine_letter =
          get_blanked_machine_letter(machine_letter);
      string_copy(
          letter_distribution
              ->machine_letter_to_human_readable_letter[blanked_machine_letter],
          lower_case_letter);
    }

    int i = machine_letter;
    for (;
         i > 0 &&
         (int)letter_distribution
                 ->scores[(int)letter_distribution->score_order[i - 1]] < score;
         i--) {
      letter_distribution->score_order[i] =
          letter_distribution->score_order[i - 1];
    }
    letter_distribution->score_order[i] = machine_letter;
    destroy_string_splitter(single_letter_info);
    machine_letter++;
  }
  letter_distribution->max_tile_length = max_tile_length;
  fclose(file);
}

// This is a linear search. This function should not be used for anything
// that is speed-critical. If we ever need to use this in anything
// speed-critical, we should use a hash.
uint8_t
human_readable_letter_to_machine_letter(LetterDistribution *letter_distribution,
                                        char *letter) {
  for (int i = 0; i < MACHINE_LETTER_MAX_VALUE; i++) {
    if (strings_equal(
            letter_distribution->machine_letter_to_human_readable_letter[i],
            letter)) {
      return i;
    }
  }
  return INVALID_LETTER;
}

// Convert a string of arbitrary characters into an array of machine letters,
// returning the number of machine letters. This function does not allocate
// the ml array; it is the caller's responsibility to make this array big
// enough.
// Note: This is a slow function that should not be used in any hot loops.
int str_to_machine_letters(LetterDistribution *letter_distribution,
                           const char *str, bool allow_played_through_marker,
                           uint8_t *mls) {

  int num_mls = 0;
  int num_bytes = string_length(str);
  int i = 0;
  int prev_i = -1;
  while (i < num_bytes) {
    for (int j = i + letter_distribution->max_tile_length; j > i; j--) {
      if (j > num_bytes) {
        continue;
      }
      // possible letter goes from index i to j. Search for it.
      char possible_letter[MAX_LETTER_CHAR_LENGTH];
      memory_copy(possible_letter, str + i, j - i);
      possible_letter[j - i] = '\0';
      uint8_t ml = human_readable_letter_to_machine_letter(letter_distribution,
                                                           possible_letter);
      if (ml == INVALID_LETTER) {
        if (j - i == 1 && allow_played_through_marker &&
            possible_letter[0] == ASCII_PLAYED_THROUGH) {
          ml = PLAYED_THROUGH_MARKER;
        } else {
          continue;
        }
      }
      // Otherwise, we found the letter we're looking for
      mls[num_mls] = ml;
      num_mls++;
      i = j;
    }
    if (i == prev_i) {
      // Search is not finding any valid machine letters
      // and is not making progress. Return now with -1
      // to signify an error to avoid an infinite loop.
      return -1;
    }
    prev_i = i;
  }
  return num_mls;
}

LetterDistribution *create_letter_distribution(const char *ld_name) {
  LetterDistribution *letter_distribution =
      malloc_or_die(sizeof(LetterDistribution));
  load_letter_distribution(letter_distribution, ld_name);
  return letter_distribution;
}

void destroy_letter_distribution(LetterDistribution *letter_distribution) {
  free(letter_distribution->distribution);
  free(letter_distribution->scores);
  free(letter_distribution->is_vowel);
  free(letter_distribution->score_order);
  free(letter_distribution);
}

char *get_default_letter_distribution_name(const char *lexicon_name) {
  char *ld_name = NULL;
  if (has_prefix("CSW", lexicon_name) || has_prefix("NWL", lexicon_name) ||
      has_prefix("TWL", lexicon_name) || has_prefix("America", lexicon_name)) {
    ld_name = get_formatted_string("%s", ENGLISH_LETTER_DISTRIBUTION_NAME);
  } else if (has_prefix("RD", lexicon_name)) {
    ld_name = get_formatted_string("%s", GERMAN_LETTER_DISTRIBUTION_NAME);
  } else if (has_prefix("NSF", lexicon_name)) {
    ld_name = get_formatted_string("%s", NORWEGIAN_LETTER_DISTRIBUTION_NAME);
  } else if (has_prefix("DISC", lexicon_name)) {
    ld_name = get_formatted_string("%s", CATALAN_LETTER_DISTRIBUTION_NAME);
  } else if (has_prefix("FRA", lexicon_name)) {
    ld_name = get_formatted_string("%s", FRENCH_LETTER_DISTRIBUTION_NAME);
  } else if (has_prefix("OSPS", lexicon_name)) {
    ld_name = get_formatted_string("%s", POLISH_LETTER_DISTRIBUTION_NAME);
  } else {
    log_fatal("default letter distribution not found for lexicon %s\n",
              lexicon_name);
  }
  return ld_name;
}

void string_builder_add_user_visible_letter(
    LetterDistribution *letter_distribution, uint8_t ml, size_t len,
    StringBuilder *string_builder) {
  string_builder_add_string(
      string_builder,
      letter_distribution->machine_letter_to_human_readable_letter[ml], len);
}