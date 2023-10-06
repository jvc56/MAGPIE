#include "constants.h"

typedef enum {
  CGP_PARSE_STATUS_SUCCESS,
  CGP_PARSE_STATUS_MISSING_REQUIRED_FIELDS,
  CGP_PARSE_STATUS_INVALID_NUMBER_OF_BOARD_ROWS,
  CGP_PARSE_STATUS_INVALID_NUMBER_OF_BOARD_COLUMNS,
  CGP_PARSE_STATUS_INVALID_NUMBER_OF_PLAYER_RACKS,
  CGP_PARSE_STATUS_INVALID_NUMBER_OF_PLAYER_SCORES,
  CGP_PARSE_STATUS_MALFORMED_BOARD_LETTERS,
  CGP_PARSE_STATUS_MALFORMED_SCORES,
  CGP_PARSE_STATUS_MALFORMED_RACK_LETTERS,
  CGP_PARSE_STATUS_MALFORMED_CONSECUTIVE_ZEROS,
  CGP_PARSE_STATUS_MALFORMED_CGP_OPCODE_BINGO_BONUS,
  CGP_PARSE_STATUS_MALFORMED_CGP_OPCODE_BOARD_NAME,
  CGP_PARSE_STATUS_MALFORMED_CGP_OPCODE_GAME_VARIANT,
} cgp_parse_status_t;

typedef struct CGPOperations {
  int bingo_bonus;
  board_layout_t board_layout;
  game_variant_t game_variant;
  char *letter_distribution_name;
  char *lexicon_name;
} CGPOperations;

CGPOperations *get_default_cgp_operations() {
  CGPOperations *cgp_operations = malloc_or_die(sizeof(CGPOperations));
  cgp_operations->bingo_bonus = BINGO_BONUS;
  cgp_operations->board_layout = BOARD_LAYOUT_CROSSWORD_GAME;
  cgp_operations->game_variant = GAME_VARIANT_CLASSIC;
  cgp_operations->letter_distribution_name = NULL;
  cgp_operations->lexicon_name = NULL;
  return cgp_operations;
}

void destroy_cgp_operations(CGPOperations *cgp_operations) {
  if (cgp_operations->lexicon_name) {
    free(cgp_operations->lexicon_name);
  }
  if (cgp_operations->letter_distribution_name) {
    free(cgp_operations->letter_distribution_name);
  }
  free(cgp_operations);
}

cgp_parse_status_t load_cgp_operations(CGPOperations *cgp_operations,
                                       const char *cgp) {
  cgp_parse_status_t cgp_parse_status = CGP_PARSE_STATUS_SUCCESS;
  StringSplitter *split_cgp_string = split_string_by_whitespace(cgp, true);
  int number_of_items = string_splitter_get_number_of_items(split_cgp_string);
  for (int i = 0; i < number_of_items - 1; i++) {
    const char *opcode = string_splitter_get_item(split_cgp_string, i);
    char *string_value = string_splitter_get_item(split_cgp_string, i + 1);

    // For now all values can be derived from a single contiguous
    // string, so if any of them have a semicolon at the end,
    // remove it.
    // FIXME: move this 'remove last char' function to string util
    size_t string_value_length = string_length(string_value);
    if (string_value[string_value_length - 1] == ';') {
      string_value[string_value_length - 1] = '\0';
    }
    if (strings_equal(CGP_OPCODE_BINGO_BONUS, opcode)) {
      if (!is_all_digits_or_empty(string_value)) {
        cgp_parse_status = CGP_PARSE_STATUS_MALFORMED_CGP_OPCODE_BINGO_BONUS;
        break;
      }
      cgp_operations->bingo_bonus = string_to_int(string_value);
    } else if (strings_equal(CGP_OPCODE_BOARD_NAME, opcode)) {
      cgp_operations->board_layout =
          board_layout_string_to_board_layout(string_value);
      if (cgp_operations->board_layout == BOARD_LAYOUT_UNKNOWN) {
        cgp_parse_status = CGP_PARSE_STATUS_MALFORMED_CGP_OPCODE_BOARD_NAME;
      }
    } else if (strings_equal(CGP_OPCODE_GAME_VARIANT, opcode)) {
      cgp_operations->game_variant =
          get_game_variant_type_from_name(string_value);
      if (cgp_operations->game_variant == GAME_VARIANT_UNKNOWN) {
        cgp_parse_status = CGP_PARSE_STATUS_MALFORMED_CGP_OPCODE_GAME_VARIANT;
      }
    } else if (strings_equal(CGP_OPCODE_LETTER_DISTRIBUTION_NAME, opcode)) {
      if (cgp_operations->letter_distribution_name) {
        free(cgp_operations->letter_distribution_name);
      }
      cgp_operations->letter_distribution_name =
          get_formatted_string("%s", string_value);
    } else if (strings_equal(CGP_OPCODE_LEXICON_NAME, opcode)) {
      if (cgp_operations->lexicon_name) {
        free(cgp_operations->lexicon_name);
      }
      cgp_operations->lexicon_name = get_formatted_string("%s", string_value);
    }
  }
  destroy_string_splitter(split_cgp_string);
  return cgp_parse_status;
}
