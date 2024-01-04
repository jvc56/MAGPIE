#include "letter_distribution.h"

#include <stdbool.h>
#include <stdlib.h>

#include "../def/letter_distribution_defs.h"

#include "../util/log.h"
#include "../util/string_util.h"
#include "../util/util.h"

#define INVALID_LETTER (0x80 - 1)
#define MULTICHAR_START_DELIMITIER '['
#define MULTICHAR_END_DELIMITIER ']'

struct LetterDistribution {
  int size;
  int *distribution;
  int *scores;
  // machine letters sorted in descending
  // score order
  int *score_order;
  bool *is_vowel;
  int total_tiles;
  int max_tile_length;
  char ld_ml_to_hl[MACHINE_LETTER_MAX_VALUE][MAX_LETTER_BYTE_LENGTH];
};

char *get_ld_filepath(const char *ld_name) {
  // Check for invalid inputs
  if (!ld_name) {
    log_fatal("letter distribution name is null");
  }
  return get_formatted_string("%s%s%s", LETTER_DISTRIBUTION_FILEPATH, ld_name,
                              LETTER_DISTRIBUTION_FILE_EXTENSION);
}

// Sort the machine letters in descending score order
void sort_score_order(LetterDistribution *ld) {
  int *score_order = ld->score_order;
  int *scores = ld->scores;
  int size = ld->size;

  for (int i = 1; i < size; ++i) {
    int key = score_order[i];
    int j = i - 1;

    while (j >= 0 && scores[score_order[j]] < scores[key]) {
      score_order[j + 1] = score_order[j];
      --j;
    }

    score_order[j + 1] = key;
  }
}

void load_ld(LetterDistribution *ld, const char *ld_name) {
  // This function call opens and closes the file, so
  // call it before the fopen to prevent a nested file read
  char *ld_filename = get_ld_filepath(ld_name);

  StringSplitter *ld_lines = split_file_by_newline(ld_filename);

  free(ld_filename);

  int number_of_lines = string_splitter_get_number_of_items(ld_lines);

  ld->size = number_of_lines;

  ld->distribution = (int *)malloc_or_die(ld->size * sizeof(int));
  ld->scores = (int *)malloc_or_die(ld->size * sizeof(int));
  ld->score_order = (int *)malloc_or_die(ld->size * sizeof(int));
  ld->is_vowel = (bool *)malloc_or_die(ld->size * sizeof(bool));

  for (int i = 0; i < MACHINE_LETTER_MAX_VALUE; i++) {
    ld->ld_ml_to_hl[i][0] = '\0';
  }

  int machine_letter = 0;
  int max_tile_length = 0;
  ld->total_tiles = 0;
  for (int i = 0; i < number_of_lines; i++) {
    const char *line = string_splitter_get_item(ld_lines, i);
    StringSplitter *single_letter_info = split_string(line, ',', true);
    if (string_splitter_get_number_of_items(single_letter_info) != 5) {
      log_fatal("invalid letter distribution line in %s:\n>%s<\n", ld_name,
                line);
    }
    // letter, lower case, dist, score, is_vowel
    const char *letter = string_splitter_get_item(single_letter_info, 0);
    const char *lower_case_letter =
        string_splitter_get_item(single_letter_info, 1);
    int dist = string_to_int(string_splitter_get_item(single_letter_info, 2));
    ld->total_tiles += dist;
    int score = string_to_int(string_splitter_get_item(single_letter_info, 3));
    int is_vowel =
        string_to_int(string_splitter_get_item(single_letter_info, 4));

    int tile_length = string_length(letter);
    if (tile_length > max_tile_length) {
      max_tile_length = tile_length;
    }

    ld->distribution[machine_letter] = dist;
    ld->score_order[i] = machine_letter;
    ld->scores[machine_letter] = score;
    ld->is_vowel[machine_letter] = is_vowel == 1;

    string_copy(ld->ld_ml_to_hl[machine_letter], letter);

    if (machine_letter > 0) {
      uint8_t blanked_machine_letter =
          get_blanked_machine_letter(machine_letter);
      string_copy(ld->ld_ml_to_hl[blanked_machine_letter], lower_case_letter);
    }
    destroy_string_splitter(single_letter_info);
    machine_letter++;
  }
  destroy_string_splitter(ld_lines);

  sort_score_order(ld);

  ld->max_tile_length = max_tile_length;
}

