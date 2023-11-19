#include <stdio.h>
#include <stdlib.h>

#include "fileproxy.h"
#include "log.h"
#include "util.h"
#include "winpct.h"

extern inline float win_pct(const WinPct *wp, int spread_plus_leftover,
                            unsigned int tiles_unseen);

char *get_win_pct_filepath(const char *win_pct_name) {
  // Check for invalid inputs
  if (!win_pct_name) {
    log_fatal("win percentage name is null");
  }
  return get_formatted_string("%s%s%s", WIN_PCT_FILEPATH, win_pct_name,
                              WIN_PCT_FILE_EXTENSION);
}

void parse_winpct_csv(WinPct *wp, const char *win_pct_name) {
  char *win_pct_filename = get_win_pct_filepath(win_pct_name);
  StringSplitter *win_pct_lines = split_file_by_newline(win_pct_filename);
  free(win_pct_filename);

  // Use -1 to account for header line
  wp->number_of_spreads =
      string_splitter_get_number_of_items(win_pct_lines) - 1;

  if (wp->number_of_spreads < 1) {
    log_fatal("no data found in win percentage file: %s\n", win_pct_name);
  }

  // Allocate memory for the 2D array
  float **array =
      (float **)malloc_or_die(wp->number_of_spreads * sizeof(float *));

  // Read data lines
  int row = 0;
  int number_of_columns = 0;
  // Start at 1 to skip header line
  for (int i = 0; i < wp->number_of_spreads; i++) {
    // Use +1 to skip the header line
    StringSplitter *win_pct_data =
        split_string(string_splitter_get_item(win_pct_lines, i + 1), ',', true);
    int number_of_items = string_splitter_get_number_of_items(win_pct_data);

    if (i == 0) {
      number_of_columns = number_of_items;
      for (int i = 0; i < wp->number_of_spreads; i++) {
        array[i] = (float *)malloc_or_die(number_of_columns * sizeof(float));
      }
    } else if (number_of_items != number_of_columns) {
      log_fatal("inconsistent number of columns in %s at line %d: %d != %d\n",
                win_pct_name, i, number_of_columns, number_of_items);
    }

    // We assume the spread values are continuous and descending
    if (i == 0) {
      int spread = string_to_int(string_splitter_get_item(win_pct_data, 0));
      wp->max_spread = spread;
    } else if (i == wp->number_of_spreads - 1) {
      int spread = string_to_int(string_splitter_get_item(win_pct_data, 0));
      wp->min_spread = spread;
    }

    // Start at 1 to ignore the first column.
    for (int i = 1; i < number_of_items; i++) {
      array[row][i - 1] =
          string_to_double(string_splitter_get_item(win_pct_data, i));
    }
    destroy_string_splitter(win_pct_data);
    row++;
  }
  destroy_string_splitter(win_pct_lines);

  wp->win_pcts = array;
  wp->max_tiles_unseen = number_of_columns - 2;
}

WinPct *create_winpct(const char *winpct_name) {
  WinPct *wp = malloc_or_die(sizeof(WinPct));
  parse_winpct_csv(wp, winpct_name);
  return wp;
}

// Function to free the memory allocated for the 2D array
void destroy_winpct(WinPct *wp) {
  if (!wp) {
    return;
  }
  for (int i = 0; i < wp->number_of_spreads; i++) {
    free(wp->win_pcts[i]);
  }
  free(wp->win_pcts);
  free(wp);
}