#include "egcal_table.h"

#include "../compat/endian_conv.h"
#include "../def/egcal_defs.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static const char EGCAL_MAGIC[EGCAL_MAGIC_SIZE] = "EGCAL";

// Percentile levels (fractions in [0,1])
static const double EGCAL_PERCENTILE_LEVELS[EGCAL_NUM_PERCENTILES] = {
    0.90, 0.95, 0.99, 0.995, 0.999, 0.9995, 0.9999,
};

// --- Raw bin: used during accumulation ---

typedef struct EgcalRawBin {
  uint32_t count;
  int32_t *errors;       // dynamically grown array of (exact - greedy)
  uint32_t errors_capacity;
} EgcalRawBin;

// --- Finalized bin: used after finalize ---

typedef struct EgcalFinalizedBin {
  uint32_t count;
  int32_t mean_error; // millipoints
  int32_t upper_percentiles[EGCAL_NUM_PERCENTILES]; // upper tail (exact > greedy)
  int32_t lower_percentiles[EGCAL_NUM_PERCENTILES]; // lower tail (exact < greedy)
} EgcalFinalizedBin;

struct EgcalTable {
  EgcalRawBin *raw_bins;             // non-NULL during accumulation
  EgcalFinalizedBin *finalized_bins; // non-NULL after finalization
  bool is_finalized;
};

// --- Index computation ---

// All max_tile_value, stuck_frac, and legal_moves parameters are bucket indices.
static int compute_bin_index(int total_tiles, int tiles_on_turn,
                             int max_tile_value_on_turn_bucket,
                             int max_tile_value_off_turn_bucket,
                             int stuck_frac_on_turn_bucket,
                             int stuck_frac_off_turn_bucket,
                             int num_legal_moves_bucket) {
  int tt_idx = total_tiles - EGCAL_TOTAL_TILES_MIN;
  int tot_idx = tiles_on_turn - 1;

  int index = tt_idx;
  index = index * EGCAL_TILES_ON_TURN_BUCKETS + tot_idx;
  index = index * EGCAL_MAX_TILE_VALUE_BUCKETS + max_tile_value_on_turn_bucket;
  index = index * EGCAL_MAX_TILE_VALUE_BUCKETS + max_tile_value_off_turn_bucket;
  index = index * EGCAL_STUCK_FRAC_BUCKETS + stuck_frac_on_turn_bucket;
  index = index * EGCAL_STUCK_FRAC_BUCKETS + stuck_frac_off_turn_bucket;
  index = index * EGCAL_NUM_LEGAL_MOVES_BUCKETS + num_legal_moves_bucket;

  return index;
}

// Pack feature values into a uint32_t for serialization.
// Layout (bit allocation):
//   total_tiles_idx:       4 bits (0-12)
//   tiles_on_turn_idx:     3 bits (0-6)
//   max_val_otk_bucket:    2 bits (0-2)
//   max_val_ott_bucket:    2 bits (0-2)
//   stuck_otk_bucket:      3 bits (0-4)
//   stuck_ott_bucket:      3 bits (0-4)
//   legal_moves_bucket:    3 bits (0-5)
//   Total: 20 bits
static uint32_t pack_feature_key(int total_tiles, int tiles_on_turn,
                                 int max_tile_value_on_turn_bucket,
                                 int max_tile_value_off_turn_bucket,
                                 int stuck_frac_on_turn_bucket,
                                 int stuck_frac_off_turn_bucket,
                                 int num_legal_moves_bucket) {
  uint32_t key = 0;
  key |= (uint32_t)(total_tiles - EGCAL_TOTAL_TILES_MIN);
  key |= (uint32_t)(tiles_on_turn - 1) << 4;
  key |= (uint32_t)max_tile_value_on_turn_bucket << 7;
  key |= (uint32_t)max_tile_value_off_turn_bucket << 9;
  key |= (uint32_t)stuck_frac_on_turn_bucket << 11;
  key |= (uint32_t)stuck_frac_off_turn_bucket << 14;
  key |= (uint32_t)num_legal_moves_bucket << 17;
  return key;
}