LetterDistribution *ld_create(const char *ld_name) {
  LetterDistribution *ld = malloc_or_die(sizeof(LetterDistribution));
  load_ld(ld, ld_name);
  return ld;
}

void ld_destroy(LetterDistribution *ld) {
  if (!ld) {
    return;
  }
  free(ld->distribution);
  free(ld->scores);
  free(ld->is_vowel);
  free(ld->score_order);
  free(ld);
}

int ld_get_size(const LetterDistribution *ld) { return ld->size; }

int ld_get_dist(const LetterDistribution *ld, uint8_t machine_letter) {
  return ld->distribution[machine_letter];
}

int ld_get_score(const LetterDistribution *ld, uint8_t machine_letter) {
  return ld->scores[machine_letter];
}

int ld_get_score_order(const LetterDistribution *ld, uint8_t machine_letter) {
  return ld->score_order[machine_letter];
}

bool ld_get_is_vowel(const LetterDistribution *ld, uint8_t machine_letter) {
  return ld->is_vowel[machine_letter];
}

int ld_get_total_tiles(const LetterDistribution *ld) { return ld->total_tiles; }

int ld_get_max_tile_length(const LetterDistribution *ld) {
  return ld->max_tile_length;
}

char *ld_ml_to_hl(const LetterDistribution *ld, uint8_t ml) {
  return string_duplicate(ld->ld_ml_to_hl[ml]);
}

// This is a linear search. This function should not be used for anything
// that is speed-critical. If we ever need to use this in anything
// speed-critical, we should use a hash.
uint8_t ld_hl_to_ml(const LetterDistribution *ld, char *letter) {
  for (int i = 0; i < MACHINE_LETTER_MAX_VALUE; i++) {
    if (strings_equal(ld->ld_ml_to_hl[i], letter)) {
      return i;
    }
  }
  return INVALID_LETTER;
}

// Returns:
//  * the number of utf8 bytes for this code point for the first byte or
//  * 0 for subsequent bytes in the code point or
//  * -1 for invalid UTF8 bytes
int get_number_of_utf8_bytes_for_code_point(uint8_t byte) {
  int number_of_bytes = -1;
  if ((byte & 0xC0) == 0x80) {
    // Subsequent byte in a code point
    number_of_bytes = 0;
  } else if ((byte & 0x80) == 0x00) {
    // Single-byte UTF-8 character
    number_of_bytes = 1;
  } else if ((byte & 0xE0) == 0xC0) {
    // Two-byte UTF-8 character
    number_of_bytes = 2;
  } else if ((byte & 0xF0) == 0xE0) {
    // Three-byte UTF-8 character
    number_of_bytes = 3;
  } else if ((byte & 0xF8) == 0xF0) {
    // Four-byte UTF-8 character
    number_of_bytes = 4;
  }
  return number_of_bytes;
}

