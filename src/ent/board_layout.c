#include "board_layout.h"

#include <stdint.h>
#include <stdio.h>

#include "../def/board_defs.h"
#include "../def/board_layout_defs.h"

#include "../util/log.h"
#include "../util/string_util.h"
#include "../util/util.h"

#define BONUS_SQUARE_MAP_SIZE 256

#define BONUS_SQUARE_CHAR_NONE ' '
#define BONUS_SQUARE_CHAR_DOUBLE_LETTER '\''
#define BONUS_SQUARE_CHAR_DOUBLE_WORD '-'
#define BONUS_SQUARE_CHAR_TRIPLE_LETTER '"'
#define BONUS_SQUARE_CHAR_TRIPLE_WORD '='
#define BONUS_SQUARE_CHAR_QUADRUPLE_LETTER '^'
#define BONUS_SQUARE_CHAR_QUADRUPLE_WORD '~'

static const uint8_t bonus_square_chars_to_values_map[BONUS_SQUARE_MAP_SIZE] = {
    [BONUS_SQUARE_CHAR_NONE] = 0x11,
    [BONUS_SQUARE_CHAR_DOUBLE_LETTER] = 0x12,
    [BONUS_SQUARE_CHAR_DOUBLE_WORD] = 0x21,
    [BONUS_SQUARE_CHAR_TRIPLE_LETTER] = 0x13,
    [BONUS_SQUARE_CHAR_TRIPLE_WORD] = 0x31,
    [BONUS_SQUARE_CHAR_QUADRUPLE_LETTER] = 0x14,
    [BONUS_SQUARE_CHAR_QUADRUPLE_WORD] = 0x41,
    [BRICK_CHAR] = BRICK_VALUE,
};

static const char bonus_square_values_to_chars_map[BONUS_SQUARE_MAP_SIZE] = {
    [0x11] = BONUS_SQUARE_CHAR_NONE,
    [0x12] = BONUS_SQUARE_CHAR_DOUBLE_LETTER,
    [0x21] = BONUS_SQUARE_CHAR_DOUBLE_WORD,
    [0x13] = BONUS_SQUARE_CHAR_TRIPLE_LETTER,
    [0x31] = BONUS_SQUARE_CHAR_TRIPLE_WORD,
    [0x14] = BONUS_SQUARE_CHAR_QUADRUPLE_LETTER,
    [0x41] = BONUS_SQUARE_CHAR_QUADRUPLE_WORD,
    [BRICK_VALUE] = BRICK_CHAR,
};

static const char* bonus_square_values_to_color_codes[BONUS_SQUARE_MAP_SIZE] = {
    [0x11] = "\x1b[0m",     // none, reset
    [0x12] = "\x1b[1;36m",  // DLS, cyan
    [0x21] = "\x1b[1;35m",  // DWS, magenta
    [0x13] = "\x1b[1;34m",  // TLS, blue
    [0x31] = "\x1b[1;31m",  // TWS, red
    [0x14] = "\x1b[1;95m",  // QLS, bright magenta
    [0x41] = "\x1b[1;33m",  // QWS, yellow
    [BRICK_VALUE] = "\x1b[1;90m",  // brick, gray
};

static const char* bonus_square_values_to_alt_strings[BONUS_SQUARE_MAP_SIZE] = {
    [0x11] = "　",  // ideographic space
    [0x12] = "＇",  // fullwidth apostrophe
    [0x21] = "－",  // fullwidth hyphen-minus
    [0x13] = "＂",  // fullwidth quotation mark
    [0x31] = "＝",  // fullwidth equals sign
    [0x14] = "＾",  // fullwidth circumflex accent
    [0x41] = "～",  // fullwidth tilde
    [BRICK_VALUE] = "＃",  // fullwidth number sign
};

struct BoardLayout {
  int start_coords[2];
  uint8_t bonus_squares[BOARD_DIM * BOARD_DIM];
};

int board_layout_get_index(int row, int col) { return row * BOARD_DIM + col; }

uint8_t board_layout_get_bonus_square(const BoardLayout *bl, int row, int col) {
  return bl->bonus_squares[board_layout_get_index(row, col)];
}

int board_layout_get_start_coord(const BoardLayout *bl, int index) {
  return bl->start_coords[index];
}

BoardLayout *board_layout_create() {
  return malloc_or_die(sizeof(BoardLayout));
}

