#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fileproxy.h"
#include "log.h"
#include "util.h"
#include "winpct.h"

extern inline float win_pct(WinPct *wp, int spread_plus_leftover,
                            unsigned int tiles_unseen);

char *get_win_pct_filepath(const char *win_pct_name) {
  // Check for invalid inputs
  if (!win_pct_name) {
    log_fatal("win percentage name is null");
  }
  return get_formatted_string("%s%s%s%s", WIN_PCT_FILEPATH,
                              WIN_PCT_DEFAULT_ENGLISH_DIR, win_pct_name,
                              WIN_PCT_FILE_EXTENSION);
}

// note: this function was largely written by ChatGPT.
void parse_winpct_csv(WinPct *wp, const char *win_pct_name) {
  const char *win_pct_filename = get_win_pct_filepath(win_pct_name);
  FILE *file = stream_from_filename(win_pct_filename);
  if (!file) {
    log_fatal("Error opening file: %s\n", win_pct_filename);
  }
  free(win_pct_filename);
  int max_rows = MAX_SPREAD * 2 + 1;
  // Allocate memory for the 2D array
  float **array = (float **)malloc_or_die(max_rows * sizeof(float *));
  for (int i = 0; i < max_rows; i++) {
    array[i] = (float *)malloc_or_die(MAX_COLS * sizeof(float));
  }

  // Read and process the CSV file
  // Assuming each field is no longer than 10 characters
  char line[MAX_COLS * 10];

  // Read the header line
  fgets(line, sizeof(line), file);

  // Read data lines
  int row = 0;
  while (fgets(line, sizeof(line), file) && row < max_rows) {
    StringSplitter *win_pct_data = split_string(line, ',', true);
    int number_of_items = string_splitter_get_number_of_items(win_pct_data);
    // Start at 1 to ignore the first column.
    for (int i = 1; i < number_of_items; i++) {
      array[row][i - 1] =
          string_to_double(string_splitter_get_item(win_pct_data, i));
    }
    destroy_string_splitter(win_pct_data);
    row++;
  }

  fclose(file);
  wp->win_pcts = array;

  wp->max_spread = 300;
  wp->min_spread = -300;
  wp->max_tiles_unseen = 93;
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
  for (int i = 0; i < MAX_SPREAD * 2 + 1; i++) {
    free(wp->win_pcts[i]);
  }
  free(wp->win_pcts);
  free(wp);
}