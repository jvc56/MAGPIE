#include "win_pct_smoother.h"

#include "../def/rack_defs.h"
#include "../ent/win_pct_counts.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include <float.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

enum {
  WIN_PCT_SMOOTH_MAX_BANDWIDTH = 12,
  WIN_PCT_SMOOTH_NUM_MIN_BAGS = 3,
  WIN_PCT_SMOOTH_NUM_PRIOR_WEIGHTS = 10,
  // Bag-empty states differing by one tile on one rack.
  WIN_PCT_SMOOTH_NUM_NEIGHBORS = 4,
  // Win% errors are scored over spreads within this fraction (in fifths) of
  // the table's maximum; beyond it nearly every state is decided.
  WIN_PCT_SMOOTH_SPREAD_FIFTHS = 3,
};

static const int min_bags[WIN_PCT_SMOOTH_NUM_MIN_BAGS] = {1, 8, 15};
static const double prior_weights[WIN_PCT_SMOOTH_NUM_PRIOR_WEIGHTS] = {
    0, 1, 3, 10, 30, 100, 300, 1000, 3000, 10000};

typedef struct SmoothingParams {
  int bandwidth;
  int min_bag;
  double prior_weight;
} SmoothingParams;

typedef struct CvError {
  double win_pct;
  double mean_swing;
} CvError;

static void copy_row(const WinPctCounts *counts, const double *src,
                     double *dst) {
  memcpy(dst, src, sizeof(double) * win_pct_counts_get_row_length(counts));
}

static void scale_row(const WinPctCounts *counts, double *row, double scale) {
  const int row_length = win_pct_counts_get_row_length(counts);
  for (int value_idx = 0; value_idx < row_length; value_idx++) {
    row[value_idx] *= scale;
  }
}

static void add_scaled_row(const WinPctCounts *counts, const double *src,
                           double scale, double *dst) {
  const int row_length = win_pct_counts_get_row_length(counts);
  for (int value_idx = 0; value_idx < row_length; value_idx++) {
    dst[value_idx] += scale * src[value_idx];
  }
}

static void smooth_bag_row(const WinPctCounts *src, int src_sample, int bag,
                           const SmoothingParams *params, double *dst_row) {
  const int max_bag = win_pct_counts_get_max_bag(src);
  const double *row = win_pct_counts_get_const_row(
      src, src_sample, win_pct_get_cell_index(bag, RACK_SIZE, RACK_SIZE));
  if (params->bandwidth == 0 || bag < params->min_bag) {
    copy_row(src, row, dst_row);
    return;
  }
  memset(dst_row, 0, sizeof(double) * win_pct_counts_get_row_length(src));
  double weight_sum = 0.0;
  for (int offset = -params->bandwidth; offset <= params->bandwidth; offset++) {
    const int neighbor_bag = bag + offset;
    if (neighbor_bag < params->min_bag || neighbor_bag > max_bag) {
      continue;
    }
    const double weight = params->bandwidth + 1 - abs(offset);
    add_scaled_row(
        src,
        win_pct_counts_get_const_row(
            src, src_sample,
            win_pct_get_cell_index(neighbor_bag, RACK_SIZE, RACK_SIZE)),
        weight, dst_row);
    weight_sum += weight;
  }
  // Keep the state's own number of observations so the output reads as
  // counts; a state without data keeps the kernel's average.
  const double total = win_pct_counts_row_get_total(src, row);
  const double pooled_total = win_pct_counts_row_get_total(src, dst_row);
  if (total > 0.0 && pooled_total > 0.0) {
    scale_row(src, dst_row, total / pooled_total);
  } else {
    scale_row(src, dst_row, 1.0 / weight_sum);
  }
}

static void smooth_bag_empty_row(const WinPctCounts *src, int src_sample,
                                 int cell, const SmoothingParams *params,
                                 double *dst_row) {
  copy_row(src, win_pct_counts_get_const_row(src, src_sample, cell), dst_row);
  if (params->prior_weight <= 0.0) {
    return;
  }
  const int on_turn_rack_tiles = cell / RACK_SIZE + 1;
  const int off_turn_rack_tiles = cell % RACK_SIZE + 1;
  const int neighbor_offsets[WIN_PCT_SMOOTH_NUM_NEIGHBORS][2] = {
      {-1, 0}, {1, 0}, {0, -1}, {0, 1}};
  const int row_length = win_pct_counts_get_row_length(src);
  double *prior = calloc_or_die(row_length, sizeof(double));
  for (int neighbor_idx = 0; neighbor_idx < WIN_PCT_SMOOTH_NUM_NEIGHBORS;
       neighbor_idx++) {
    const int neighbor_on =
        on_turn_rack_tiles + neighbor_offsets[neighbor_idx][0];
    const int neighbor_off =
        off_turn_rack_tiles + neighbor_offsets[neighbor_idx][1];
    if (neighbor_on < 1 || neighbor_on > RACK_SIZE || neighbor_off < 1 ||
        neighbor_off > RACK_SIZE) {
      continue;
    }
    add_scaled_row(src,
                   win_pct_counts_get_const_row(
                       src, src_sample,
                       win_pct_get_cell_index(0, neighbor_on, neighbor_off)),
                   1.0, prior);
  }
  const double prior_total = win_pct_counts_row_get_total(src, prior);
  if (prior_total > 0.0) {
    add_scaled_row(src, prior, params->prior_weight / prior_total, dst_row);
  }
  free(prior);
}