static void unpack_feature_key(uint32_t key, int *total_tiles,
                               int *tiles_on_turn,
                               int *max_tile_value_on_turn_bucket,
                               int *max_tile_value_off_turn_bucket,
                               int *stuck_frac_on_turn_bucket,
                               int *stuck_frac_off_turn_bucket,
                               int *num_legal_moves_bucket) {
  *total_tiles = (int)(key & 0xF) + EGCAL_TOTAL_TILES_MIN;
  *tiles_on_turn = (int)((key >> 4) & 0x7) + 1;
  *max_tile_value_on_turn_bucket = (int)((key >> 7) & 0x3);
  *max_tile_value_off_turn_bucket = (int)((key >> 9) & 0x3);
  *stuck_frac_on_turn_bucket = (int)((key >> 11) & 0x7);
  *stuck_frac_off_turn_bucket = (int)((key >> 14) & 0x7);
  *num_legal_moves_bucket = (int)((key >> 17) & 0x7);
}

// --- Comparison function for qsort ---

static int compare_int32(const void *a, const void *b) {
  int32_t va = *(const int32_t *)a;
  int32_t vb = *(const int32_t *)b;
  if (va < vb) {
    return -1;
  }
  if (va > vb) {
    return 1;
  }
  return 0;
}

// --- Create / Destroy ---

EgcalTable *egcal_table_create(void) {
  EgcalTable *table = malloc_or_die(sizeof(EgcalTable));
  table->raw_bins = calloc(EGCAL_TOTAL_BINS, sizeof(EgcalRawBin));
  if (!table->raw_bins) {
    log_fatal("failed to allocate egcal raw bins");
  }
  table->finalized_bins = NULL;
  table->is_finalized = false;
  return table;
}

void egcal_table_destroy(EgcalTable *table) {
  if (!table) {
    return;
  }
  if (table->raw_bins) {
    for (int bin_idx = 0; bin_idx < EGCAL_TOTAL_BINS; bin_idx++) {
      free(table->raw_bins[bin_idx].errors);
    }
    free(table->raw_bins);
  }
  free(table->finalized_bins);
  free(table);
}

// --- Add observation ---

void egcal_table_add(EgcalTable *table, int total_tiles, int tiles_on_turn,
                     int max_tile_value_on_turn_bucket,
                     int max_tile_value_off_turn_bucket,
                     int stuck_frac_on_turn_bucket,
                     int stuck_frac_off_turn_bucket,
                     int num_legal_moves_bucket, int32_t greedy_spread,
                     int32_t exact_spread) {
  int index = compute_bin_index(total_tiles, tiles_on_turn,
                                max_tile_value_on_turn_bucket,
                                max_tile_value_off_turn_bucket,
                                stuck_frac_on_turn_bucket,
                                stuck_frac_off_turn_bucket,
                                num_legal_moves_bucket);
  EgcalRawBin *bin = &table->raw_bins[index];
  int32_t error = exact_spread - greedy_spread;

  if (bin->count >= bin->errors_capacity) {
    uint32_t new_cap = bin->errors_capacity == 0 ? 16 : bin->errors_capacity * 2;
    bin->errors = realloc(bin->errors, new_cap * sizeof(int32_t));
    if (!bin->errors) {
      log_fatal("failed to grow egcal bin errors array");
    }
    bin->errors_capacity = new_cap;
  }
  bin->errors[bin->count] = error;
  bin->count++;
}

// --- Merge ---

void egcal_table_merge(EgcalTable *dst, const EgcalTable *src) {
  for (int bin_idx = 0; bin_idx < EGCAL_TOTAL_BINS; bin_idx++) {
    const EgcalRawBin *src_bin = &src->raw_bins[bin_idx];
    if (src_bin->count == 0) {
      continue;
    }
    EgcalRawBin *dst_bin = &dst->raw_bins[bin_idx];
    uint32_t new_count = dst_bin->count + src_bin->count;
    if (new_count > dst_bin->errors_capacity) {
      uint32_t new_cap = dst_bin->errors_capacity;
      if (new_cap == 0) {
        new_cap = 16;
      }
      while (new_cap < new_count) {
        new_cap *= 2;
      }
      dst_bin->errors = realloc(dst_bin->errors, new_cap * sizeof(int32_t));
      if (!dst_bin->errors) {
        log_fatal("failed to grow egcal bin errors array during merge");
      }
      dst_bin->errors_capacity = new_cap;
    }
    memcpy(dst_bin->errors + dst_bin->count, src_bin->errors,
           src_bin->count * sizeof(int32_t));
    dst_bin->count = new_count;
  }
}

