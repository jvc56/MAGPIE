#include "../src/def/egcal_defs.h"
#include "../src/ent/egcal_table.h"
#include "../src/util/io_util.h"
#include "test_util.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

void test_egcal_table_round_trip(void) {
  EgcalTable *table = egcal_table_create();

  // Add observations at various tile counts and features
  // Bucket indices: max_tile_value 1=mid(4-6), stuck_frac 0=zero, legal_moves
  // 2=[6-15]
  for (int obs_idx = 0; obs_idx < 100; obs_idx++) {
    // Simulate a position with 6 total tiles, 3 on turn
    int32_t greedy = 10000 + obs_idx * 100; // 10.0 to 19.9 points
    int32_t exact = 12000 + obs_idx * 100;  // 12.0 to 21.9 points (error ~2.0)
    egcal_table_add(table, 6, 3, 1, 1, 0, 0, 2, greedy, exact);
  }

  for (int obs_idx = 0; obs_idx < 50; obs_idx++) {
    // Simulate a position with 10 total tiles, 5 on turn
    int32_t greedy = -5000 + obs_idx * 200;
    int32_t exact = -3000 + obs_idx * 200; // error ~2.0
    egcal_table_add(table, 10, 5, 2, 0, 1, 0, 3, greedy, exact);
  }

  assert(egcal_table_get_total_observations(table) == 150);

  // Finalize
  egcal_table_finalize(table);

  // Lookup the 6-tile bin (should have 100 observations >= EGCAL_MIN_BIN_COUNT)
  int32_t mean_error;
  int32_t upper_pcts[EGCAL_NUM_PERCENTILES];
  int32_t lower_pcts[EGCAL_NUM_PERCENTILES];
  uint32_t count = egcal_table_lookup(table, 6, 3, 1, 1, 0, 0, 2,
                                      &mean_error, upper_pcts, lower_pcts);
  assert(count >= EGCAL_MIN_BIN_COUNT);
  // Error is always 2000 millipoints (exact - greedy = 2000)
  assert(mean_error == 2000);
  // All upper percentiles should be 2000 since error is constant
  for (int pct_idx = 0; pct_idx < EGCAL_NUM_PERCENTILES; pct_idx++) {
    assert(upper_pcts[pct_idx] == 2000);
    assert(lower_pcts[pct_idx] == 2000);
  }

  // Write to a temp file
  const char *test_filename = "./testdata/lexica/test_egcal_roundtrip.egcal";
  ErrorStack *error_stack = error_stack_create();
  egcal_table_write(table, test_filename, error_stack);
  assert(error_stack_is_empty(error_stack));

  // Load it back
  EgcalTable *loaded = egcal_table_load(test_filename, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(loaded != NULL);

  // Verify loaded data matches
  int32_t loaded_mean;
  int32_t loaded_upper[EGCAL_NUM_PERCENTILES];
  int32_t loaded_lower[EGCAL_NUM_PERCENTILES];
  uint32_t loaded_count = egcal_table_lookup(loaded, 6, 3, 1, 1, 0, 0, 2,
                                             &loaded_mean, loaded_upper,
                                             loaded_lower);
  assert(loaded_count == count);
  assert(loaded_mean == mean_error);
  for (int pct_idx = 0; pct_idx < EGCAL_NUM_PERCENTILES; pct_idx++) {
    assert(loaded_upper[pct_idx] == upper_pcts[pct_idx]);
    assert(loaded_lower[pct_idx] == lower_pcts[pct_idx]);
  }

  // Cleanup temp file
  remove(test_filename);

  egcal_table_destroy(loaded);
  egcal_table_destroy(table);
  error_stack_destroy(error_stack);
}

void test_egcal_table_fallback(void) {
  EgcalTable *table = egcal_table_create();

  // Add only 5 observations to a specific bin (below EGCAL_MIN_BIN_COUNT)
  for (int obs_idx = 0; obs_idx < 5; obs_idx++) {
    egcal_table_add(table, 4, 2, 0, 0, 0, 0, 1, 1000, 3000);
  }

  // Add 50 observations to the same total_tiles/tiles_on_turn but different
  // features, so the fallback can aggregate them
  for (int obs_idx = 0; obs_idx < 50; obs_idx++) {
    egcal_table_add(table, 4, 2, 1, 1, 0, 0, 2, 1000, 4000);
  }

  egcal_table_finalize(table);

  // The specific bin (4, 2, 0, 0, 0, 0, 1) has only 5 observations,
  // but fallback should aggregate with the 50 other observations at
  // (4, 2, ...) with progressively coarser features.
  int32_t mean_error;
  int32_t upper_pcts[EGCAL_NUM_PERCENTILES];
  uint32_t count = egcal_table_lookup(table, 4, 2, 0, 0, 0, 0, 1,
                                      &mean_error, upper_pcts, NULL);
  // Should have found enough data through fallback
  assert(count >= EGCAL_MIN_BIN_COUNT);

  egcal_table_destroy(table);
}

void test_egcal_table_tile_count_stats(void) {
  EgcalTable *table = egcal_table_create();

  // Add observations at tile count 8 with varying features
  for (int obs_idx = 0; obs_idx < 40; obs_idx++) {
    egcal_table_add(table, 8, 4, 1, 1, 0, 0, 2, 0,
                    obs_idx * 100); // error = obs_idx * 100
  }

  int32_t mean_error;
  int32_t upper_pcts[EGCAL_NUM_PERCENTILES];
  uint32_t count =
      egcal_table_get_tile_count_stats(table, 8, &mean_error, upper_pcts);
  assert(count == 40);
  // Mean error = mean(0, 100, 200, ..., 3900) = 1950
  assert(mean_error == 1950);
  // p90 should be around 3500-3600 millipoints
  assert(upper_pcts[0] > 3000);
  assert(upper_pcts[0] < 4000);

  egcal_table_destroy(table);
}

void test_egcal(void) {
  test_egcal_table_round_trip();
  test_egcal_table_fallback();
  test_egcal_table_tile_count_stats();
}