char *board_layout_get_filepath(const char *layout_name) {
  if (!layout_name) {
    log_fatal("layout name is null");
  }
  return get_formatted_string("%s%s%s", BOARD_LAYOUT_FILEPATH, layout_name,
                              BOARD_LAYOUT_FILE_EXTENSION);
}

board_layout_load_status_t
board_layout_parse_split_start_coords(BoardLayout *bl,
                                      const StringSplitter *starting_coords) {

  if (string_splitter_get_number_of_items(starting_coords) != 2) {
    return BOARD_LAYOUT_LOAD_STATUS_MALFORMED_START_COORDS;
  }

  for (int i = 0; i < 2; i++) {
    const int lane_start_value =
        string_to_int(string_splitter_get_item(starting_coords, i));
    if (lane_start_value < 0 || lane_start_value >= BOARD_DIM) {
      return BOARD_LAYOUT_LOAD_STATUS_OUT_OF_BOUNDS_START_COORDS;
    }
    bl->start_coords[i] = lane_start_value;
  }
  return BOARD_LAYOUT_LOAD_STATUS_SUCCESS;
}

char bonus_square_value_to_char(uint8_t bonus_square_value) {
  return bonus_square_values_to_chars_map[bonus_square_value];
}

const char *bonus_square_value_to_color_code(uint8_t bonus_square_value) {
  return bonus_square_values_to_color_codes[bonus_square_value];
}

const char *bonus_square_value_to_alt_string(uint8_t bonus_square_value) {
  return bonus_square_values_to_alt_strings[bonus_square_value];
}

uint8_t bonus_square_char_to_value(char bonus_square_char) {
  return bonus_square_chars_to_values_map[(int)bonus_square_char];
}

board_layout_load_status_t
board_layout_parse_split_file(BoardLayout *bl,
                              const StringSplitter *file_lines) {
  const int number_of_rows = string_splitter_get_number_of_items(file_lines);

  if (number_of_rows != BOARD_DIM + 1) {
    return BOARD_LAYOUT_LOAD_STATUS_INVALID_NUMBER_OF_ROWS;
  }

  StringSplitter *starting_coords =
      split_string(string_splitter_get_item(file_lines, 0), ',', true);

  board_layout_load_status_t status =
      board_layout_parse_split_start_coords(bl, starting_coords);

  destroy_string_splitter(starting_coords);

  if (status != BOARD_LAYOUT_LOAD_STATUS_SUCCESS) {
    return status;
  }

  for (int row = 0; row < BOARD_DIM; row++) {
    const char *layout_row = string_splitter_get_item(file_lines, row + 1);
    if (string_length(layout_row) != BOARD_DIM) {
      return BOARD_LAYOUT_LOAD_STATUS_INVALID_NUMBER_OF_COLS;
    }
    for (int col = 0; col < BOARD_DIM; col++) {
      const char bonus_square_char = layout_row[col];
      const int index = board_layout_get_index(row, col);
      const uint8_t bonus_square_value =
          bonus_square_char_to_value(bonus_square_char);
      if (bonus_square_value == 0) {
        return BOARD_LAYOUT_LOAD_STATUS_INVALID_BONUS_SQUARE;
      }
      bl->bonus_squares[index] = bonus_square_value;
    }
  }
  return BOARD_LAYOUT_LOAD_STATUS_SUCCESS;
}

board_layout_load_status_t board_layout_load(BoardLayout *bl,
                                             const char *layout_filename) {
  StringSplitter *layout_rows = split_file_by_newline(layout_filename);

  board_layout_load_status_t status =
      board_layout_parse_split_file(bl, layout_rows);

  destroy_string_splitter(layout_rows);

  return status;
}

char *board_layout_get_default_name() {
  return get_formatted_string("standard%d", BOARD_DIM);
}

BoardLayout *board_layout_create_default() {
  BoardLayout *bl = malloc_or_die(sizeof(BoardLayout));
  char *default_layout_name = board_layout_get_default_name();
  char *default_layout_filepath =
      board_layout_get_filepath(default_layout_name);
  board_layout_load_status_t status =
      board_layout_load(bl, default_layout_filepath);
  free(default_layout_name);
  free(default_layout_filepath);
  if (status != BOARD_LAYOUT_LOAD_STATUS_SUCCESS) {
    log_fatal("standard board with dim %d failed to load", BOARD_DIM);
  }
  return bl;
}

void board_layout_destroy(BoardLayout *bl) { free(bl); }