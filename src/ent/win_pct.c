#include "win_pct.h"

#include <stdbool.h>
#include <stdlib.h>

#include "data_filepaths.h"

#include "../util/io_util.h"
#include "../util/string_util.h"

struct WinPct {
  char *name;
  float **win_pcts;
  int min_spread;
  int max_spread;
  int number_of_spreads;
  unsigned int max_tiles_unseen;
};

const char *win_pct_get_name(WinPct *wp) { return wp->name; }

float win_pct_get(const WinPct *wp, int spread_plus_leftover,
                  unsigned int game_unseen_tiles) {
  if (spread_plus_leftover > wp->max_spread) {
    spread_plus_leftover = wp->max_spread;
  }
  if (spread_plus_leftover < wp->min_spread) {
    spread_plus_leftover = wp->min_spread;
  }
  if (game_unseen_tiles > wp->max_tiles_unseen) {
    game_unseen_tiles = wp->max_tiles_unseen;
  }
  return wp->win_pcts[wp->max_spread - spread_plus_leftover][game_unseen_tiles];
};

void win_pct_create_internal(const char *win_pct_name,
                             const char *win_pct_filename, WinPct *wp,
                             StringSplitter *win_pct_lines,
                             ErrorStack *error_stack) {
  // Use -1 to account for header line
  wp->number_of_spreads =
      string_splitter_get_number_of_items(win_pct_lines) - 1;

  if (wp->number_of_spreads < 1) {
    error_stack_push(
        error_stack, ERROR_STATUS_WIN_PCT_NO_DATA_FOUND,
        get_formatted_string("no data found in win percentage file: %s\n",
                             win_pct_filename));
    return;
  }

  // Allocate memory for the 2D array
  float **array =
      (float **)malloc_or_die(wp->number_of_spreads * sizeof(float *));

  // Read data lines
  int row = 0;
  int number_of_columns = 0;
  // Start at 1 to skip header line
  StringSplitter *win_pct_data = NULL;
  for (int i = 0; i < wp->number_of_spreads; i++) {
    // Use +1 to skip the header line
    win_pct_data =
        split_string(string_splitter_get_item(win_pct_lines, i + 1), ',', true);
    int number_of_items = string_splitter_get_number_of_items(win_pct_data);

    if (i == 0) {
      number_of_columns = number_of_items;
      for (int j = 0; j < wp->number_of_spreads; j++) {
        array[j] = (float *)malloc_or_die(number_of_columns * sizeof(float));
      }
    } else if (number_of_items != number_of_columns) {
      error_stack_push(
          error_stack, ERROR_STATUS_WIN_PCT_INVALID_NUMBER_OF_COLUMNS,
          get_formatted_string("inconsistent number of columns in '%s' at line "
                               "%d (found %d but expected %d)",
                               win_pct_name, i, number_of_items,
                               number_of_columns));
      break;
    }

    // We assume the spread values are continuous and descending
    int spread =
        string_to_int(string_splitter_get_item(win_pct_data, 0), error_stack);
    if (!error_stack_is_empty(error_stack)) {
      error_stack_push(
          error_stack, ERROR_STATUS_WIN_PCT_INVALID_SPREAD,
          get_formatted_string("invalid win percentage spread value '%s'",
                               string_splitter_get_item(win_pct_data, 0)));
      break;
    }
    if (i == 0) {
      wp->max_spread = spread;
    } else if (i == wp->number_of_spreads - 1) {
      wp->min_spread = spread;
    }

    // Start at 1 to ignore the first column.
    for (int j = 1; j < number_of_items; j++) {
      const double win_fraction = string_to_double(
          string_splitter_get_item(win_pct_data, j), error_stack);
      if (!error_stack_is_empty(error_stack)) {
        error_stack_push(
            error_stack, ERROR_STATUS_WIN_PCT_INVALID_DECIMAL,
            get_formatted_string("invalid win percentage decimal value '%s'",
                                 string_splitter_get_item(win_pct_data, 0)));
        break;
      }
      array[row][j - 1] = win_fraction;
    }
    string_splitter_destroy(win_pct_data);
    win_pct_data = NULL;
    row++;
  }
  string_splitter_destroy(win_pct_data);
  wp->win_pcts = array;
  wp->max_tiles_unseen = number_of_columns - 2;
  wp->name = string_duplicate(win_pct_name);
}

// Function to free the memory allocated for the 2D array
void win_pct_destroy(WinPct *wp) {
  if (!wp) {
    return;
  }
  for (int i = 0; i < wp->number_of_spreads; i++) {
    free(wp->win_pcts[i]);
  }
  free(wp->name);
  free(wp->win_pcts);
  free(wp);
}

WinPct *win_pct_create(const char *data_paths, const char *win_pct_name,
                       ErrorStack *error_stack) {
  char *win_pct_filename = data_filepaths_get_readable_filename(
      data_paths, win_pct_name, DATA_FILEPATH_TYPE_WIN_PCT, error_stack);
  WinPct *wp = NULL;
  if (error_stack_is_empty(error_stack)) {
    StringSplitter *win_pct_lines =
        split_file_by_newline(win_pct_filename, error_stack);
    if (error_stack_is_empty(error_stack)) {
      wp = calloc_or_die(1, sizeof(WinPct));
      win_pct_create_internal(win_pct_name, win_pct_filename, wp, win_pct_lines,
                              error_stack);
    }
    string_splitter_destroy(win_pct_lines);
  }
  free(win_pct_filename);
  if (!error_stack_is_empty(error_stack)) {
    win_pct_destroy(wp);
    wp = NULL;
  }
  return wp;
}