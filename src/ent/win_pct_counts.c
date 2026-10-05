#include "win_pct_counts.h"

#include "../def/rack_defs.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

enum {
  WIN_PCT_COUNTS_HEADER_FIELDS = 5,
  WIN_PCT_COUNTS_ROW_KEY_FIELDS = 4,
};

static const char *const WIN_PCT_COUNTS_MAGIC = "winpct";

struct WinPctCounts {
  int max_bag;
  int max_spread;
  int num_cells;
  int num_bins;
  int row_length;
  // [sample][cell][row_length]
  double *rows;
};

WinPctCounts *win_pct_counts_create(int max_bag, int max_spread) {
  WinPctCounts *counts = malloc_or_die(sizeof(WinPctCounts));
  counts->max_bag = max_bag;
  counts->max_spread = max_spread;
  counts->num_cells = WIN_PCT_NUM_BAG_EMPTY_CELLS + max_bag;
  counts->num_bins = 2 * max_spread + 3;
  counts->row_length = counts->num_bins + 1;
  counts->rows = calloc_or_die((size_t)WIN_PCT_NUM_SAMPLES * counts->num_cells *
                                   counts->row_length,
                               sizeof(double));
  return counts;
}

WinPctCounts *win_pct_counts_duplicate(const WinPctCounts *counts) {
  WinPctCounts *copy =
      win_pct_counts_create(counts->max_bag, counts->max_spread);
  memcpy(copy->rows, counts->rows,
         sizeof(double) * WIN_PCT_NUM_SAMPLES * counts->num_cells *
             counts->row_length);
  return copy;
}

void win_pct_counts_destroy(WinPctCounts *counts) {
  if (!counts) {
    return;
  }
  free(counts->rows);
  free(counts);
}

int win_pct_counts_get_max_bag(const WinPctCounts *counts) {
  return counts->max_bag;
}

int win_pct_counts_get_max_spread(const WinPctCounts *counts) {
  return counts->max_spread;
}

int win_pct_counts_get_num_cells(const WinPctCounts *counts) {
  return counts->num_cells;
}

int win_pct_counts_get_num_bins(const WinPctCounts *counts) {
  return counts->num_bins;
}

int win_pct_counts_get_row_length(const WinPctCounts *counts) {
  return counts->row_length;
}

int win_pct_counts_get_cell_bag(int cell_index) {
  if (cell_index < WIN_PCT_NUM_BAG_EMPTY_CELLS) {
    return 0;
  }
  return cell_index - WIN_PCT_NUM_BAG_EMPTY_CELLS + 1;
}

// cppcheck-suppress constParameterPointer
double *win_pct_counts_get_row(WinPctCounts *counts, int sample, int cell) {
  return counts->rows +
         ((size_t)sample * counts->num_cells + cell) * counts->row_length;
}

const double *win_pct_counts_get_const_row(const WinPctCounts *counts,
                                           int sample, int cell) {
  return counts->rows +
         ((size_t)sample * counts->num_cells + cell) * counts->row_length;
}

void win_pct_counts_add_swing(WinPctCounts *counts, int sample, int cell,
                              int swing) {
  double *row = win_pct_counts_get_row(counts, sample, cell);
  int bin = swing + counts->max_spread + 1;
  if (bin < 0) {
    bin = 0;
  } else if (bin >= counts->num_bins) {
    bin = counts->num_bins - 1;
  }
  row[bin] += 1.0;
  row[counts->num_bins] += (double)swing;
}

bool win_pct_counts_have_same_shape(const WinPctCounts *counts1,
                                    const WinPctCounts *counts2) {
  return counts1->max_bag == counts2->max_bag &&
         counts1->max_spread == counts2->max_spread;
}

void win_pct_counts_add(WinPctCounts *dst, const WinPctCounts *src) {
  if (!win_pct_counts_have_same_shape(dst, src)) {
    log_fatal("cannot add win percentage counts of different shapes");
  }
  const size_t num_values =
      (size_t)WIN_PCT_NUM_SAMPLES * dst->num_cells * dst->row_length;
  for (size_t value_idx = 0; value_idx < num_values; value_idx++) {
    dst->rows[value_idx] += src->rows[value_idx];
  }
}

void win_pct_counts_reset(WinPctCounts *counts) {
  memset(counts->rows, 0,
         sizeof(double) * WIN_PCT_NUM_SAMPLES * counts->num_cells *
             counts->row_length);
}

