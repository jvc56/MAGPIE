#ifndef INFERENCE_DEFS_H
#define INFERENCE_DEFS_H

// Minimum leave size for enabling cutoff optimization on exchanges.
// Benchmarks show cutoff optimization hurts performance for exchanges
// with leave size >= this threshold (i.e., small exchanges). The
// optimization never changes which racks are inferred, only how fast.
#define INFERENCE_CUTOFF_MIN_EXCHANGE_LEAVE_SIZE 3

// The fewest unknown tiles on the target's rack for which an inference values
// its racks with the board's census play scores: with fewer there are too few
// racks to pay for building them. Measured on CSW24 self-play positions, the
// census is the faster from 5 tiles on 1 to 18 threads, and from 4 on up to
// 10 threads, but not on 18.
#define INFERENCE_CENSUS_MIN_TILES_TO_INFER 5

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