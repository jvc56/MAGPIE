#ifndef EGCAL_TABLE_H
#define EGCAL_TABLE_H

#include "../def/egcal_defs.h"
#include "../util/io_util.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct EgcalTable EgcalTable;

EgcalTable *egcal_table_create(void);
void egcal_table_destroy(EgcalTable *table);

// Record a single observation. Spread values are in millipoints.
// max_tile_value, stuck_frac, and num_legal_moves are all bucket indices.
void egcal_table_add(EgcalTable *table, int total_tiles, int tiles_on_turn,
                     int max_tile_value_on_turn_bucket,
                     int max_tile_value_off_turn_bucket,
                     int stuck_frac_on_turn_bucket,
                     int stuck_frac_off_turn_bucket,
                     int num_legal_moves_bucket, int32_t greedy_spread,
                     int32_t exact_spread);

// Merge source into dst (for combining per-thread tables).
void egcal_table_merge(EgcalTable *dst, const EgcalTable *src);

// Finalize: sort raw errors, compute percentiles, resolve fallback hierarchy.
// Must be called after all adds/merges and before write or lookup.
void egcal_table_finalize(EgcalTable *table);

// Write finalized table to a binary .egcal file.
void egcal_table_write(const EgcalTable *table, const char *filename,
                       ErrorStack *error_stack);

// Load a binary .egcal file. The returned table is already finalized.
EgcalTable *egcal_table_load(const char *filename, ErrorStack *error_stack);

// Lookup percentile margins for a given feature combination.
// Falls back through the hierarchy if the exact bin has too few samples.
// Writes EGCAL_NUM_PERCENTILES values to upper/lower percentiles
// (millipoints). Upper = how much better exact can be than greedy.
// Lower = how much worse exact can be than greedy (negative values).
// Either output pointer may be NULL if not needed.
// Returns the count of the bin used (0 if no data at all).
// All feature parameters except total_tiles and tiles_on_turn are bucket
// indices.
uint32_t egcal_table_lookup(const EgcalTable *table, int total_tiles,
                            int tiles_on_turn,
                            int max_tile_value_on_turn_bucket,
                            int max_tile_value_off_turn_bucket,
                            int stuck_frac_on_turn_bucket,
                            int stuck_frac_off_turn_bucket,
                            int num_legal_moves_bucket, int32_t *mean_error_out,
                            int32_t *upper_percentiles_out,
                            int32_t *lower_percentiles_out);

// Get summary statistics for printing.
int egcal_table_get_total_observations(const EgcalTable *table);
int egcal_table_get_populated_bin_count(const EgcalTable *table);

// Get per-tile-count aggregate statistics (for summary output).
// tile_count is in [EGCAL_TOTAL_TILES_MIN, EGCAL_TOTAL_TILES_MAX].
// Returns observation count for that tile count. Writes mean and upper
// percentiles aggregated across all bins with that tile count.
uint32_t egcal_table_get_tile_count_stats(const EgcalTable *table,
                                          int tile_count,
                                          int32_t *mean_error_out,
                                          int32_t *upper_percentiles_out);

#endif
