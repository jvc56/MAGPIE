#ifndef INFERENCE_DEFS_H
#define INFERENCE_DEFS_H

// Minimum leave size for enabling cutoff optimization on exchanges.
// Benchmarks show cutoff optimization hurts performance for exchanges
// with leave size >= this threshold (i.e., small exchanges).
#define INFERENCE_CUTOFF_MIN_EXCHANGE_LEAVE_SIZE 3

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