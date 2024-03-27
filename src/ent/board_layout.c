#include "board_layout.h"

#include <stdint.h>
#include <stdio.h>

#include "../def/board_defs.h"
#include "../def/board_layout_defs.h"

#include "../util/log.h"
#include "../util/string_util.h"
#include "../util/util.h"

static const uint8_t bonus_square_chars_to_values_map[256] = {
    [' '] = 0x11, ['\''] = 0x12, ['-'] = 0x21, ['"'] = 0x13,
    ['='] = 0x31, ['*'] = 0x14,  ['~'] = 0x41, [BRICK_CHAR] = BRICK_VALUE};

static const char bonus_square_values_to_chars_map[256] = {
    [0x11] = ' ', [0x12] = '\'', [0x21] = '-', [0x13] = '"',
    [0x31] = '=', [0x14] = '*',  [0x41] = '~', [BRICK_VALUE] = BRICK_CHAR};

struct BoardLayout {
  int start_coords[2];
  uint8_t bonus_squares[BOARD_DIM * BOARD_DIM];
};

int board_layout_get_index(int row, int col) { return row * BOARD_DIM + col; }

uint8_t board_layout_get_bonus_square(const BoardLayout *bl, int row, int col) {
  return bl->bonus_squares[board_layout_get_index(row, col)];
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
    int lane_start_value =
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

uint8_t bonus_square_char_to_value(char bonus_square_char) {
  return bonus_square_chars_to_values_map[(int)bonus_square_char];
}

board_layout_load_status_t
board_layout_parse_split_file(BoardLayout *bl,
                              const StringSplitter *file_lines) {
  int number_of_rows = string_splitter_get_number_of_items(file_lines);

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
      char bonus_square_char = layout_row[col];
      int index = board_layout_get_index(row, col);
      uint8_t bonus_square_value =
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