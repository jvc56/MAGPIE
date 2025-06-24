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
    log_fatal("cannot get win percentage value for %d unseen tiles when the "
              "maximum unseen tiles is %d",
              game_unseen_tiles, wp->max_tiles_unseen);
  }
  return wp
      ->win_pcts[game_unseen_tiles - 1][wp->max_spread + spread_plus_leftover];
};

void win_pct_create_internal(const char *win_pct_name,
                             const char *win_pct_filename, WinPct *wp,
                             StringSplitter *split_win_pct_contents,
                             ErrorStack *error_stack) {
  wp->max_tiles_unseen =
      string_splitter_get_number_of_items(split_win_pct_contents);

  if (wp->max_tiles_unseen < 1) {
    error_stack_push(
        error_stack, ERROR_STATUS_WIN_PCT_NO_DATA_FOUND,
        get_formatted_string("no data found in win percentage file: %s\n",
                             win_pct_filename));
    return;
  }

  // Allocate memory for the 2D array
  float **array =
      (float **)malloc_or_die(wp->max_tiles_unseen * sizeof(float *));

  // Read data lines
  StringSplitter *split_tiles_remaining_row = NULL;
  int prev_nonzero_total_games_index = -1;
  for (unsigned int tiles_unseen_index = 0;
       tiles_unseen_index < wp->max_tiles_unseen; tiles_unseen_index++) {
    split_tiles_remaining_row = split_string_by_whitespace(
        string_splitter_get_item(split_win_pct_contents, tiles_unseen_index),
        true);
    int num_spreads_in_row =
        string_splitter_get_number_of_items(split_tiles_remaining_row) - 1;

    if (tiles_unseen_index == 0) {
      if (num_spreads_in_row % 2 != 1) {
        error_stack_push(
            error_stack, ERROR_STATUS_WIN_PCT_INVALID_NUMBER_OF_COLUMNS,
            get_formatted_string("invalid number of columns in '%s' at line %d "
                                 "(expected odd number)",
                                 win_pct_name, tiles_unseen_index + 1));
        break;
      }
      wp->max_spread = num_spreads_in_row / 2;
      wp->min_spread = -wp->max_spread;
      wp->number_of_spreads = num_spreads_in_row;
      for (unsigned int j = 0; j < wp->max_tiles_unseen; j++) {
        array[j] =
            (float *)malloc_or_die(wp->number_of_spreads * sizeof(float));
      }
    } else if (num_spreads_in_row != wp->number_of_spreads) {
      error_stack_push(
          error_stack, ERROR_STATUS_WIN_PCT_INVALID_NUMBER_OF_COLUMNS,
          get_formatted_string("inconsistent number of columns in '%s' at line "
                               "%d (found %d but expected %d)",
                               win_pct_name, tiles_unseen_index + 1,
                               num_spreads_in_row, wp->number_of_spreads));
      break;
    }

    uint64_t total_games_for_tiles_remaining = string_to_uint64(
        string_splitter_get_item(split_tiles_remaining_row, 0), error_stack);

    if (!error_stack_is_empty(error_stack)) {
      error_stack_push(
          error_stack, ERROR_STATUS_WIN_PCT_INVALID_TOTAL_GAMES,
          get_formatted_string(
              "invalid total games '%s' for %d tiles remaining in win "
              "percentage file",
              string_splitter_get_item(split_tiles_remaining_row, 0),
              tiles_unseen_index + 1));
      break;
    }

    if (total_games_for_tiles_remaining == 0) {
      if (prev_nonzero_total_games_index < 0) {
        error_stack_push(
            error_stack, ERROR_STATUS_WIN_PCT_INVALID_TOTAL_GAMES,
            get_formatted_string(
                "cannot have zero total games (for %d tiles remaining) when "
                "all previous totals are also zero",
                tiles_unseen_index + 1));
        break;
      }
      memcpy(array[tiles_unseen_index], array[prev_nonzero_total_games_index],
             num_spreads_in_row * sizeof(float));
    } else {
      for (int spread_index = 0; spread_index < num_spreads_in_row;
           spread_index++) {
        // Use +1 to ignore the total games column
        const uint64_t total_win_score =
            string_to_uint64(string_splitter_get_item(split_tiles_remaining_row,
                                                      spread_index + 1),
                             error_stack);
        if (!error_stack_is_empty(error_stack)) {
          error_stack_push(
              error_stack, ERROR_STATUS_WIN_PCT_INVALID_TOTAL_WINS,
              get_formatted_string(
                  "invalid total wins score '%s' for %d tiles remaining in win "
                  "percentage file",
                  string_splitter_get_item(split_tiles_remaining_row, 0),
                  tiles_unseen_index + 1));
          break;
        }
        array[tiles_unseen_index][spread_index] =
            total_win_score / (float)(total_games_for_tiles_remaining * 2);
      }
      prev_nonzero_total_games_index = tiles_unseen_index;
    }
    string_splitter_destroy(split_tiles_remaining_row);
    split_tiles_remaining_row = NULL;
\  }
  string_splitter_destroy(split_tiles_remaining_row);
  wp->win_pcts = array;
  wp->name = string_duplicate(win_pct_name);
}

// Function to free the memory allocated for the 2D array
void win_pct_destroy(WinPct *wp) {
  if (!wp) {
    return;
  }
  if (wp->win_pcts) {
    for (unsigned int i = 0; i < wp->max_tiles_unseen; i++) {
      free(wp->win_pcts[i]);
    }
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
    StringSplitter *split_win_pct_contents =
        split_file_by_newline(win_pct_filename, error_stack);
    if (error_stack_is_empty(error_stack)) {
      wp = calloc_or_die(1, sizeof(WinPct));
      win_pct_create_internal(win_pct_name, win_pct_filename, wp,
                              split_win_pct_contents, error_stack);
    }
    string_splitter_destroy(split_win_pct_contents);
  }
  free(win_pct_filename);
  if (!error_stack_is_empty(error_stack)) {
    win_pct_destroy(wp);
    wp = NULL;
  }
  return wp;
}