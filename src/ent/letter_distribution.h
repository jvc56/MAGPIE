#ifndef LETTER_DISTRIBUTION_H
#define LETTER_DISTRIBUTION_H

#include "../def/board_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../ent/equity.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "data_filepaths.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

typedef enum {
  LD_TYPE_UNKNOWN,
  LD_TYPE_ENGLISH,
  LD_TYPE_GERMAN,
  LD_TYPE_NORWEGIAN,
  LD_TYPE_CATALAN,
  LD_TYPE_POLISH,
  LD_TYPE_DUTCH,
  LD_TYPE_FRENCH,
} ld_t;

enum {
  INVALID_LETTER = 0x80 - 1,
  MULTICHAR_START_DELIMITER = '[',
  MULTICHAR_END_DELIMITER = ']'
};

typedef struct LetterDistribution {
  char *name;
  int size;
  Equity scores[MACHINE_LETTER_MAX_VALUE];
  uint8_t distribution[MACHINE_LETTER_MAX_VALUE];
  // machine letters sorted in descending
  // score order
  MachineLetter score_order[MACHINE_LETTER_MAX_VALUE];
  bool is_vowel[MACHINE_LETTER_MAX_VALUE];
  int total_tiles;
  size_t max_tile_length;
  char ld_ml_to_hl[MACHINE_LETTER_MAX_VALUE][MAX_LETTER_BYTE_LENGTH];
} LetterDistribution;

static inline MachineLetter get_blanked_machine_letter(MachineLetter ml) {
  return ml | BLANK_MASK;
}

static inline MachineLetter get_unblanked_machine_letter(MachineLetter ml) {
  return ml & UNBLANK_MASK;
}

static inline bool get_is_blanked(MachineLetter ml) {
  return (ml & BLANK_MASK) > 0;
}

// Returns true if the machine letters are successfully unblanked
// Returns false if the unblanking fails and renders the machine letters
// invalid.
static inline bool unblank_machine_letters(MachineLetter *mls, int size) {
  for (int i = 0; i < size; i++) {
    if (mls[i] == BLANK_MACHINE_LETTER) {
      return false;
    }
    mls[i] = get_unblanked_machine_letter(mls[i]);
  }
  return true;
}

static inline void sort_score_order(LetterDistribution *ld) {
  const Equity *scores = ld->scores;
  int size = ld->size;

  for (int i = 1; i < size; ++i) {
    MachineLetter key = ld->score_order[i];
    int j = i - 1;

    while (j >= 0 && scores[ld->score_order[j]] < scores[key]) {
      ld->score_order[j + 1] = ld->score_order[j];
      --j;
    }

    ld->score_order[j + 1] = key;
  }
}