// Smooths src's src_sample into dst's dst_sample.
static void smooth_sample(const WinPctCounts *src, int src_sample,
                          const SmoothingParams *params, WinPctCounts *dst,
                          int dst_sample) {
  const int num_cells = win_pct_counts_get_num_cells(src);
  for (int cell = 0; cell < num_cells; cell++) {
    double *dst_row = win_pct_counts_get_row(dst, dst_sample, cell);
    const int bag = win_pct_counts_get_cell_bag(cell);
    if (bag > 0) {
      smooth_bag_row(src, src_sample, bag, params, dst_row);
    } else {
      smooth_bag_empty_row(src, src_sample, cell, params, dst_row);
    }
  }
}

// Adds the error of predicting test_sample from predicted's sample 0 over
// the bag-empty cells or the bag cells, weighted by the test observations.
static void add_cv_error(const WinPctCounts *predicted,
                         const WinPctCounts *test, int test_sample,
                         bool bag_cells, double *predicted_win_pcts,
                         double *test_win_pcts, CvError *error,
                         double *weight) {
  const int max_spread = win_pct_counts_get_max_spread(test);
  const int scored_spread = max_spread * WIN_PCT_SMOOTH_SPREAD_FIFTHS / 5;
  const int num_cells = win_pct_counts_get_num_cells(test);
  for (int cell = 0; cell < num_cells; cell++) {
    if ((win_pct_counts_get_cell_bag(cell) > 0) != bag_cells) {
      continue;
    }
    const double *test_row =
        win_pct_counts_get_const_row(test, test_sample, cell);
    const double *predicted_row =
        win_pct_counts_get_const_row(predicted, 0, cell);
    const double test_total = win_pct_counts_row_get_total(test, test_row);
    if (test_total <= 0.0 ||
        win_pct_counts_row_get_total(predicted, predicted_row) <= 0.0) {
      continue;
    }
    win_pct_counts_row_get_win_pcts(predicted, predicted_row,
                                    predicted_win_pcts);
    win_pct_counts_row_get_win_pcts(test, test_row, test_win_pcts);
    double win_pct_error = 0.0;
    for (int spread = -scored_spread; spread <= scored_spread; spread++) {
      const double diff = predicted_win_pcts[max_spread + spread] -
                          test_win_pcts[max_spread + spread];
      win_pct_error += diff * diff;
    }
    const double mean_diff =
        win_pct_counts_row_get_mean_swing(predicted, predicted_row) -
        win_pct_counts_row_get_mean_swing(test, test_row);
    error->win_pct += test_total * win_pct_error / (2 * scored_spread + 1);
    error->mean_swing += test_total * mean_diff * mean_diff;
    *weight += test_total;
  }
}

// Cross-validates params in both directions over one group of cells.
static CvError cross_validate(const WinPctCounts *counts,
                              const SmoothingParams *params, bool bag_cells,
                              WinPctCounts *scratch, double *predicted_win_pcts,
                              double *test_win_pcts) {
  CvError error = {0};
  double weight = 0.0;
  for (int train_sample = 0; train_sample < WIN_PCT_NUM_SAMPLES;
       train_sample++) {
    smooth_sample(counts, train_sample, params, scratch, 0);
    add_cv_error(scratch, counts, 1 - train_sample, bag_cells,
                 predicted_win_pcts, test_win_pcts, &error, &weight);
  }
  if (weight > 0.0) {
    error.win_pct /= weight;
    error.mean_swing /= weight;
  }
  return error;
}

static double get_sample_total(const WinPctCounts *counts, int sample) {
  double total = 0.0;
  const int num_cells = win_pct_counts_get_num_cells(counts);
  for (int cell = 0; cell < num_cells; cell++) {
    total += win_pct_counts_row_get_total(
        counts, win_pct_counts_get_const_row(counts, sample, cell));
  }
  return total;
}

