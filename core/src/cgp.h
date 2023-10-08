
#ifndef CGP_H
#define CGP_H

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

CGPOperations *get_default_cgp_operations();
void destroy_cgp_operations(CGPOperations *cgp_operations);
cgp_parse_status_t load_cgp_operations(CGPOperations *cgp_operations,
                                       const char *cgp);
#endif