static inline void ld_create_internal(const char *ld_name,
                                      const StringSplitter *ld_lines,
                                      LetterDistribution *ld,
                                      ErrorStack *error_stack) {
  int number_of_lines = string_splitter_get_number_of_items(ld_lines);

  ld->size = number_of_lines;

  memset(ld->distribution, 0, sizeof(ld->distribution));
  memset(ld->scores, 0, sizeof(ld->scores));
  memset(ld->score_order, 0, sizeof(ld->score_order));
  memset(ld->is_vowel, 0, sizeof(ld->is_vowel));

  for (int i = 0; i < MACHINE_LETTER_MAX_VALUE; i++) {
    ld->ld_ml_to_hl[i][0] = '\0';
  }

  int machine_letter = 0;
  size_t max_tile_length = 0;
  ld->total_tiles = 0;
  StringSplitter *single_letter_info = NULL;
  for (int i = 0; i < number_of_lines; i++) {
    const char *line = string_splitter_get_item(ld_lines, i);
    single_letter_info = split_string(line, ',', true);
    if (string_splitter_get_number_of_items(single_letter_info) != 5) {
      error_stack_push(
          error_stack, ERROR_STATUS_LD_INVALID_ROW,
          get_formatted_string("invalid row in letter distribution file %s: %s",
                               ld_name, line));
      break;
    }
    // letter, lower case, dist, score, is_vowel
    const char *letter = string_splitter_get_item(single_letter_info, 0);
    const char *lower_case_letter =
        string_splitter_get_item(single_letter_info, 1);
    int dist = string_to_int(string_splitter_get_item(single_letter_info, 2),
                             error_stack);
    if (!error_stack_is_empty(error_stack)) {
      error_stack_push(
          error_stack, ERROR_STATUS_LD_INVALID_ROW,
          get_formatted_string("invalid value for the number of '%s' in letter "
                               "distribution file %s: %s",
                               letter, ld_name, line));
      break;
    }
    ld->total_tiles += dist;
    const int score_int = string_to_int(
        string_splitter_get_item(single_letter_info, 3), error_stack);
    if (!error_stack_is_empty(error_stack)) {
      error_stack_push(
          error_stack, ERROR_STATUS_LD_INVALID_ROW,
          get_formatted_string("invalid value for the score of '%s' in letter "
                               "distribution file %s: %s",
                               letter, ld_name, line));
      break;
    }
    const Equity score = int_to_equity(score_int);
    int is_vowel = string_to_int(
        string_splitter_get_item(single_letter_info, 4), error_stack);

    if (!error_stack_is_empty(error_stack) || is_vowel < 0 || is_vowel > 1) {
      error_stack_push(
          error_stack, ERROR_STATUS_LD_INVALID_ROW,
          get_formatted_string("invalid value for whether '%s' is a vowel "
                               "(expected 0 or 1) in letter "
                               "distribution file %s: %s",
                               letter, ld_name, line));
      break;
    }

    size_t tile_length = string_length(letter);
    if (tile_length > max_tile_length) {
      max_tile_length = tile_length;
    }

    ld->distribution[machine_letter] = dist;
    ld->score_order[i] = machine_letter;
    ld->scores[machine_letter] = score;
    ld->is_vowel[machine_letter] = is_vowel == 1;

    strncpy(ld->ld_ml_to_hl[machine_letter], letter,
            sizeof(ld->ld_ml_to_hl[machine_letter]));

    if (machine_letter > 0) {
      MachineLetter blanked_machine_letter =
          get_blanked_machine_letter(machine_letter);
      strncpy(ld->ld_ml_to_hl[blanked_machine_letter], lower_case_letter,
              sizeof(ld->ld_ml_to_hl[blanked_machine_letter]));
    }
    string_splitter_destroy(single_letter_info);
    single_letter_info = NULL;
    machine_letter++;
  }
  string_splitter_destroy(single_letter_info);
  if (error_stack_is_empty(error_stack)) {
    sort_score_order(ld);
    ld->max_tile_length = max_tile_length;
    ld->name = string_duplicate(ld_name);
  }
}

static inline void ld_destroy(LetterDistribution *ld) {
  if (!ld) {
    return;
  }
  free(ld->name);
  free(ld);
}

static inline LetterDistribution *ld_create(const char *data_paths,
                                            const char *ld_name,
                                            ErrorStack *error_stack) {
  char *ld_filename = data_filepaths_get_readable_filename(
      data_paths, ld_name, DATA_FILEPATH_TYPE_LD, error_stack);
  LetterDistribution *ld = NULL;
  if (error_stack_is_empty(error_stack)) {
    StringSplitter *ld_lines = split_file_by_newline(ld_filename, error_stack);
    if (error_stack_is_empty(error_stack)) {
      ld = calloc_or_die(1, sizeof(LetterDistribution));
      ld_create_internal(ld_name, ld_lines, ld, error_stack);
    }
    string_splitter_destroy(ld_lines);
  }
  free(ld_filename);
  if (!error_stack_is_empty(error_stack)) {
    ld_destroy(ld);
    ld = NULL;
  }
  return ld;
}

static inline const char *ld_get_name(const LetterDistribution *ld) {
  return ld->name;
}

static inline int ld_get_size(const LetterDistribution *ld) { return ld->size; }

static inline int ld_get_dist(const LetterDistribution *ld, MachineLetter ml) {
  return ld->distribution[ml];
}

static inline Equity ld_get_score(const LetterDistribution *ld,
                                  MachineLetter ml) {
  return ld->scores[ml];
}

static inline int ld_get_score_order(const LetterDistribution *ld,
                                     MachineLetter ml) {
  return ld->score_order[ml];
}

static inline bool ld_get_is_vowel(const LetterDistribution *ld,
                                   MachineLetter ml) {
  return ld->is_vowel[ml];
}

static inline int ld_get_total_tiles(const LetterDistribution *ld) {
  return ld->total_tiles;
}