// --- Finalize helpers ---

static int32_t compute_percentile(const int32_t *sorted_errors, uint32_t count,
                                  double percentile_fraction) {
  if (count == 0) {
    return 0;
  }
  double rank = percentile_fraction * (double)(count - 1);
  int lower = (int)floor(rank);
  int upper = lower + 1;
  if (upper >= (int)count) {
    return sorted_errors[count - 1];
  }
  double frac = rank - (double)lower;
  return (int32_t)((1.0 - frac) * (double)sorted_errors[lower] +
                   frac * (double)sorted_errors[upper]);
}

// Lower percentile levels mirror the upper ones (e.g., p10, p5, p1, ...)
static const double EGCAL_LOWER_PERCENTILE_LEVELS[EGCAL_NUM_PERCENTILES] = {
    0.10, 0.05, 0.01, 0.005, 0.001, 0.0005, 0.0001,
};

static void finalize_bin_from_errors(EgcalFinalizedBin *fbin,
                                     const int32_t *sorted_errors,
                                     uint32_t count) {
  fbin->count = count;
  if (count == 0) {
    fbin->mean_error = 0;
    memset(fbin->upper_percentiles, 0, sizeof(fbin->upper_percentiles));
    memset(fbin->lower_percentiles, 0, sizeof(fbin->lower_percentiles));
    return;
  }
  int64_t sum = 0;
  for (uint32_t obs_idx = 0; obs_idx < count; obs_idx++) {
    sum += sorted_errors[obs_idx];
  }
  fbin->mean_error = (int32_t)(sum / (int64_t)count);
  for (int pct_idx = 0; pct_idx < EGCAL_NUM_PERCENTILES; pct_idx++) {
    fbin->upper_percentiles[pct_idx] = compute_percentile(
        sorted_errors, count, EGCAL_PERCENTILE_LEVELS[pct_idx]);
    fbin->lower_percentiles[pct_idx] = compute_percentile(
        sorted_errors, count, EGCAL_LOWER_PERCENTILE_LEVELS[pct_idx]);
  }
}

// --- Finalize with fallback hierarchy ---

// Aggregate all raw errors for bins matching a partial feature key.
// Set a dimension to -1 to wildcard it.
static void aggregate_errors_for_fallback(const EgcalTable *table,
                                          int total_tiles, int tiles_on_turn,
                                          int max_val_otk, int max_val_ott,
                                          int stuck_otk, int stuck_ott,
                                          int legal_moves, int32_t **errors_out,
                                          uint32_t *count_out,
                                          uint32_t *capacity_out) {
  int tt_start = total_tiles >= 0 ? total_tiles - EGCAL_TOTAL_TILES_MIN
                                  : 0;
  int tt_end = total_tiles >= 0 ? tt_start + 1 : EGCAL_TOTAL_TILES_BUCKETS;

  int tot_start = tiles_on_turn >= 0 ? tiles_on_turn - 1 : 0;
  int tot_end =
      tiles_on_turn >= 0 ? tot_start + 1 : EGCAL_TILES_ON_TURN_BUCKETS;

  int mvotk_start = max_val_otk >= 0 ? max_val_otk : 0;
  int mvotk_end =
      max_val_otk >= 0 ? mvotk_start + 1 : EGCAL_MAX_TILE_VALUE_BUCKETS;

  int mvott_start = max_val_ott >= 0 ? max_val_ott : 0;
  int mvott_end =
      max_val_ott >= 0 ? mvott_start + 1 : EGCAL_MAX_TILE_VALUE_BUCKETS;

  int sotk_start = stuck_otk >= 0 ? stuck_otk : 0;
  int sotk_end = stuck_otk >= 0 ? sotk_start + 1 : EGCAL_STUCK_FRAC_BUCKETS;

  int sott_start = stuck_ott >= 0 ? stuck_ott : 0;
  int sott_end = stuck_ott >= 0 ? sott_start + 1 : EGCAL_STUCK_FRAC_BUCKETS;

  int lm_start = legal_moves >= 0 ? legal_moves : 0;
  int lm_end =
      legal_moves >= 0 ? lm_start + 1 : EGCAL_NUM_LEGAL_MOVES_BUCKETS;

  uint32_t total_count = 0;
  uint32_t cap = 0;
  int32_t *all_errors = NULL;

  for (int tt = tt_start; tt < tt_end; tt++) {
    for (int tot = tot_start; tot < tot_end; tot++) {
      for (int mvotk = mvotk_start; mvotk < mvotk_end; mvotk++) {
        for (int mvott = mvott_start; mvott < mvott_end; mvott++) {
          for (int sotk = sotk_start; sotk < sotk_end; sotk++) {
            for (int sott = sott_start; sott < sott_end; sott++) {
              for (int lm = lm_start; lm < lm_end; lm++) {
                int index = compute_bin_index(
                    tt + EGCAL_TOTAL_TILES_MIN, tot + 1, mvotk, mvott, sotk,
                    sott, lm);
                const EgcalRawBin *bin = &table->raw_bins[index];
                if (bin->count == 0) {
                  continue;
                }
                uint32_t new_count = total_count + bin->count;
                if (new_count > cap) {
                  if (cap == 0) {
                    cap = 256;
                  }
                  while (cap < new_count) {
                    cap *= 2;
                  }
                  all_errors = realloc(all_errors, cap * sizeof(int32_t));
                  if (!all_errors) {
                    log_fatal("failed to allocate fallback errors");
                  }
                }
                // cppcheck-suppress nullPointerArithmeticRedundantCheck
                memcpy(all_errors + total_count, bin->errors,
                       bin->count * sizeof(int32_t));
                total_count = new_count;
              }
            }
          }
        }
      }
    }
  }

  *errors_out = all_errors;
  *count_out = total_count;
  *capacity_out = cap;
}

