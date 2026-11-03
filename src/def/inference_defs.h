#ifndef INFERENCE_DEFS_H
#define INFERENCE_DEFS_H

// Minimum leave size for enabling cutoff optimization on exchanges.
// Benchmarks show cutoff optimization hurts performance for exchanges
// with leave size >= this threshold (i.e., small exchanges). The
// optimization never changes which racks are inferred, only how fast.
#define INFERENCE_CUTOFF_MIN_EXCHANGE_LEAVE_SIZE 3

// Use per-rack move generation for one to three unknown tiles and census
// play scores for four or more. On the NWL23 single-worker benchmark census
// is 2.46x faster at four tiles, but 2.24x slower at three. See
// docs/static-temperature-validation-20261103.md for the measured workload.
#define INFERENCE_CENSUS_MIN_TILES_TO_INFER 4

// The positions of the walk over possible leaves that a worker claims at a
// time when the census values them: each takes about a microsecond, so
// claiming one at a time would have the workers contend for the counter.
#define INFERENCE_CENSUS_CLAIM_SIZE 64

// The most multisets a census lattice may hold for an inference to build its
// play scores, one Equity each: English (27 letters, 7 tiles) has 5,379,616.
#define INFERENCE_CENSUS_MAX_LATTICE_SIZE (1U << 24)

typedef enum {
  INFERENCE_TYPE_LEAVE,
  INFERENCE_TYPE_EXCHANGED,
  INFERENCE_TYPE_RACK,
  NUMBER_OF_INFER_TYPES,
} inference_stat_t;

typedef enum {
  INFERENCE_SUBTOTAL_DRAW,
  INFERENCE_SUBTOTAL_LEAVE,
  NUMBER_OF_INFERENCE_SUBTOTALS,
} inference_subtotal_t;

#endif