double win_pct_counts_row_get_total(const WinPctCounts *counts,
                                    const double *row) {
  double total = 0.0;
  for (int bin_idx = 0; bin_idx < counts->num_bins; bin_idx++) {
    total += row[bin_idx];
  }
  return total;
}

double win_pct_counts_row_get_mean_swing(const WinPctCounts *counts,
                                         const double *row) {
  const double total = win_pct_counts_row_get_total(counts, row);
  if (total <= 0.0) {
    return 0.0;
  }
  return row[counts->num_bins] / total;
}

void win_pct_counts_row_get_win_pcts(const WinPctCounts *counts,
                                     const double *row, double *win_pcts) {
  const double total = win_pct_counts_row_get_total(counts, row);
  // The on-turn player, ahead by spread, wins when the swing exceeds -spread
  // and ties when it equals it, which is bin max_spread + 1 - spread. Walk
  // spreads upward so the tie bin walks downward, starting above it with just
  // the top tail bin.
  const int num_spreads = 2 * counts->max_spread + 1;
  double above = row[counts->num_bins - 1];
  for (int spread_idx = 0; spread_idx < num_spreads; spread_idx++) {
    const int tie_bin = counts->num_bins - 2 - spread_idx;
    win_pcts[spread_idx] = (above + 0.5 * row[tie_bin]) / total;
    above += row[tie_bin];
  }
}

bool win_pct_counts_string_has_header(const char *contents) {
  const char *start = contents + strspn(contents, " \t\r\n");
  const size_t magic_length = strlen(WIN_PCT_COUNTS_MAGIC);
  return strncmp(start, WIN_PCT_COUNTS_MAGIC, magic_length) == 0 &&
         (start[magic_length] == ' ' || start[magic_length] == '\t');
}

static void push_row_error(ErrorStack *error_stack, const char *filename,
                           int line_idx, const char *reason) {
  error_stack_push(error_stack, ERROR_STATUS_WIN_PCT_INVALID_ROW,
                   get_formatted_string("%s in win percentage file '%s' at "
                                        "line %d",
                                        reason, filename, line_idx + 1));
}

// Parses one data row into counts. Returns false after pushing an error.
static bool parse_row(WinPctCounts *counts, const char *line,
                      const char *filename, int line_idx,
                      ErrorStack *error_stack) {
  const char *cursor = line;
  int key[WIN_PCT_COUNTS_ROW_KEY_FIELDS];
  for (int field_idx = 0; field_idx < WIN_PCT_COUNTS_ROW_KEY_FIELDS;
       field_idx++) {
    key[field_idx] = string_to_int_prefix(cursor, &cursor, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      push_row_error(error_stack, filename, line_idx, "invalid row key");
      return false;
    }
  }
  const int sample = key[0];
  const int bag = key[1];
  const int on_turn_rack_tiles = key[2];
  const int off_turn_rack_tiles = key[3];
  const bool bag_empty_state_is_valid =
      bag == 0 && on_turn_rack_tiles >= 1 && on_turn_rack_tiles <= RACK_SIZE &&
      off_turn_rack_tiles >= 1 && off_turn_rack_tiles <= RACK_SIZE;
  const bool bag_state_is_valid = bag >= 1 && bag <= counts->max_bag &&
                                  on_turn_rack_tiles == RACK_SIZE &&
                                  off_turn_rack_tiles == RACK_SIZE;
  if (sample < 0 || sample >= WIN_PCT_NUM_SAMPLES ||
      !(bag_empty_state_is_valid || bag_state_is_valid)) {
    push_row_error(error_stack, filename, line_idx, "invalid state");
    return false;
  }
  double *row = win_pct_counts_get_row(
      counts, sample,
      win_pct_get_cell_index(bag, on_turn_rack_tiles, off_turn_rack_tiles));
  // The file stores the swing sum before the bins; the row stores it after.
  row[counts->num_bins] = string_to_double_prefix(cursor, &cursor, error_stack);
  for (int bin_idx = 0;
       bin_idx < counts->num_bins && error_stack_is_empty(error_stack);
       bin_idx++) {
    row[bin_idx] = string_to_double_prefix(cursor, &cursor, error_stack);
    if (error_stack_is_empty(error_stack) &&
        (row[bin_idx] < 0.0 || !isfinite(row[bin_idx]))) {
      push_row_error(error_stack, filename, line_idx, "invalid count");
      return false;
    }
  }
  if (!error_stack_is_empty(error_stack)) {
    push_row_error(error_stack, filename, line_idx, "missing or invalid count");
    return false;
  }
  if (cursor[strspn(cursor, " \t\r")] != '\0') {
    push_row_error(error_stack, filename, line_idx, "too many columns");
    return false;
  }
  return true;
}