WinPctCounts *win_pct_smooth(const WinPctCounts *counts, StringBuilder *report,
                             ErrorStack *error_stack) {
  const double sample_totals[WIN_PCT_NUM_SAMPLES] = {
      get_sample_total(counts, 0), get_sample_total(counts, 1)};
  if (sample_totals[0] <= 0.0 || sample_totals[1] <= 0.0) {
    error_stack_push(
        error_stack, ERROR_STATUS_WIN_PCT_NO_DATA_FOUND,
        string_duplicate("smoothing a win percentage table needs data in both "
                         "samples; record it with the winpct autoplay "
                         "recorder"));
    return NULL;
  }
  const int max_bag = win_pct_counts_get_max_bag(counts);
  const int max_spread = win_pct_counts_get_max_spread(counts);
  WinPctCounts *scratch = win_pct_counts_create(max_bag, max_spread);
  double *predicted_win_pcts =
      malloc_or_die(sizeof(double) * (2 * max_spread + 1));
  double *test_win_pcts = malloc_or_die(sizeof(double) * (2 * max_spread + 1));

  string_builder_add_formatted_string(
      report,
      "positions: %.0f and %.0f in the two samples\n"
      "win%% error: mean squared, spreads within +/-%d; mean swing error: "
      "mean squared, points^2\n\n"
      "bag states: bandwidth, smallest bag pooled, win%% error, mean swing "
      "error\n",
      sample_totals[0], sample_totals[1],
      max_spread * WIN_PCT_SMOOTH_SPREAD_FIFTHS / 5);
  SmoothingParams best = {0, 1, 0.0};
  double best_error = DBL_MAX;
  for (int min_bag_idx = 0; min_bag_idx < WIN_PCT_SMOOTH_NUM_MIN_BAGS;
       min_bag_idx++) {
    // Bandwidth 0 is the same for every floor, so score it once.
    const int first_bandwidth = min_bag_idx == 0 ? 0 : 1;
    for (int bandwidth = first_bandwidth;
         bandwidth <= WIN_PCT_SMOOTH_MAX_BANDWIDTH; bandwidth++) {
      const SmoothingParams params = {bandwidth, min_bags[min_bag_idx], 0.0};
      const CvError error = cross_validate(counts, &params, true, scratch,
                                           predicted_win_pcts, test_win_pcts);
      string_builder_add_formatted_string(report, "  %2d  %2d  %.4e  %.4e\n",
                                          bandwidth, params.min_bag,
                                          error.win_pct, error.mean_swing);
      if (error.win_pct < best_error) {
        best_error = error.win_pct;
        best.bandwidth = bandwidth;
        best.min_bag = params.min_bag;
      }
    }
  }
  string_builder_add_formatted_string(
      report,
      "\nbag-empty states: prior weight, win%% error, mean swing error\n");
  best_error = DBL_MAX;
  for (int prior_idx = 0; prior_idx < WIN_PCT_SMOOTH_NUM_PRIOR_WEIGHTS;
       prior_idx++) {
    const SmoothingParams params = {0, 1, prior_weights[prior_idx]};
    const CvError error = cross_validate(counts, &params, false, scratch,
                                         predicted_win_pcts, test_win_pcts);
    string_builder_add_formatted_string(report, "  %6.0f  %.4e  %.4e\n",
                                        params.prior_weight, error.win_pct,
                                        error.mean_swing);
    if (error.win_pct < best_error) {
      best_error = error.win_pct;
      best.prior_weight = params.prior_weight;
    }
  }
  string_builder_add_formatted_string(
      report,
      "\nchosen: bandwidth %d from bag %d, bag-empty prior weight %.0f\n",
      best.bandwidth, best.min_bag, best.prior_weight);

  // Pool the samples into sample 0 of scratch, then smooth that.
  win_pct_counts_reset(scratch);
  const int num_cells = win_pct_counts_get_num_cells(counts);
  for (int cell = 0; cell < num_cells; cell++) {
    double *pooled_row = win_pct_counts_get_row(scratch, 0, cell);
    for (int sample = 0; sample < WIN_PCT_NUM_SAMPLES; sample++) {
      add_scaled_row(counts, win_pct_counts_get_const_row(counts, sample, cell),
                     1.0, pooled_row);
    }
  }
  WinPctCounts *smoothed = win_pct_counts_create(max_bag, max_spread);
  smooth_sample(scratch, 0, &best, smoothed, 0);
  free(test_win_pcts);
  free(predicted_win_pcts);
  win_pct_counts_destroy(scratch);
  return smoothed;
}
