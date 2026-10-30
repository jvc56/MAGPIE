#ifndef PEG_DEFS_H
#define PEG_DEFS_H

#include "game_defs.h"
#include "rack_defs.h"

// Bag-size range a pre-endgame (PEG) position may have. PEG handles 1..4 tiles
// in the bag; larger positions are midgame and are rejected by the solver.
// Exception: a bag above PEG_MAX_BAG (up to RACK_SIZE) is still accepted when
// every candidate move under consideration is guaranteed to empty it -- see
// the bag-emptying-guarantee check in peg_solve.
enum {
  PEG_MIN_BAG = 1,
  PEG_MAX_BAG = 4,
  PEG_MAX_UNSEEN = RACK_SIZE + PEG_MAX_BAG,
};

// Capacity of the mover-drawn / bag-remaining scratch arrays in peg.c's outer
// (non-nested) scenario enumeration. Normally bounded by PEG_MAX_BAG, but the
// bag-emptying-guarantee exception above lets bag_size run as high as
// RACK_SIZE, and k_drawn = min(tiles_played, bag_size) <= tiles_played is
// always <= RACK_SIZE regardless, so these arrays are sized off RACK_SIZE
// unconditionally rather than off the (possibly relaxed) bag_size.
enum {
  PEG_SCENARIO_ARRAY_CAP = RACK_SIZE,
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
  // Leading greedy playout plies that come before any draw whose outcome
  // depends on the bag's order (the opponent's reply, then the mover's), so
  // every ordering of one split plays them identically (see PegSharedPly).
  PEG_SHARED_PLAYOUT_PLIES = 2,
  // Opponent racks a candidate's reply cache holds (a power of two). An
  // opponent holds RACK_SIZE of at most RACK_SIZE + PEG_MAX_BAG unseen tiles,
  // so at most C(11, 4) = 330 racks at the default rack size.
  PEG_REPLY_CACHE_SLOTS = 1024,
  // Endgame depth for the exhaustive single stage (-pegtopk all / 0): the
  // deepest the endgame searches, whose per-ply arrays hold MAX_SEARCH_DEPTH
  // entries. An emptier PEG leaf holds at most 2*RACK_SIZE tiles, so the
  // endgame tree bottoms out at terminal nodes short of this in practice, and
  // the solve runs to game end with no frontier truncation.
  PEG_EXHAUSTIVE_PLIES = MAX_SEARCH_DEPTH,
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
  // Max mover draws of the candidate in progress a snapshot lists: every
  // running one (one per thread), then finished ones.
  PEG_POLL_MAX_DRAW_ROWS = 64,
  // Max candidates solved at once, one per thread, a snapshot lists.
  PEG_POLL_MAX_CAND_ROWS = 64,
  // Max solved candidates of the current endgame stage a snapshot lists, with
  // their ranks, beside the top PEG_POLL_MAX_ENTRIES.
  PEG_POLL_MAX_SOLVED_ROWS = 32,
};

// The ranking rows status, shpeg and peg show by default, and the most
// solved plays ranked below them that follow.
enum {
  PEG_DISPLAY_ROWS = 10,
  PEG_DISPLAY_MAX_PINNED_ROWS = 10,
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
};

#endif