WinPctCounts *win_pct_counts_create_from_string(const char *contents,
                                                const char *filename,
                                                ErrorStack *error_stack) {
  StringSplitter *lines = split_string_by_newline(contents, true);
  const int num_lines = string_splitter_get_number_of_items(lines);
  int header[WIN_PCT_COUNTS_HEADER_FIELDS - 1] = {0};
  if (num_lines > 0 &&
      win_pct_counts_string_has_header(string_splitter_get_item(lines, 0))) {
    const char *cursor = string_splitter_get_item(lines, 0);
    cursor += strspn(cursor, " \t") + strlen(WIN_PCT_COUNTS_MAGIC);
    for (int field_idx = 0; field_idx < WIN_PCT_COUNTS_HEADER_FIELDS - 1 &&
                            error_stack_is_empty(error_stack);
         field_idx++) {
      header[field_idx] = string_to_int_prefix(cursor, &cursor, error_stack);
    }
  } else {
    error_stack_push(
        error_stack, ERROR_STATUS_WIN_PCT_INVALID_HEADER,
        get_formatted_string("missing header in win percentage file '%s'",
                             filename));
  }
  const int version = header[0];
  const int rack_size = header[1];
  const int max_bag = header[2];
  const int max_spread = header[3];
  if (error_stack_is_empty(error_stack) &&
      (version != WIN_PCT_COUNTS_VERSION || rack_size != RACK_SIZE ||
       max_bag < 1 || max_spread < 1)) {
    error_stack_push(
        error_stack, ERROR_STATUS_WIN_PCT_INVALID_HEADER,
        get_formatted_string(
            "win percentage file '%s' has version %d, rack size %d, maximum "
            "bag %d, and maximum spread %d, but version %d with rack size %d "
            "and positive maximums are required",
            filename, version, rack_size, max_bag, max_spread,
            WIN_PCT_COUNTS_VERSION, RACK_SIZE));
  }
  WinPctCounts *counts = NULL;
  if (error_stack_is_empty(error_stack)) {
    counts = win_pct_counts_create(max_bag, max_spread);
    for (int line_idx = 1; line_idx < num_lines; line_idx++) {
      if (!parse_row(counts, string_splitter_get_item(lines, line_idx),
                     filename, line_idx, error_stack)) {
        win_pct_counts_destroy(counts);
        counts = NULL;
        break;
      }
    }
  }
  string_splitter_destroy(lines);
  return counts;
}

static void add_count_to_string_builder(StringBuilder *sb, double value) {
  if (value == floor(value) && fabs(value) < 1e15) {
    string_builder_add_formatted_string(sb, " %.0f", value);
  } else {
    string_builder_add_formatted_string(sb, " %.9g", value);
  }
}

char *win_pct_counts_get_string(const WinPctCounts *counts) {
  StringBuilder *sb = string_builder_create();
  string_builder_add_formatted_string(
      sb, "%s %d %d %d %d\n", WIN_PCT_COUNTS_MAGIC, WIN_PCT_COUNTS_VERSION,
      RACK_SIZE, counts->max_bag, counts->max_spread);
  for (int sample = 0; sample < WIN_PCT_NUM_SAMPLES; sample++) {
    for (int cell = 0; cell < counts->num_cells; cell++) {
      const double *row = win_pct_counts_get_const_row(counts, sample, cell);
      if (win_pct_counts_row_get_total(counts, row) <= 0.0) {
        continue;
      }
      const int bag = win_pct_counts_get_cell_bag(cell);
      const int on_turn_rack_tiles = bag > 0 ? RACK_SIZE : cell / RACK_SIZE + 1;
      const int off_turn_rack_tiles =
          bag > 0 ? RACK_SIZE : cell % RACK_SIZE + 1;
      string_builder_add_formatted_string(sb, "%d %d %d %d", sample, bag,
                                          on_turn_rack_tiles,
                                          off_turn_rack_tiles);
      add_count_to_string_builder(sb, row[counts->num_bins]);
      for (int bin_idx = 0; bin_idx < counts->num_bins; bin_idx++) {
        add_count_to_string_builder(sb, row[bin_idx]);
      }
      string_builder_add_string(sb, "\n");
    }
  }
  char *result = string_builder_dump(sb, NULL);
  string_builder_destroy(sb);
  return result;
}
