#ifndef SIM_DEFS_H
#define SIM_DEFS_H

#include <stdint.h>

enum {
  MAX_PLIES = 25,
};

// Ply strategy controls how each ply decision is made during simulation
// rollouts. STATIC uses get_top_equity_move (fast, single movegen call).
// NESTED_SIM generates K candidates, runs N mini-rollouts per candidate,
// and picks the one with the best average spread.
typedef enum {
  PLY_STRATEGY_STATIC,
  PLY_STRATEGY_NESTED_SIM,
} ply_strategy_t;

// Configuration for a single fidelity level in multi-fidelity BAI.
// Each level specifies how many BAI samples to draw and how each
// sample's rollout plies choose their moves.
typedef struct FidelityLevel {
  uint64_t sample_limit;
  uint64_t sample_minimum;
  double time_limit_seconds; // per-level time budget (0 = no limit)
  ply_strategy_t ply_strategy;
  // Fields below only used when ply_strategy == PLY_STRATEGY_NESTED_SIM
  int nested_candidates;   // K: number of candidate moves to evaluate per ply
  int nested_rollouts;     // N: per-candidate floor (initial round-robin)
  int nested_plies;        // depth of each mini-rollout
  int nested_max_samples;  // total sample cap across all candidates (0 = K*N)
  double nested_stop_z;    // early-stop z-score on top1 vs top2 Welch t-test
                           // (e.g. 2.326 for ~99% one-sided; 0 disables)
  // Inner utility weights used by the nested ply chooser. Same semantics as
  // SimArgs.utility_w_{winpct,spread} / utility_spread_scale (see
  // sim_utility_blend in sim_args.h). Defaults (1.0, 0.0, 100.0) pick by
  // pure win%, matching the legacy lexicographic behavior.
  double inner_w_winpct;
  double inner_w_spread;
  double inner_spread_scale;
} FidelityLevel;

enum {
  MAX_FIDELITY_LEVELS = 4,
};

#endif