#include "win_pct.h"

#include "../def/rack_defs.h"
#include "../util/fileproxy.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "data_filepaths.h"
#include "equity.h"
#include "win_pct_counts.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct WinPct {
  char *name;
  // One row per unseen count (original format) or per game state (see
  // win_pct_counts.h).
  float **win_pcts;
  int num_rows;
  // NULL for the original format.
  Equity *expected_swings;
  // Zero for the original format.
  unsigned int max_bag;
  int min_spread;
  int max_spread;
  int number_of_spreads;
  unsigned int max_tiles_unseen;
};

const char *win_pct_get_name(const WinPct *wp) { return wp->name; }

unsigned int win_pct_get_max_tiles_unseen(const WinPct *wp) {
  return wp->max_tiles_unseen;
}

static int win_pct_get_row_index(const WinPct *wp, unsigned int bag_tiles,
                                 unsigned int on_turn_rack_tiles,
                                 unsigned int off_turn_rack_tiles) {
  if (wp->expected_swings == NULL) {
    const unsigned int game_unseen_tiles = bag_tiles + off_turn_rack_tiles;
    if (game_unseen_tiles > wp->max_tiles_unseen) {
      log_fatal("cannot get win percentage value for %d unseen tiles when the "
                "maximum unseen tiles is %d",
                game_unseen_tiles, wp->max_tiles_unseen);
    }
    if (game_unseen_tiles == 0) {
      log_fatal("cannot get win percentage value for 0 unseen tiles when the "
                "minimum unseen tiles is 1");
    }
    return (int)game_unseen_tiles - 1;
  }
  if (bag_tiles > wp->max_bag) {
    log_fatal("cannot get win percentage value for %d tiles in the bag when "
              "the maximum is %d",
              bag_tiles, wp->max_bag);
  }
  return win_pct_get_cell_index((int)bag_tiles, (int)on_turn_rack_tiles,
                                (int)off_turn_rack_tiles);
}

float win_pct_get(const WinPct *wp, int spread_plus_leftover,
                  unsigned int bag_tiles, unsigned int on_turn_rack_tiles,
                  unsigned int off_turn_rack_tiles) {
  if (spread_plus_leftover > wp->max_spread) {
    spread_plus_leftover = wp->max_spread;
  }
  if (spread_plus_leftover < wp->min_spread) {
    spread_plus_leftover = wp->min_spread;
  }
  const int row_index = win_pct_get_row_index(wp, bag_tiles, on_turn_rack_tiles,
                                              off_turn_rack_tiles);
  return wp->win_pcts[row_index][wp->max_spread + spread_plus_leftover];
}

bool win_pct_has_expected_swing(const WinPct *wp) {
  return wp->expected_swings != NULL;
}

Equity win_pct_get_expected_swing(const WinPct *wp, unsigned int bag_tiles,
                                  unsigned int on_turn_rack_tiles,
                                  unsigned int off_turn_rack_tiles) {
  if (wp->expected_swings == NULL) {
    log_fatal("win percentage table '%s' does not predict the final margin",
              wp->name);
    return 0;
  }
  return wp->expected_swings[win_pct_get_row_index(
      wp, bag_tiles, on_turn_rack_tiles, off_turn_rack_tiles)];
}

// Pools a cell's samples into pooled_row.
static void pool_samples(const WinPctCounts *counts, int cell,
                         double *pooled_row) {
  const int row_length = win_pct_counts_get_row_length(counts);
  memset(pooled_row, 0, sizeof(double) * row_length);
  for (int sample = 0; sample < WIN_PCT_NUM_SAMPLES; sample++) {
    const double *row = win_pct_counts_get_const_row(counts, sample, cell);
    for (int value_idx = 0; value_idx < row_length; value_idx++) {
      pooled_row[value_idx] += row[value_idx];
    }
  }
}

