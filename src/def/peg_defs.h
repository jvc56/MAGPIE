#ifndef PEG_DEFS_H
#define PEG_DEFS_H

#include "rack_defs.h"

// Bag-size range a pre-endgame (PEG) position may have. PEG handles 1..4 tiles
// in the bag; larger positions are midgame and are rejected by the solver.
enum {
  PEG_MIN_BAG = 1,
  PEG_MAX_BAG = 4,
  PEG_MAX_UNSEEN = RACK_SIZE + PEG_MAX_BAG,
};

// Default inner-peg recursion depth when nested lookahead is on: one nested
// peg, then a greedy rollout. Deeper recursion does not improve the decision.
enum {
  PEG_NESTED_DEFAULT_DEPTH = 1,
};

// Solver internals: fixed capacities and depth ceilings for peg.c.
enum {
  // Candidate move-list capacity for the root generate_moves.
  PEG_CAND_LIST_CAP = 16384,
  // Greedy playout depth ceiling (a PEG playout terminates well before this).
  PEG_PLAYOUT_MAX_PLIES = 40,
  // Endgame depth for the exhaustive single stage (-pegtopk all / 0). An
  // emptier PEG leaf holds at most 2*RACK_SIZE tiles, so the endgame tree
  // bottoms out at terminal nodes well short of this; the overshoot is free and
  // just guarantees the solve runs to game end with no frontier truncation.
  PEG_EXHAUSTIVE_PLIES = 40,
  // Fixed endgame seed for deterministic leaf solves.
  PEG_ENDGAME_SEED = 1,
  // Pessimistic playout: capacity of the opponent reply list. The adversarial
  // 1-ply lookahead tries every generated reply (a late-game PEG position has
  // far fewer than this), so the opponent's true worst-for-mover reply -- which
  // includes the rational best-equity one -- is always considered.
  PEG_PESSIMISTIC_OPP_LIST_CAP = 1024,
  // Max recursion depth of the inline nested-PEG lookahead (one frame per
  // non-emptier ply). Bounds the per-level scratch stack and caps how far an
  // exhaustive nested solve can recurse through passes/exchanges before the
  // lookahead budget is spent. Comfortably above any realistic fidelity.
  PEG_MAX_NEST_DEPTH = 24,
  // Candidate list capacity for a nested-PEG level's move generation.
  PEG_NEST_CAND_LIST_CAP = 4096,
  // Max candidate field an inner-peg cascade carries (the first stage cap is
  // clamped to this); the staged narrowing keeps the working set tiny.
  PEG_NEST_FIELD_MAX = 64,
  // Slot capacity of the per-solve leaf-prune cache. Distinct post-candidate
  // boards reaching an exact leaf are bounded by the candidate field (a few
  // hundred), so this is never approached in practice.
  PEG_PRUNE_CACHE_CAPACITY = 1 << 14,
};

// Live-view (PegPoll) fixed capacities sizing the pollable snapshot's arrays.
enum {
  PEG_POLL_MAX_STAGES = 20,
  PEG_POLL_MAX_ENTRIES = 64,
  // Max non-greedy stages we keep per-candidate timing history for. 8 covers
  // the default cascade (32->16->8->4->2, five stages) with room to spare.
  PEG_POLL_MAX_HISTORY_STAGES = 8,
};

// Smallest outcomes-cell content width the renderer ever wraps to: enough for
// the label plus one worst-case token (a 4-tile sequence with a 4-digit
// weight) -- strlen("W: A/B/C/Dx1234").
enum {
  PEG_OUTCOMES_MIN_CELL = 15,
};

// PEG work-stealing pool capacities (peg_pool.c).
enum {
  PEG_POOL_QUEUE_INIT_CAP = 1024,
  // Per-worker stack. Workers recurse into nested solves while help-draining
  // the queue (bounded by the PEG fork-nesting cap), so the small default
  // secondary-thread stack is not enough. 64 MiB is virtual address space,
  // committed lazily, so only the nesting depth actually reached is paid for.
  PEG_POOL_WORKER_STACK_BYTES = 64 * 1024 * 1024,
};

#endif
