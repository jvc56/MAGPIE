#include "peg_adjust.h"

#include "../def/rack_defs.h"
#include "../def/static_eval_defs.h"
#include "../util/fileproxy.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "data_filepaths.h"
#include "equity.h"
#include <stdbool.h>
#include <stdlib.h>

struct PegAdjust {
  char *name;
  Equity values[PEG_ADJUST_VALUES_LENGTH];
};

PegAdjust *
peg_adjust_create_from_values(const char *name,
                              const Equity values[PEG_ADJUST_VALUES_LENGTH]) {
  PegAdjust *peg_adjust = malloc_or_die(sizeof(PegAdjust));
  peg_adjust->name = string_duplicate(name);
  for (int index = 0; index < PEG_ADJUST_VALUES_LENGTH; index++) {
    peg_adjust->values[index] = values[index];
  }
  return peg_adjust;
}

void peg_adjust_destroy(PegAdjust *peg_adjust) {
  if (!peg_adjust) {
    return;
  }
  free(peg_adjust->name);
  free(peg_adjust);
}

static void push_row_error(ErrorStack *error_stack, const char *filename,
                           int row_number, const char *reason) {
  error_stack_push(
      error_stack, ERROR_STATUS_PEG_ADJUST_INVALID_ROW,
      get_formatted_string("pre-endgame adjustment file %s, row %d: %s",
                           filename, row_number, reason));
}

// Reads one "index,value" row into values, marking its index as given.
static void parse_row(const char *row, const char *filename, int row_number,
                      Equity values[PEG_ADJUST_VALUES_LENGTH],
                      bool given[PEG_ADJUST_VALUES_LENGTH],
                      ErrorStack *error_stack) {
  StringSplitter *fields = split_string(row, ',', false);
  if (string_splitter_get_number_of_items(fields) != 2) {
    push_row_error(error_stack, filename, row_number,
                   "expected two fields, an index and a value");
    string_splitter_destroy(fields);
    return;
  }
  const int index =
      string_to_int(string_splitter_get_item(fields, 0), error_stack);
  if (!error_stack_is_empty(error_stack)) {
    push_row_error(error_stack, filename, row_number, "invalid index");
    string_splitter_destroy(fields);
    return;
  }
  if (index < 0 || index >= PEG_ADJUST_VALUES_LENGTH) {
    push_row_error(error_stack, filename, row_number, "index out of range");
    string_splitter_destroy(fields);
    return;
  }
  const double value =
      string_to_double(string_splitter_get_item(fields, 1), error_stack);
  string_splitter_destroy(fields);
  if (!error_stack_is_empty(error_stack)) {
    push_row_error(error_stack, filename, row_number, "invalid value");
    return;
  }
  if (given[index]) {
    push_row_error(error_stack, filename, row_number, "index given twice");
    return;
  }
  given[index] = true;
  values[index] = double_to_equity(value);
}

PegAdjust *peg_adjust_create(const char *data_paths, const char *name,
                             ErrorStack *error_stack) {
  char *filename = data_filepaths_get_readable_filename(
      data_paths, name, DATA_FILEPATH_TYPE_PEG_ADJUST, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    free(filename);
    return NULL;
  }
  char *contents = fileproxy_get_string_from_filename(filename, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    free(contents);
    free(filename);
    return NULL;
  }
  Equity values[PEG_ADJUST_VALUES_LENGTH] = {0};
  bool given[PEG_ADJUST_VALUES_LENGTH] = {false};
  StringSplitter *rows = split_string_by_newline(contents, false);
  const int number_of_rows = string_splitter_get_number_of_items(rows);
  for (int row_idx = 0;
       row_idx < number_of_rows && error_stack_is_empty(error_stack);
       row_idx++) {
    char *row = string_duplicate(string_splitter_get_item(rows, row_idx));
    trim_whitespace(row);
    if (row[0] != '\0' && row[0] != '#') {
      parse_row(row, filename, row_idx + 1, values, given, error_stack);
    }
    free(row);
  }
  string_splitter_destroy(rows);
  free(contents);
  free(filename);
  if (!error_stack_is_empty(error_stack)) {
    return NULL;
  }
  return peg_adjust_create_from_values(name, values);
}

const char *peg_adjust_get_name(const PegAdjust *peg_adjust) {
  return peg_adjust->name;
}

Equity peg_adjust_get_value(const PegAdjust *peg_adjust, int index) {
  return peg_adjust->values[index];
}

Equity peg_adjust_get_placement_value(const PegAdjust *peg_adjust,
                                      int number_of_tiles_in_bag,
                                      int tiles_played) {
  if (!peg_adjust || number_of_tiles_in_bag == 0) {
    return 0;
  }
  const int index = number_of_tiles_in_bag - tiles_played + RACK_SIZE;
  if (index >= PEG_ADJUST_VALUES_LENGTH) {
    return 0;
  }
  return peg_adjust->values[index];
}

void peg_adjust_fill_by_tiles_played(const PegAdjust *peg_adjust,
                                     int number_of_tiles_in_bag,
                                     Equity by_tiles_played[RACK_SIZE + 1]) {
  by_tiles_played[0] = 0;
  for (int tiles_played = 1; tiles_played <= RACK_SIZE; tiles_played++) {
    by_tiles_played[tiles_played] = peg_adjust_get_placement_value(
        peg_adjust, number_of_tiles_in_bag, tiles_played);
  }
}
