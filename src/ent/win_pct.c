#include "win_pct.h"

#include <stdbool.h>
#include <stdlib.h>

#include "data_filepaths.h"

#include "../util/log.h"
#include "../util/string_util.h"
#include "../util/util.h"

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

WinPct *win_pct_create(const char *data_path, const char *win_pct_name) {
  WinPct *wp = malloc_or_die(sizeof(WinPct));

  wp->name = string_duplicate(win_pct_name);

  char *win_pct_filename =
      data_filepaths_get(data_path, win_pct_name, DATA_FILEPATH_TYPE_WIN_PCT);
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

  return wp;
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