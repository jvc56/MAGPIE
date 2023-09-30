#include <assert.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fileproxy.h"
#include "letter_distribution.h"
#include "log.h"
#include "string_builder.h"
#include "util.h"

#define LETTER_DISTRIBUTION_FILE_EXTENSION ".csv"
#define LETTER_DISTRIBUTION_FILEPATH "data/letterdistributions/"
#define INVALID_LETTER (0x80 - 1)

extern inline uint8_t get_blanked_machine_letter(uint8_t ml);
extern inline uint8_t get_unblanked_machine_letter(uint8_t ml);
extern inline uint8_t is_blanked(uint8_t ml);

int get_letter_distribution_size(const char *filename) {
  FILE *file = stream_from_filename(filename);
  if (file == NULL) {
    printf("Error opening file to count lines: %s\n", filename);
    return -1;
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

void load_letter_distribution(LetterDistribution *letter_distribution,
                              const char *letter_distribution_filename) {
  // This function call opens and closes the file, so
  // call it before the fopen to prevent a nested file read
  letter_distribution->size =
      get_letter_distribution_size(letter_distribution_filename);

  FILE *file = stream_from_filename(letter_distribution_filename);
  if (file == NULL) {
    log_fatal("Error opening letter distribution file: %s\n",
              letter_distribution_filename);
  }

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
    char *token;
    // letter, lower case, dist, score, is_vowel
    token = strtok(line, ",");
    char letter[5];
    char lower_case_letter[5];
    strcpy(letter, token);

    token = strtok(NULL, ",");
    strcpy(lower_case_letter, token);

    int nl = strlen(letter);
    if (nl > max_tile_length) {
      max_tile_length = nl;
    }

    token = strtok(NULL, ",");
    int dist = atoi(token);

    token = strtok(NULL, ",");
    int score = atoi(token);

    token = strtok(NULL, ",");
    int is_vowel = atoi(token);

    letter_distribution->distribution[machine_letter] = dist;
    letter_distribution->scores[machine_letter] = score;
    letter_distribution->is_vowel[machine_letter] = is_vowel;

    strcpy(letter_distribution
               ->machine_letter_to_human_readable_letter[machine_letter],
           letter);

    if (machine_letter > 0) {
      uint8_t blanked_machine_letter =
          get_blanked_machine_letter(machine_letter);
      strcpy(
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
    if (strcmp(letter_distribution->machine_letter_to_human_readable_letter[i],
               letter) == 0) {
      return i;
    }
  }
  return INVALID_LETTER;
}

void machine_letter_to_human_readable_letter(
    LetterDistribution *letter_distribution, uint8_t ml,
    char letter[MAX_LETTER_CHAR_LENGTH]) {
  strcpy(letter,
         letter_distribution->machine_letter_to_human_readable_letter[ml]);
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
  int num_bytes = strlen(str);
  int i = 0;
  int prev_i = -1;
  while (i < num_bytes) {
    for (int j = i + letter_distribution->max_tile_length; j > i; j--) {
      if (j > num_bytes) {
        continue;
      }
      // possible letter goes from index i to j. Search for it.
      char possible_letter[MAX_LETTER_CHAR_LENGTH];
      memcpy(possible_letter, str + i, j - i);
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

LetterDistribution *create_letter_distribution(const char *filename) {
  LetterDistribution *letter_distribution =
      malloc_or_die(sizeof(LetterDistribution));
  load_letter_distribution(letter_distribution, filename);
  return letter_distribution;
}

void destroy_letter_distribution(LetterDistribution *letter_distribution) {
  free(letter_distribution->distribution);
  free(letter_distribution->scores);
  free(letter_distribution->is_vowel);
  free(letter_distribution->score_order);
  free(letter_distribution);
}

char *get_letter_distribution_filepath(const char *ld_name) {
  // Check for invalid inputs
  if (ld_name == NULL) {
    return NULL;
  }

  const char *directory_path = LETTER_DISTRIBUTION_FILEPATH;

  // Calculate the lengths of the input strings
  size_t directory_path_len = strlen(directory_path);
  size_t ld_name_len = strlen(ld_name);

  // Allocate memory for the result string
  char *result =
      (char *)malloc_or_die((directory_path_len + ld_name_len +
                             strlen(LETTER_DISTRIBUTION_FILE_EXTENSION) + 1) *
                            sizeof(char));

  // Copy the directory_path into the result
  strcpy(result, directory_path);

  // Check if directory_path ends with a directory separator (e.g., '/' or '\')
  if (directory_path_len > 0 && directory_path[directory_path_len - 1] != '/' &&
      directory_path[directory_path_len - 1] != '\\') {
    // Add a directory separator if it's missing
    strcat(result, "/");
  }

  // Concatenate the ld_name
  strcat(result, ld_name);

  // Add the ".csv" extension
  strcat(result, LETTER_DISTRIBUTION_FILE_EXTENSION);

  return result;
}

// FIXME: return letter distrubitions other than english
char *get_letter_distribution_name_from_lexicon_name(const char *lexicon_name) {
  log_warn("returning 'english' for %s", lexicon_name);
  return strdup("english");
}

void write_user_visible_letter(char *dest,
                               LetterDistribution *letter_distribution,
                               uint8_t ml) {

  char human_letter[MAX_LETTER_CHAR_LENGTH] = "";
  machine_letter_to_human_readable_letter(letter_distribution, ml,
                                          human_letter);
  for (size_t i = 0; i < strlen(human_letter); i++) {
    sprintf(dest + strlen(dest), "%c", human_letter[i]);
  }
}

void string_builder_add_user_visible_letter(
    LetterDistribution *letter_distribution, uint8_t ml, size_t len,
    StringBuilder *string_builder) {

  char human_letter[MAX_LETTER_CHAR_LENGTH] = "";
  write_user_visible_letter(human_letter, letter_distribution, ml);
  string_builder_add_string(string_builder, human_letter, len);
}