void egcal_table_finalize(EgcalTable *table) {
  if (table->is_finalized) {
    return;
  }

  table->finalized_bins =
      calloc(EGCAL_TOTAL_BINS, sizeof(EgcalFinalizedBin));
  if (!table->finalized_bins) {
    log_fatal("failed to allocate egcal finalized bins");
  }

  // Sort each populated raw bin's errors and compute direct statistics.
  for (int bin_idx = 0; bin_idx < EGCAL_TOTAL_BINS; bin_idx++) {
    EgcalRawBin *rbin = &table->raw_bins[bin_idx];
    if (rbin->count == 0) {
      continue;
    }
    if (rbin->count > 1) {
      qsort(rbin->errors, rbin->count, sizeof(int32_t), compare_int32);
    }
    finalize_bin_from_errors(&table->finalized_bins[bin_idx], rbin->errors,
                             rbin->count);
  }

  table->is_finalized = true;
}

// Fallback lookup: try progressively coarser feature combinations until
// a bin with enough samples is found. This is done at lookup time to avoid
// processing all 1.365M bins at finalization time.
static uint32_t lookup_with_fallback(const EgcalTable *table, int total_tiles,
                                     int tiles_on_turn, int max_val_otk,
                                     int max_val_ott, int stuck_otk,
                                     int stuck_ott, int legal_moves,
                                     int32_t *mean_error_out,
                                     int32_t *upper_percentiles_out,
                                     int32_t *lower_percentiles_out) {
  // First try the exact bin
  int index = compute_bin_index(total_tiles, tiles_on_turn, max_val_otk,
                                max_val_ott, stuck_otk, stuck_ott,
                                legal_moves);
  const EgcalFinalizedBin *fbin = &table->finalized_bins[index];
  if (fbin->count >= EGCAL_MIN_BIN_COUNT) {
    if (mean_error_out) {
      *mean_error_out = fbin->mean_error;
    }
    if (upper_percentiles_out) {
      memcpy(upper_percentiles_out, fbin->upper_percentiles,
             sizeof(fbin->upper_percentiles));
    }
    if (lower_percentiles_out) {
      memcpy(lower_percentiles_out, fbin->lower_percentiles,
             sizeof(fbin->lower_percentiles));
    }
    return fbin->count;
  }

  // Fallback: aggregate raw errors from progressively coarser bins.
  // Only possible if raw_bins are available (not for loaded tables).
  if (!table->raw_bins) {
    if (mean_error_out) {
      *mean_error_out = fbin->mean_error;
    }
    if (upper_percentiles_out) {
      memcpy(upper_percentiles_out, fbin->upper_percentiles,
             sizeof(fbin->upper_percentiles));
    }
    if (lower_percentiles_out) {
      memcpy(lower_percentiles_out, fbin->lower_percentiles,
             sizeof(fbin->lower_percentiles));
    }
    return fbin->count;
  }

  // Fallback levels: drop features from least to most important
  int fb_legal_moves = legal_moves;
  int fb_max_val_ott = max_val_ott;
  int fb_stuck_ott = stuck_ott;
  int fb_stuck_otk = stuck_otk;
  int fb_max_val_otk = max_val_otk;

  for (int level = 0; level < EGCAL_NUM_FALLBACK_LEVELS; level++) {
    switch (level) {
    case 0:
      fb_legal_moves = -1;
      break;
    case 1:
      fb_max_val_ott = -1;
      break;
    case 2:
      fb_stuck_ott = -1;
      break;
    case 3:
      fb_stuck_otk = -1;
      break;
    case 4:
      fb_max_val_otk = -1;
      break;
    case 5:
      break;
    }

    int32_t *fallback_errors = NULL;
    uint32_t fallback_count = 0;
    uint32_t fallback_cap = 0;
    aggregate_errors_for_fallback(table, total_tiles, tiles_on_turn,
                                  fb_max_val_otk, fb_max_val_ott, fb_stuck_otk,
                                  fb_stuck_ott, fb_legal_moves,
                                  &fallback_errors, &fallback_count,
                                  &fallback_cap);

    if (fallback_count >= EGCAL_MIN_BIN_COUNT) {
      qsort(fallback_errors, fallback_count, sizeof(int32_t), compare_int32);
      EgcalFinalizedBin temp;
      finalize_bin_from_errors(&temp, fallback_errors, fallback_count);
      free(fallback_errors);
      if (mean_error_out) {
        *mean_error_out = temp.mean_error;
      }
      if (upper_percentiles_out) {
        memcpy(upper_percentiles_out, temp.upper_percentiles,
               sizeof(temp.upper_percentiles));
      }
      if (lower_percentiles_out) {
        memcpy(lower_percentiles_out, temp.lower_percentiles,
               sizeof(temp.lower_percentiles));
      }
      return temp.count;
    }
    free(fallback_errors);
  }

  // No fallback found with enough data
  if (mean_error_out) {
    *mean_error_out = 0;
  }
  if (upper_percentiles_out) {
    memset(upper_percentiles_out, 0, EGCAL_NUM_PERCENTILES * sizeof(int32_t));
  }
  if (lower_percentiles_out) {
    memset(lower_percentiles_out, 0, EGCAL_NUM_PERCENTILES * sizeof(int32_t));
  }
  return 0;
}

