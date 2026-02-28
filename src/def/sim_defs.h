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
  ply_strategy_t ply_strategy;
  // Fields below only used when ply_strategy == PLY_STRATEGY_NESTED_SIM
  int nested_candidates;   // K: number of candidate moves to evaluate per ply
  int nested_rollouts;     // N: mini-rollouts per candidate
  int nested_plies;        // depth of each mini-rollout
} FidelityLevel;

enum {
  MAX_FIDELITY_LEVELS = 4,
};

#endif