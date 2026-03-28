#ifndef EGCAL_DEFS_H
#define EGCAL_DEFS_H

enum {
  // Feature bucket counts
  EGCAL_TOTAL_TILES_MIN = 2,
  EGCAL_TOTAL_TILES_MAX = 14,
  EGCAL_TOTAL_TILES_BUCKETS = 13, // 2..14
  EGCAL_TILES_ON_TURN_BUCKETS = 7, // 1..7
  EGCAL_MAX_TILE_VALUE_BUCKETS = 3, // [1-3], [4-6], [7-10]
  EGCAL_STUCK_FRAC_BUCKETS = 5, // 0, (0,0.25], (0.25,0.5], (0.5,0.75], (0.75,1.0]
  EGCAL_NUM_LEGAL_MOVES_BUCKETS = 6, // [1], [2-5], [6-15], [16-50], [51-200], [201+]

  // Statistics
  EGCAL_NUM_PERCENTILES = 7, // p90, p95, p99, p99.5, p99.9, p99.95, p99.99

  // Minimum observations in a bin before its statistics are trusted
  EGCAL_MIN_BIN_COUNT = 30,

  // Maximum playout depth for standalone greedy playout
  EGCAL_MAX_PLAYOUT_DEPTH = 25,

  // Total number of bins in the flat array
  // 13 * 7 * 10 * 10 * 5 * 5 * 6 = 1,365,000
  EGCAL_TOTAL_BINS = EGCAL_TOTAL_TILES_BUCKETS * EGCAL_TILES_ON_TURN_BUCKETS *
                     EGCAL_MAX_TILE_VALUE_BUCKETS *
                     EGCAL_MAX_TILE_VALUE_BUCKETS * EGCAL_STUCK_FRAC_BUCKETS *
                     EGCAL_STUCK_FRAC_BUCKETS * EGCAL_NUM_LEGAL_MOVES_BUCKETS,

  // Binary format
  EGCAL_MAGIC_SIZE = 6, // "EGCAL\0"
  EGCAL_VERSION = 1,

  // Number of fallback levels (drop features one at a time)
  EGCAL_NUM_FALLBACK_LEVELS = 6,
};

#endif
