#ifndef INFERENCE_DEFS_H
#define INFERENCE_DEFS_H

// Minimum leave size for enabling cutoff optimization on exchanges.
// Benchmarks show cutoff optimization hurts performance for exchanges
// with leave size >= this threshold (i.e., small exchanges).
#define INFERENCE_CUTOFF_MIN_EXCHANGE_LEAVE_SIZE 3

// Win-probability mode defaults (mirrors Macondo's rangefinder constants).
#define INFERENCE_WINPCT_DEFAULT_TAU 0.05
#define INFERENCE_WINPCT_LOGIT_EPS 1e-6
#define INFERENCE_WINPCT_DEFAULT_SIM_PLIES 2
// Tile-placement inference: 10 matches Macondo rangefinder inference.go:600.
// Exchange inference uses 15 (inference.go:650) and overrides this at call
// site.
#define INFERENCE_WINPCT_DEFAULT_SIM_MAX_PLAYS 10
// Hard cap for the candidate_move_list and fixed-size arrays in
// compute_softmax_likelihood.  The +1 allows appending the target move when it
// is not already in the top-N-by-equity list (Macondo always sims the target).
// Exchange inference uses 15 (inference.go:650); tile placement uses 10.
#define INFERENCE_WINPCT_MAX_CANDIDATE_PLAYS 16
// Minimum samples per arm in the BAI initial (uniform) phase before adaptive
// TopTwo+GK16 sampling begins. Mirrors Macondo's minIterationsForPruning=128
// in stopping_condition.go: arms below this threshold are never pruned.
// Capped at mini_sim_iters to prevent initial_limit > sample_limit.
#define INFERENCE_WINPCT_BAI_INITIAL_ITERS 128
#define INFERENCE_WINPCT_DEFAULT_MC_MAX_ITERS 200
// Wall-clock time limit for the MC outer loop (seconds). Mirrors Macondo's
// context deadline default of 1 minute.
#define INFERENCE_WINPCT_DEFAULT_TIME_LIMIT_SECS 60
#define INFERENCE_WINPCT_DEFAULT_MAX_ENUM_HYPOTHESES 750
#define INFERENCE_WINPCT_DEFAULT_MIN_WEIGHT_EPS 0.01

typedef enum {
  INFERENCE_MODE_EQUITY,
  INFERENCE_MODE_WINPCT,
} inference_mode_t;

typedef enum {
  INFERENCE_SAMPLE_MODE_AUTO,
  INFERENCE_SAMPLE_MODE_ENUM,
  INFERENCE_SAMPLE_MODE_MC,
} inference_sample_mode_t;

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