// Convert a string of arbitrary characters into an array of machine letters,
// returning the number of machine letters. This function does not allocate
// the ml array; it is the caller's responsibility to make this array big
// enough. This function will return -1 if it encounters an invalid letter.
// Note: This is a slow function that should not be used in any hot loops.
int ld_str_to_mls(const LetterDistribution *ld, const char *str,
                  bool allow_played_through_marker, uint8_t *mls,
                  size_t mls_size) {

  int num_mls = 0;
  size_t num_bytes = string_length(str);
  // Use +1 for the null terminator
  char current_letter[MAX_LETTER_BYTE_LENGTH + 1];
  int current_letter_byte_index = 0;
  bool building_multichar_letter = false;
  int current_code_point_bytes_remaining = 0;
  int number_of_letters_in_builder = 0;

  // While writing to mls, this loop verifies the following:
  // - absence of nested multichar characters
  // - bijection between the set of start and end multichar delimiters
  // - multichar characters are nonempty
  for (size_t i = 0; i < num_bytes; i++) {
    char current_char = str[i];
    switch (current_char) {
    case MULTICHAR_START_DELIMITIER:
      if (building_multichar_letter || current_code_point_bytes_remaining > 0) {
        return -1;
      }
      building_multichar_letter = true;
      break;
    case MULTICHAR_END_DELIMITIER:
      // Return an error if
      // - multichar is building
      // - multichar has fewer than two letters
      // - code point is being built
      if (!building_multichar_letter || number_of_letters_in_builder < 2 ||
          current_code_point_bytes_remaining > 0) {
        return -1;
      }
      building_multichar_letter = false;
      break;
    default:
      if (current_letter_byte_index == MAX_LETTER_BYTE_LENGTH) {
        // Exceeded max char length
        return -1;
      }

      int number_of_bytes_for_code_point =
          get_number_of_utf8_bytes_for_code_point(current_char);

      if (number_of_bytes_for_code_point < 0) {
        // Return -1 for invalid UTF8 byte
        return -1;
      }

      if (number_of_bytes_for_code_point > 0 &&
          current_code_point_bytes_remaining > 0) {
        // Invalid UTF8 start sequence
        return -1;
      }

      if (!building_multichar_letter && number_of_bytes_for_code_point > 0) {
        // If we are building a multichar character
        // with [ and ], we do not need to account for
        // multibyte unicode code points since the batch
        // processing for multiple bytes is already handled.
        // This is the start of a multibyte code point
        current_code_point_bytes_remaining = number_of_bytes_for_code_point;
      } else if (number_of_bytes_for_code_point != 0) {
        // If we are building a multichar letter, we do not
        // allow single width chars such as [Ż], so we
        // count how many chars we encounter while building
        // a multichar and return an error if we finish with fewer than 2.
        // If this byte is not a continuation of an existing
        // unicode code point, then it is a new character.
        number_of_letters_in_builder++;
      }

      current_letter[current_letter_byte_index] = current_char;
      current_letter_byte_index++;
      current_letter[current_letter_byte_index] = '\0';

      if (current_code_point_bytes_remaining > 0) {
        // Another byte of the multibyte code point
        // has been processed
        current_code_point_bytes_remaining--;
      }
      break;
    }

    // Only write if
    //  - multichar is done building and
    //  - unicode code point is done building
    if (!building_multichar_letter && current_code_point_bytes_remaining == 0) {
      // Not enough space allocated to mls
      if (num_mls >= (int)mls_size) {
        return -1;
      }
      uint8_t ml = ld_hl_to_ml(ld, current_letter);
      if (ml == INVALID_LETTER) {
        if (current_letter_byte_index == 1 && allow_played_through_marker &&
            current_char == ASCII_PLAYED_THROUGH) {
          ml = PLAYED_THROUGH_MARKER;
        } else {
          // letter is invalid
          return -1;
        }
      }
      mls[num_mls] = ml;
      num_mls++;
      current_letter_byte_index = 0;
      number_of_letters_in_builder = 0;
    }
  }
  if (building_multichar_letter || current_code_point_bytes_remaining != 0) {
    return -1;
  }
  return num_mls;
}

char *ld_get_default_name(const char *lexicon_name) {
  char *ld_name = NULL;
  if (has_prefix("CSW", lexicon_name) || has_prefix("NWL", lexicon_name) ||
      has_prefix("TWL", lexicon_name) || has_prefix("America", lexicon_name)) {
    ld_name = string_duplicate(ENGLISH_LETTER_DISTRIBUTION_NAME);
  } else if (has_prefix("RD", lexicon_name)) {
    ld_name = string_duplicate(GERMAN_LETTER_DISTRIBUTION_NAME);
  } else if (has_prefix("NSF", lexicon_name)) {
    ld_name = string_duplicate(NORWEGIAN_LETTER_DISTRIBUTION_NAME);
  } else if (has_prefix("DISC", lexicon_name)) {
    ld_name = string_duplicate(CATALAN_LETTER_DISTRIBUTION_NAME);
  } else if (has_prefix("FRA", lexicon_name)) {
    ld_name = string_duplicate(FRENCH_LETTER_DISTRIBUTION_NAME);
  } else if (has_prefix("OSPS", lexicon_name)) {
    ld_name = string_duplicate(POLISH_LETTER_DISTRIBUTION_NAME);
  } else {
    log_fatal("default letter distribution not found for lexicon %s\n",
              lexicon_name);
  }
  return ld_name;
}

uint8_t get_blanked_machine_letter(uint8_t ml) { return ml | BLANK_MASK; }

uint8_t get_unblanked_machine_letter(uint8_t ml) { return ml & UNBLANK_MASK; }

bool get_is_blanked(uint8_t ml) { return (ml & BLANK_MASK) > 0; }