// The cell whose data stands in for a cell with none: the nearest bag size
// with data (preferring fewer tiles) for a bag state, or -1, meaning all
// bag-empty states pooled, for a bag-empty state.
static int get_fallback_cell(const double *totals, int max_bag, int cell) {
  const int bag = win_pct_counts_get_cell_bag(cell);
  if (bag == 0) {
    return -1;
  }
  for (int distance = 1; distance < max_bag; distance++) {
    const int candidates[2] = {bag - distance, bag + distance};
    for (int candidate_idx = 0; candidate_idx < 2; candidate_idx++) {
      const int candidate_bag = candidates[candidate_idx];
      if (candidate_bag >= 1 && candidate_bag <= max_bag) {
        const int candidate_cell = win_pct_get_cell_index(candidate_bag, 0, 0);
        if (totals[candidate_cell] > 0.0) {
          return candidate_cell;
        }
      }
    }
  }
  return cell;
}

static void win_pct_create_from_counts(const char *win_pct_name,
                                       const char *win_pct_filename, WinPct *wp,
                                       const WinPctCounts *counts,
                                       ErrorStack *error_stack) {
  const int num_cells = win_pct_counts_get_num_cells(counts);
  const int row_length = win_pct_counts_get_row_length(counts);
  const int max_bag = win_pct_counts_get_max_bag(counts);
  double *pooled_rows = malloc_or_die(sizeof(double) * num_cells * row_length);
  double *totals = malloc_or_die(sizeof(double) * num_cells);
  double *bag_empty_row = calloc_or_die(row_length, sizeof(double));
  for (int cell = 0; cell < num_cells; cell++) {
    double *pooled_row = pooled_rows + (size_t)cell * row_length;
    pool_samples(counts, cell, pooled_row);
    totals[cell] = win_pct_counts_row_get_total(counts, pooled_row);
    if (win_pct_counts_get_cell_bag(cell) == 0) {
      for (int value_idx = 0; value_idx < row_length; value_idx++) {
        bag_empty_row[value_idx] += pooled_row[value_idx];
      }
    }
  }

  wp->max_bag = (unsigned int)max_bag;
  wp->max_tiles_unseen = (unsigned int)(max_bag + RACK_SIZE);
  wp->max_spread = win_pct_counts_get_max_spread(counts);
  wp->min_spread = -wp->max_spread;
  wp->number_of_spreads = 2 * wp->max_spread + 1;
  wp->num_rows = num_cells;
  wp->win_pcts = calloc_or_die(num_cells, sizeof(float *));
  wp->expected_swings = calloc_or_die(num_cells, sizeof(Equity));
  double *row_win_pcts = malloc_or_die(sizeof(double) * wp->number_of_spreads);
  for (int cell = 0; cell < num_cells && error_stack_is_empty(error_stack);
       cell++) {
    const double *row = pooled_rows + (size_t)cell * row_length;
    if (totals[cell] <= 0.0) {
      const int fallback_cell = get_fallback_cell(totals, max_bag, cell);
      if (fallback_cell < 0) {
        row = bag_empty_row;
      } else {
        row = pooled_rows + (size_t)fallback_cell * row_length;
      }
    }
    if (win_pct_counts_row_get_total(counts, row) <= 0.0) {
      error_stack_push(
          error_stack, ERROR_STATUS_WIN_PCT_NO_DATA_FOUND,
          get_formatted_string("no data for %s states in win percentage "
                               "file: %s",
                               win_pct_counts_get_cell_bag(cell) == 0
                                   ? "bag-empty"
                                   : "nonempty bag",
                               win_pct_filename));
      break;
    }
    win_pct_counts_row_get_win_pcts(counts, row, row_win_pcts);
    wp->win_pcts[cell] = malloc_or_die(sizeof(float) * wp->number_of_spreads);
    for (int spread_idx = 0; spread_idx < wp->number_of_spreads; spread_idx++) {
      wp->win_pcts[cell][spread_idx] = (float)row_win_pcts[spread_idx];
    }
    wp->expected_swings[cell] =
        double_to_equity(win_pct_counts_row_get_mean_swing(counts, row));
  }
  free(row_win_pcts);
  free(bag_empty_row);
  free(totals);
  free(pooled_rows);
  wp->name = string_duplicate(win_pct_name);
}

