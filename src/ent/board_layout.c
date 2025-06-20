#include "board_layout.h"

#include <stdint.h>
#include <stdio.h>

#include "../def/board_defs.h"

#include "bonus_square.h"

#include "data_filepaths.h"

#include "../util/io_util.h"
#include "../util/string_util.h"

struct BoardLayout {
  char *name;
  int start_coords[2];
  BonusSquare bonus_squares[BOARD_DIM * BOARD_DIM];
};

int board_layout_get_index(int row, int col) { return row * BOARD_DIM + col; }

BonusSquare board_layout_get_bonus_square(const BoardLayout *bl, int row,
                                          int col) {
  return bl->bonus_squares[board_layout_get_index(row, col)];
}

int board_layout_get_start_coord(const BoardLayout *bl, int index) {
  return bl->start_coords[index];
}

BoardLayout *board_layout_create(void) {
  BoardLayout *bl = calloc_or_die(1, sizeof(BoardLayout));
  return bl;
}

void board_layout_parse_split_start_coords(
    int start_coords[2], const StringSplitter *starting_coords,
    ErrorStack *error_stack) {

  if (string_splitter_get_number_of_items(starting_coords) != 2) {
    error_stack_push(
        error_stack, ERROR_STATUS_BOARD_LAYOUT_MALFORMED_START_COORDS,
        string_duplicate(
            "invalid starting coordinates, expected 2 comma separated values"));
    return;
  }

  for (int i = 0; i < 2; i++) {
    const int lane_start_value = string_to_int(
        string_splitter_get_item(starting_coords, i), error_stack);
    if (!error_stack_is_empty(error_stack)) {
      error_stack_push(error_stack,
                       ERROR_STATUS_BOARD_LAYOUT_MALFORMED_START_COORDS,
                       get_formatted_string("invalid starting coordinate '%d'",
                                            lane_start_value));
      return;
    }
    if (lane_start_value < 0 || lane_start_value >= BOARD_DIM) {
      error_stack_push(
          error_stack, ERROR_STATUS_BOARD_LAYOUT_OUT_OF_BOUNDS_START_COORDS,
          get_formatted_string("starting coordinate out of bounds: %d",
                               lane_start_value));
      return;
    }
    start_coords[i] = lane_start_value;
  }
}

void board_layout_parse_split_file(BoardLayout *bl,
                                   const char *board_layout_name,
                                   const StringSplitter *file_lines,
                                   ErrorStack *error_stack) {
  const int number_of_rows = string_splitter_get_number_of_items(file_lines);

  if (number_of_rows != BOARD_DIM + 1) {
    error_stack_push(
        error_stack, ERROR_STATUS_BOARD_LAYOUT_INVALID_NUMBER_OF_ROWS,
        get_formatted_string("invalid number of rows in the board layout file, "
                             "expected the first row to be the starting "
                             "coordinates and the rest to be the board for a "
                             "total of %d rows, got: %d",
                             BOARD_DIM + 1, number_of_rows));
    return;
  }

  StringSplitter *starting_coords =
      split_string(string_splitter_get_item(file_lines, 0), ',', true);

  int tmp_start_coords[2];

  board_layout_parse_split_start_coords(tmp_start_coords, starting_coords,
                                        error_stack);

  string_splitter_destroy(starting_coords);

  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  BonusSquare tmp_bonus_squares[BOARD_DIM * BOARD_DIM];
  for (int row = 0; row < BOARD_DIM; row++) {
    const char *layout_row = string_splitter_get_item(file_lines, row + 1);
    if (string_length(layout_row) != BOARD_DIM) {
      error_stack_push(error_stack,
                       ERROR_STATUS_BOARD_LAYOUT_INVALID_NUMBER_OF_COLS,
                       get_formatted_string(
                           "board layout has an invalid number of columns: %d",
                           string_length(layout_row)));
      return;
    }
    for (int col = 0; col < BOARD_DIM; col++) {
      const char bonus_square_char = layout_row[col];
      const int index = board_layout_get_index(row, col);
      const BonusSquare bonus_square =
          bonus_square_from_char(bonus_square_char);
      if (bonus_square_is_invalid(bonus_square)) {
        error_stack_push(
            error_stack, ERROR_STATUS_BOARD_LAYOUT_INVALID_BONUS_SQUARE,
            get_formatted_string("encountered invalid bonus square: %c",
                                 bonus_square_char));
        return;
      }
      tmp_bonus_squares[index] = bonus_square;
    }
  }
  memcpy(bl->bonus_squares, tmp_bonus_squares, sizeof(tmp_bonus_squares));
  memcpy(bl->start_coords, tmp_start_coords, sizeof(tmp_start_coords));
  free(bl->name);
  bl->name = string_duplicate(board_layout_name);
}

void board_layout_load(BoardLayout *bl, const char *data_paths,
                       const char *board_layout_name, ErrorStack *error_stack) {
  char *layout_filename = data_filepaths_get_readable_filename(
      data_paths, board_layout_name, DATA_FILEPATH_TYPE_LAYOUT, error_stack);
  if (error_stack_is_empty(error_stack)) {
    StringSplitter *layout_rows =
        split_file_by_newline(layout_filename, error_stack);
    if (error_stack_is_empty(error_stack)) {
      board_layout_parse_split_file(bl, board_layout_name, layout_rows,
                                    error_stack);
    }
    string_splitter_destroy(layout_rows);
  }
  free(layout_filename);
}

char *board_layout_get_default_name(void) {
  return get_formatted_string("standard%d", BOARD_DIM);
}

const char *board_layout_get_name(const BoardLayout *bl) { return bl->name; }

bool board_layout_is_name_default(const char *board_layout_name) {
  char *default_layout_name = board_layout_get_default_name();
  bool is_default = strings_equal(default_layout_name, board_layout_name);
  free(default_layout_name);
  return is_default;
}

BoardLayout *board_layout_create_default(const char *data_paths,
                                         ErrorStack *error_stack) {
  BoardLayout *bl = board_layout_create();
  char *default_layout_name = board_layout_get_default_name();
  board_layout_load(bl, data_paths, default_layout_name, error_stack);
  free(default_layout_name);
  if (!error_stack_is_empty(error_stack)) {
    board_layout_destroy(bl);
    bl = NULL;
  }
  return bl;
}

void board_layout_destroy(BoardLayout *bl) {
  if (!bl) {
    return;
  }
  free(bl->name);
  free(bl);
}