// --- Lookup ---

uint32_t egcal_table_lookup(const EgcalTable *table, int total_tiles,
                            int tiles_on_turn,
                            int max_tile_value_on_turn_bucket,
                            int max_tile_value_off_turn_bucket,
                            int stuck_frac_on_turn_bucket,
                            int stuck_frac_off_turn_bucket,
                            int num_legal_moves_bucket, int32_t *mean_error_out,
                            int32_t *upper_percentiles_out,
                            int32_t *lower_percentiles_out) {
  if (!table->is_finalized) {
    log_fatal("egcal_table_lookup called on unfinalized table");
  }
  return lookup_with_fallback(table, total_tiles, tiles_on_turn,
                              max_tile_value_on_turn_bucket,
                              max_tile_value_off_turn_bucket,
                              stuck_frac_on_turn_bucket,
                              stuck_frac_off_turn_bucket,
                              num_legal_moves_bucket, mean_error_out,
                              upper_percentiles_out, lower_percentiles_out);
}

// --- Summary statistics ---

int egcal_table_get_total_observations(const EgcalTable *table) {
  int total = 0;
  if (table->raw_bins) {
    for (int bin_idx = 0; bin_idx < EGCAL_TOTAL_BINS; bin_idx++) {
      total += (int)table->raw_bins[bin_idx].count;
    }
  } else if (table->finalized_bins) {
    for (int bin_idx = 0; bin_idx < EGCAL_TOTAL_BINS; bin_idx++) {
      total += (int)table->finalized_bins[bin_idx].count;
    }
  }
  return total;
}