void win_pct_create_internal(const char *win_pct_name,
                             const char *win_pct_filename, WinPct *wp,
                             const StringSplitter *split_win_pct_contents,
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
  wp->num_rows = (int)wp->max_tiles_unseen;
  float **array =
      (float **)calloc_or_die(wp->max_tiles_unseen, sizeof(float *));

  // Read data lines
  StringSplitter *split_tiles_remaining_row = NULL;
  int prev_nonzero_total_games_index = -1;
  for (unsigned int tiles_unseen_index = 0;
       tiles_unseen_index < wp->max_tiles_unseen; tiles_unseen_index++) {
    split_tiles_remaining_row = split_string_by_whitespace(
        string_splitter_get_item(split_win_pct_contents,
                                 (int)tiles_unseen_index),
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
            (float)total_win_score /
            (float)(total_games_for_tiles_remaining * 2);
      }
      prev_nonzero_total_games_index = (int)tiles_unseen_index;
    }
    string_splitter_destroy(split_tiles_remaining_row);
    split_tiles_remaining_row = NULL;
  }
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
    for (int row_idx = 0; row_idx < wp->num_rows; row_idx++) {
      free(wp->win_pcts[row_idx]);
    }
  }
  free(wp->expected_swings);
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
    char *file_contents =
        fileproxy_get_string_from_filename(win_pct_filename, error_stack);
    if (error_stack_is_empty(error_stack) &&
        win_pct_counts_string_has_header(file_contents)) {
      WinPctCounts *counts = win_pct_counts_create_from_string(
          file_contents, win_pct_filename, error_stack);
      if (error_stack_is_empty(error_stack)) {
        wp = calloc_or_die(1, sizeof(WinPct));
        win_pct_create_from_counts(win_pct_name, win_pct_filename, wp, counts,
                                   error_stack);
      }
      win_pct_counts_destroy(counts);
    } else if (error_stack_is_empty(error_stack)) {
      StringSplitter *split_win_pct_contents =
          split_string_by_newline(file_contents, error_stack);
      if (error_stack_is_empty(error_stack)) {
        wp = calloc_or_die(1, sizeof(WinPct));
        win_pct_create_internal(win_pct_name, win_pct_filename, wp,
                                split_win_pct_contents, error_stack);
      }
      string_splitter_destroy(split_win_pct_contents);
    }
    free(file_contents);
  }
  free(win_pct_filename);
  if (!error_stack_is_empty(error_stack)) {
    win_pct_destroy(wp);
    wp = NULL;
  }
  return wp;
}

bool is_win_pct_at_upper_extreme(const double wp, const double cutoff) {
  return wp >= (1.0 - cutoff);
}

bool is_win_pct_at_lower_extreme(const double wp, const double cutoff) {
  return wp <= cutoff;
}

bool is_win_pct_within_cutoff(const double win_pct, const double cutoff) {
  return is_win_pct_at_lower_extreme(win_pct, cutoff) ||
         is_win_pct_at_upper_extreme(win_pct, cutoff);
}

bool are_win_pcts_within_cutoff_or_equal(const double wp1, const double wp2,
                                         const double cutoff) {
  return (is_win_pct_at_lower_extreme(wp1, cutoff) &&
          is_win_pct_at_lower_extreme(wp2, cutoff)) ||
         (is_win_pct_at_upper_extreme(wp1, cutoff) &&
          is_win_pct_at_upper_extreme(wp2, cutoff)) ||
         wp1 == wp2;
}

double convert_cutoff_to_user_cutoff(const double cutoff) {
  return cutoff * 100.0;
}

double convert_user_cutoff_to_cutoff(const double user_cutoff) {
  return user_cutoff / 100.0;
}