// Returns:
//  * the number of utf8 bytes for this code point for the first byte or
//  * 0 for subsequent bytes in the code point or
//  * -1 for invalid UTF8 bytes
static inline int get_number_of_utf8_bytes_for_code_point(uint8_t byte) {
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

static inline bool
is_human_readable_letter_multichar(const char *human_readable_letter) {
  // If the number of bytes in the string is greater than the number of bytes
  // in the first code point, there must be more than one code point and
  // therefore the string must necessarily be a multichar string.
  return (int)string_length(human_readable_letter) >
         get_number_of_utf8_bytes_for_code_point(human_readable_letter[0]);
}

static inline char *ld_ml_to_hl(const LetterDistribution *ld,
                                MachineLetter ml) {
  const char *human_readable_letter = ld->ld_ml_to_hl[ml];
  if (is_human_readable_letter_multichar(human_readable_letter)) {
    return get_formatted_string("[%s]", human_readable_letter);
  }
  return string_duplicate(human_readable_letter);
}

// This is a linear search. This function should not be used for anything
// that is speed-critical. If we ever need to use this in anything
// speed-critical, we should use a hash.
static inline MachineLetter ld_hl_to_ml(const LetterDistribution *ld,
                                        const char *letter) {
  for (int i = 0; i < MACHINE_LETTER_MAX_VALUE; i++) {
    if (strings_equal(ld->ld_ml_to_hl[i], letter)) {
      return i;
    }
  }
  return INVALID_LETTER;
}

static inline bool char_is_playthrough(const char c) {
  return c == ASCII_PLAYED_THROUGH || c == ASCII_UCGI_PLAYED_THROUGH;
}

// Convert a string of arbitrary characters into an array of machine letters,
// returning the number of machine letters. This function does not allocate
// the ml array; it is the caller's responsibility to make this array big
// enough. This function will return -1 if it encounters an invalid letter.
// Note: This is a slow function that should not be used in any hot loops.
static inline int ld_str_to_mls(const LetterDistribution *ld, const char *str,
                                bool allow_played_through_marker,
                                MachineLetter *mls, size_t mls_size) {
  assert(str != NULL);
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
  // - every start multichar delimiter has an associated end multichar delimiter
  // - multichar characters are nonempty
  for (size_t i = 0; i < num_bytes; i++) {
    char current_char = str[i];
    switch (current_char) {
    case MULTICHAR_START_DELIMITER:
      if (building_multichar_letter || current_code_point_bytes_remaining > 0) {
        return -1;
      }
      building_multichar_letter = true;
      break;
    case MULTICHAR_END_DELIMITER:
      // Return an error if
      // - multichar is not being built
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
      MachineLetter ml = ld_hl_to_ml(ld, current_letter);
      if (ml == INVALID_LETTER) {
        if (current_letter_byte_index == 1 && allow_played_through_marker &&
            char_is_playthrough(current_char)) {
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

static inline bool ld_types_compat(ld_t ld_type_1, ld_t ld_type_2) {
  return ld_type_1 == ld_type_2;
}

static inline ld_t ld_get_type_from_lex_name(const char *full_lexicon_name,
                                             ErrorStack *error_stack) {
  const char *lexicon_name = get_base_filename(full_lexicon_name);
  ld_t ld_type;
  if (has_iprefix("CSW", lexicon_name) || has_iprefix("NWL", lexicon_name) ||
      has_iprefix("OSPD", lexicon_name) || has_iprefix("OSW", lexicon_name) ||
      has_iprefix("TWL", lexicon_name) ||
      has_iprefix("America", lexicon_name) ||
      has_iprefix("CEL", lexicon_name)) {
    ld_type = LD_TYPE_ENGLISH;
  } else if (has_iprefix("RD", lexicon_name)) {
    ld_type = LD_TYPE_GERMAN;
  } else if (has_iprefix("NSF", lexicon_name)) {
    ld_type = LD_TYPE_NORWEGIAN;
  } else if (has_iprefix("DISC", lexicon_name)) {
    ld_type = LD_TYPE_CATALAN;
  } else if (has_iprefix("FRA", lexicon_name)) {
    ld_type = LD_TYPE_FRENCH;
  } else if (has_iprefix("OSPS", lexicon_name)) {
    ld_type = LD_TYPE_POLISH;
  } else if (has_iprefix("DSW", lexicon_name)) {
    ld_type = LD_TYPE_DUTCH;
  } else {
    error_stack_push(
        error_stack, ERROR_STATUS_LD_LEXICON_DEFAULT_NOT_FOUND,
        get_formatted_string(
            "default letter distribution not found for lexicon '%s'",
            lexicon_name));
    ld_type = LD_TYPE_UNKNOWN;
  }
  return ld_type;
}

static inline ld_t ld_get_type_from_ld_name(const char *ld_name,
                                            ErrorStack *error_stack) {
  ld_t ld_type;
  if (has_iprefix(ENGLISH_LETTER_DISTRIBUTION_NAME, ld_name)) {
    ld_type = LD_TYPE_ENGLISH;
  } else if (has_iprefix(GERMAN_LETTER_DISTRIBUTION_NAME, ld_name)) {
    ld_type = LD_TYPE_GERMAN;
  } else if (has_iprefix(NORWEGIAN_LETTER_DISTRIBUTION_NAME, ld_name)) {
    ld_type = LD_TYPE_NORWEGIAN;
  } else if (has_iprefix(CATALAN_LETTER_DISTRIBUTION_NAME, ld_name)) {
    ld_type = LD_TYPE_CATALAN;
  } else if (has_iprefix(FRENCH_LETTER_DISTRIBUTION_NAME, ld_name)) {
    ld_type = LD_TYPE_FRENCH;
  } else if (has_iprefix(POLISH_LETTER_DISTRIBUTION_NAME, ld_name)) {
    ld_type = LD_TYPE_POLISH;
  } else if (has_iprefix(DUTCH_LETTER_DISTRIBUTION_NAME, ld_name)) {
    ld_type = LD_TYPE_DUTCH;
  } else {
    error_stack_push(error_stack, ERROR_STATUS_LD_NAME_NOT_FOUND,
                     get_formatted_string(
                         "letter distribution not found for '%s'", ld_name));
    ld_type = LD_TYPE_UNKNOWN;
  }
  return ld_type;
}

// Use the lexicon name in combination with the constant
// BOARD_DIM to determine a default letter distribution name.
static inline char *ld_get_default_name_from_type(ld_t ld_type,
                                                  ErrorStack *error_stack) {
  // NOLINTNEXTLINE(misc-redundant-expression)
  if (BOARD_DIM != DEFAULT_BOARD_DIM && BOARD_DIM != DEFAULT_SUPER_BOARD_DIM) {
    error_stack_push(
        error_stack, ERROR_STATUS_LD_UNSUPPORTED_BOARD_DIM_DEFAULT,
        get_formatted_string(
            "default letter distribution not supported with a board "
            "dimension of %d (only %d and %d have "
            "default values)",
            BOARD_DIM, DEFAULT_BOARD_DIM, DEFAULT_SUPER_BOARD_DIM));
    return NULL;
  }
  const char *ld_name_extension = "";
  if (BOARD_DIM == DEFAULT_SUPER_BOARD_DIM) {
    ld_name_extension = "_" SUPER_LETTER_DISTRIBUTION_NAME_EXTENSION;
  }

  char *ld_name = NULL;
  switch (ld_type) {
  case LD_TYPE_ENGLISH:
    ld_name = get_formatted_string("%s%s", ENGLISH_LETTER_DISTRIBUTION_NAME,
                                   ld_name_extension);
    break;
  case LD_TYPE_GERMAN:
    ld_name = get_formatted_string("%s%s", GERMAN_LETTER_DISTRIBUTION_NAME,
                                   ld_name_extension);
    break;
  case LD_TYPE_NORWEGIAN:
    ld_name = get_formatted_string("%s%s", NORWEGIAN_LETTER_DISTRIBUTION_NAME,
                                   ld_name_extension);
    break;
  case LD_TYPE_CATALAN:
    ld_name = get_formatted_string("%s%s", CATALAN_LETTER_DISTRIBUTION_NAME,
                                   ld_name_extension);
    break;
  case LD_TYPE_FRENCH:
    ld_name = get_formatted_string("%s%s", FRENCH_LETTER_DISTRIBUTION_NAME,
                                   ld_name_extension);
    break;
  case LD_TYPE_POLISH:
    ld_name = get_formatted_string("%s%s", POLISH_LETTER_DISTRIBUTION_NAME,
                                   ld_name_extension);
    break;
  case LD_TYPE_DUTCH:
    ld_name = get_formatted_string("%s%s", DUTCH_LETTER_DISTRIBUTION_NAME,
                                   ld_name_extension);
    break;
  case LD_TYPE_UNKNOWN:
    log_fatal("encountered unexpected letter distribution type %d", ld_type);
    break;
  }

  return ld_name;
}

static inline char *
ld_get_default_name_from_lexicon_name(const char *lexicon_name,
                                      ErrorStack *error_stack) {
  ld_t ld_type = ld_get_type_from_lex_name(lexicon_name, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return NULL;
  }
  return ld_get_default_name_from_type(ld_type, error_stack);
}

#endif