int egcal_table_get_populated_bin_count(const EgcalTable *table) {
  int count = 0;
  if (table->raw_bins) {
    for (int bin_idx = 0; bin_idx < EGCAL_TOTAL_BINS; bin_idx++) {
      if (table->raw_bins[bin_idx].count > 0) {
        count++;
      }
    }
  } else if (table->finalized_bins) {
    for (int bin_idx = 0; bin_idx < EGCAL_TOTAL_BINS; bin_idx++) {
      if (table->finalized_bins[bin_idx].count > 0) {
        count++;
      }
    }
  }
  return count;
}

uint32_t egcal_table_get_tile_count_stats(const EgcalTable *table,
                                          int tile_count,
                                          int32_t *mean_error_out,
                                          int32_t *percentiles_out) {
  if (!table->raw_bins) {
    if (mean_error_out) {
      *mean_error_out = 0;
    }
    if (percentiles_out) {
      memset(percentiles_out, 0, EGCAL_NUM_PERCENTILES * sizeof(int32_t));
    }
    return 0;
  }

  // Aggregate all raw errors for this tile count
  int32_t *all_errors = NULL;
  uint32_t total_count = 0;
  uint32_t cap = 0;
  aggregate_errors_for_fallback(table, tile_count, -1, -1, -1, -1, -1, -1,
                                &all_errors, &total_count, &cap);

  if (total_count == 0) {
    if (mean_error_out) {
      *mean_error_out = 0;
    }
    if (percentiles_out) {
      memset(percentiles_out, 0, EGCAL_NUM_PERCENTILES * sizeof(int32_t));
    }
    free(all_errors);
    return 0;
  }

  qsort(all_errors, total_count, sizeof(int32_t), compare_int32);

  EgcalFinalizedBin temp;
  finalize_bin_from_errors(&temp, all_errors, total_count);
  if (mean_error_out) {
    *mean_error_out = temp.mean_error;
  }
  if (percentiles_out) {
    memcpy(percentiles_out, temp.upper_percentiles, sizeof(temp.upper_percentiles));
  }
  free(all_errors);
  return total_count;
}

// --- Binary I/O ---

void egcal_table_write(const EgcalTable *table, const char *filename,
                       ErrorStack *error_stack) {
  if (!table->is_finalized) {
    log_fatal("egcal_table_write called on unfinalized table");
  }

  FILE *file = fopen_safe(filename, "wb", error_stack);
  if (!file) {
    return;
  }

  // Count non-empty finalized bins (using raw bin counts, since finalized
  // bins may have fallback data even for originally-empty bins).
  uint32_t num_populated = 0;
  for (int bin_idx = 0; bin_idx < EGCAL_TOTAL_BINS; bin_idx++) {
    if (table->raw_bins && table->raw_bins[bin_idx].count > 0) {
      num_populated++;
    }
  }

  // Write header
  fwrite_or_die(EGCAL_MAGIC, 1, EGCAL_MAGIC_SIZE, file, "egcal magic");
  uint16_t version = htole16(EGCAL_VERSION);
  fwrite_or_die(&version, sizeof(version), 1, file, "egcal version");
  uint8_t num_pct = EGCAL_NUM_PERCENTILES;
  fwrite_or_die(&num_pct, 1, 1, file, "egcal num_percentiles");
  uint8_t pad8 = 0;
  fwrite_or_die(&pad8, 1, 1, file, "egcal pad");
  uint32_t num_bins_le = htole32(num_populated);
  fwrite_or_die(&num_bins_le, sizeof(num_bins_le), 1, file, "egcal num_bins");
  uint16_t pad16 = 0;
  fwrite_or_die(&pad16, sizeof(pad16), 1, file, "egcal pad");

  // Write non-empty bins
  for (int bin_idx = 0; bin_idx < EGCAL_TOTAL_BINS; bin_idx++) {
    if (!table->raw_bins || table->raw_bins[bin_idx].count == 0) {
      continue;
    }

    // Decode features from bin index
    int remaining = bin_idx;
    int legal_moves = remaining % EGCAL_NUM_LEGAL_MOVES_BUCKETS;
    remaining /= EGCAL_NUM_LEGAL_MOVES_BUCKETS;
    int stuck_ott = remaining % EGCAL_STUCK_FRAC_BUCKETS;
    remaining /= EGCAL_STUCK_FRAC_BUCKETS;
    int stuck_otk = remaining % EGCAL_STUCK_FRAC_BUCKETS;
    remaining /= EGCAL_STUCK_FRAC_BUCKETS;
    int max_val_ott = remaining % EGCAL_MAX_TILE_VALUE_BUCKETS;
    remaining /= EGCAL_MAX_TILE_VALUE_BUCKETS;
    int max_val_otk = remaining % EGCAL_MAX_TILE_VALUE_BUCKETS;
    remaining /= EGCAL_MAX_TILE_VALUE_BUCKETS;
    int tiles_on_turn = remaining % EGCAL_TILES_ON_TURN_BUCKETS;
    remaining /= EGCAL_TILES_ON_TURN_BUCKETS;
    int total_tiles = remaining;

    uint32_t feature_key = pack_feature_key(
        total_tiles + EGCAL_TOTAL_TILES_MIN, tiles_on_turn + 1, max_val_otk,
        max_val_ott, stuck_otk, stuck_ott, legal_moves);
    uint32_t feature_key_le = htole32(feature_key);
    fwrite_or_die(&feature_key_le, sizeof(feature_key_le), 1, file,
                  "egcal feature_key");

    const EgcalFinalizedBin *fbin = &table->finalized_bins[bin_idx];
    uint32_t count_le = htole32(fbin->count);
    fwrite_or_die(&count_le, sizeof(count_le), 1, file, "egcal count");

    int32_t mean_le = (int32_t)htole32((uint32_t)fbin->mean_error);
    fwrite_or_die(&mean_le, sizeof(mean_le), 1, file, "egcal mean_error");

    for (int pct_idx = 0; pct_idx < EGCAL_NUM_PERCENTILES; pct_idx++) {
      int32_t pct_le =
          (int32_t)htole32((uint32_t)fbin->upper_percentiles[pct_idx]);
      fwrite_or_die(&pct_le, sizeof(pct_le), 1, file, "egcal upper percentile");
    }
    for (int pct_idx = 0; pct_idx < EGCAL_NUM_PERCENTILES; pct_idx++) {
      int32_t pct_le =
          (int32_t)htole32((uint32_t)fbin->lower_percentiles[pct_idx]);
      fwrite_or_die(&pct_le, sizeof(pct_le), 1, file, "egcal lower percentile");
    }
  }

  fclose_or_die(file);
}

EgcalTable *egcal_table_load(const char *filename, ErrorStack *error_stack) {
  FILE *file = fopen_safe(filename, "rb", error_stack);
  if (!file) {
    return NULL;
  }

  // Read and validate header
  char magic[EGCAL_MAGIC_SIZE];
  if (fread(magic, 1, EGCAL_MAGIC_SIZE, file) != EGCAL_MAGIC_SIZE ||
      memcmp(magic, EGCAL_MAGIC, EGCAL_MAGIC_SIZE) != 0) {
    error_stack_push(error_stack, ERROR_STATUS_RW_READ_ERROR,
                     get_formatted_string("invalid egcal magic in %s",
                                          filename));
    fclose_or_die(file);
    return NULL;
  }

  uint16_t version;
  if (fread(&version, sizeof(version), 1, file) != 1) {
    error_stack_push(error_stack, ERROR_STATUS_RW_READ_ERROR,
                     get_formatted_string("failed to read egcal version from %s",
                                          filename));
    fclose_or_die(file);
    return NULL;
  }
  version = le16toh(version);
  if (version != EGCAL_VERSION) {
    error_stack_push(
        error_stack, ERROR_STATUS_RW_READ_ERROR,
        get_formatted_string("unsupported egcal version %d in %s",
                             (int)version, filename));
    fclose_or_die(file);
    return NULL;
  }

  uint8_t num_pct;
  if (fread(&num_pct, 1, 1, file) != 1) {
    error_stack_push(error_stack, ERROR_STATUS_RW_READ_ERROR,
                     get_formatted_string(
                         "failed to read egcal num_percentiles from %s",
                         filename));
    fclose_or_die(file);
    return NULL;
  }

  uint8_t pad8;
  if (fread(&pad8, 1, 1, file) != 1) {
    error_stack_push(error_stack, ERROR_STATUS_RW_READ_ERROR,
                     get_formatted_string("failed to read egcal padding from %s",
                                          filename));
    fclose_or_die(file);
    return NULL;
  }

  uint32_t num_bins;
  if (fread(&num_bins, sizeof(num_bins), 1, file) != 1) {
    error_stack_push(error_stack, ERROR_STATUS_RW_READ_ERROR,
                     get_formatted_string("failed to read egcal num_bins from %s",
                                          filename));
    fclose_or_die(file);
    return NULL;
  }
  num_bins = le32toh(num_bins);

  uint16_t pad16;
  if (fread(&pad16, sizeof(pad16), 1, file) != 1) {
    error_stack_push(error_stack, ERROR_STATUS_RW_READ_ERROR,
                     get_formatted_string("failed to read egcal padding from %s",
                                          filename));
    fclose_or_die(file);
    return NULL;
  }

  // Create table with finalized bins only (no raw bins needed for loaded data)
  EgcalTable *table = malloc_or_die(sizeof(EgcalTable));
  table->raw_bins = NULL;
  table->finalized_bins =
      calloc(EGCAL_TOTAL_BINS, sizeof(EgcalFinalizedBin));
  if (!table->finalized_bins) {
    log_fatal("failed to allocate egcal finalized bins for load");
  }
  table->is_finalized = true;

  int pct_to_read = num_pct < EGCAL_NUM_PERCENTILES ? num_pct
                                                     : EGCAL_NUM_PERCENTILES;

  for (uint32_t entry_idx = 0; entry_idx < num_bins; entry_idx++) {
    uint32_t feature_key;
    if (fread(&feature_key, sizeof(feature_key), 1, file) != 1) {
      error_stack_push(error_stack, ERROR_STATUS_RW_READ_ERROR,
                       get_formatted_string(
                           "failed to read egcal feature_key from %s",
                           filename));
      egcal_table_destroy(table);
      fclose_or_die(file);
      return NULL;
    }
    feature_key = le32toh(feature_key);

    int total_tiles, tiles_on_turn, max_val_otk, max_val_ott;
    int stuck_otk, stuck_ott, legal_moves;
    unpack_feature_key(feature_key, &total_tiles, &tiles_on_turn, &max_val_otk,
                       &max_val_ott, &stuck_otk, &stuck_ott, &legal_moves);

    int index = compute_bin_index(total_tiles, tiles_on_turn, max_val_otk,
                                  max_val_ott, stuck_otk, stuck_ott,
                                  legal_moves);
    EgcalFinalizedBin *fbin = &table->finalized_bins[index];

    uint32_t count;
    if (fread(&count, sizeof(count), 1, file) != 1) {
      error_stack_push(error_stack, ERROR_STATUS_RW_READ_ERROR,
                       get_formatted_string(
                           "failed to read egcal count from %s", filename));
      egcal_table_destroy(table);
      fclose_or_die(file);
      return NULL;
    }
    fbin->count = le32toh(count);

    int32_t mean_error;
    if (fread(&mean_error, sizeof(mean_error), 1, file) != 1) {
      error_stack_push(
          error_stack, ERROR_STATUS_RW_READ_ERROR,
          get_formatted_string("failed to read egcal mean_error from %s",
                               filename));
      egcal_table_destroy(table);
      fclose_or_die(file);
      return NULL;
    }
    fbin->mean_error = (int32_t)le32toh((uint32_t)mean_error);

    for (int pct_idx = 0; pct_idx < (int)num_pct; pct_idx++) {
      int32_t pct_val;
      if (fread(&pct_val, sizeof(pct_val), 1, file) != 1) {
        error_stack_push(
            error_stack, ERROR_STATUS_RW_READ_ERROR,
            get_formatted_string(
                "failed to read egcal upper percentile from %s", filename));
        egcal_table_destroy(table);
        fclose_or_die(file);
        return NULL;
      }
      if (pct_idx < pct_to_read) {
        fbin->upper_percentiles[pct_idx] = (int32_t)le32toh((uint32_t)pct_val);
      }
    }
    for (int pct_idx = 0; pct_idx < (int)num_pct; pct_idx++) {
      int32_t pct_val;
      if (fread(&pct_val, sizeof(pct_val), 1, file) != 1) {
        error_stack_push(
            error_stack, ERROR_STATUS_RW_READ_ERROR,
            get_formatted_string(
                "failed to read egcal lower percentile from %s", filename));
        egcal_table_destroy(table);
        fclose_or_die(file);
        return NULL;
      }
      if (pct_idx < pct_to_read) {
        fbin->lower_percentiles[pct_idx] = (int32_t)le32toh((uint32_t)pct_val);
      }
    }
  }

  fclose_or_die(file);
  return table;
}
