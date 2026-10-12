#ifndef AUTOPLAY_SOLVER_SETTINGS_H
#define AUTOPLAY_SOLVER_SETTINGS_H

#include "../def/peg_defs.h"
#include <assert.h>
#include <stdbool.h>

// How a player in an autoplay game solves the end of the game: the endgame
// once the bag is empty, and the pre-endgame (PEG) while the bag holds
// PEG_MIN_BAG..peg_max_bag tiles. Every other turn is played the player's
// usual way, statically or by simulation.
//
// Work is bounded by depth and candidate counts, never by time, so a player is
// as strong on a slow machine as on a fast one (the same reason birdtest asks
// simulations for an iteration budget rather than a time limit). The solves
// are multithreaded and so not reproducible run to run; that is expected.
//
// endgame_plies == 0 turns off both solvers: PEG scores its emptier scenarios
// with endgame solves, so a player that does not solve endgames does not run
// PEG either, whatever peg_max_bag says.

enum {
  // Most halving stages a PEG schedule may state; the -pegtopk limit.
  AUTOPLAY_SOLVER_MAX_PEG_STAGES = 16,
  // Most per-level candidate caps nested lookahead may state.
  AUTOPLAY_SOLVER_MAX_NESTED_CAND_CAPS = 16,
  // nested_strides is indexed by bag size, 0..PEG_MAX_BAG (0 unused).
  AUTOPLAY_SOLVER_NUM_NESTED_STRIDES = PEG_MAX_BAG + 1,
};

typedef struct AutoplaySolverSettings {
  // Endgame depth in plies; 0 = solve neither endgames nor pre-endgames.
  int endgame_plies;
  // Largest bag PEG runs at; 0 = no PEG. At most PEG_MAX_BAG.
  int peg_max_bag;
  // Per-stage survivor counts for PEG's halving stages (each >= 2).
  int peg_stage_top_k[AUTOPLAY_SOLVER_MAX_PEG_STAGES];
  int peg_num_stages;
  // Scenario sampling stride; 1 = full enumeration. Never 0, which would mean
  // "the solver's default".
  int peg_scenario_stride;
  bool peg_pessimistic;
  // Nested lookahead for non-emptier leaves, and its knobs.
  bool peg_nested;
  int peg_nested_cand_caps[AUTOPLAY_SOLVER_MAX_NESTED_CAND_CAPS];
  int peg_nested_num_cand_caps;
  int peg_nested_max_depth;
  // Stride an inner peg of bag b samples at, for b in 1..PEG_MAX_BAG.
  int peg_nested_strides[AUTOPLAY_SOLVER_NUM_NESTED_STRIDES];
} AutoplaySolverSettings;

// Solving off, and the PEG knobs at the CLI's own defaults, so turning a
// player's solvers on with only -eplies1/-pegbag1 gets the schedule `peg`
// runs with.
static inline void
autoplay_solver_settings_set_defaults(AutoplaySolverSettings *settings) {
  static const int default_stage_top_k[] = {32, 16, 8, 4, 2};
  static const int default_nested_cand_caps[] = {8, 4, 2};
  // peg.c's bag-dependent nested default: 2-peg full, 3-peg 1/5, 4-peg 1/7.
  static const int default_nested_strides[] = {0, 1, 1, 5, 7};
  static_assert(sizeof(default_nested_strides) /
                        sizeof(default_nested_strides[0]) ==
                    AUTOPLAY_SOLVER_NUM_NESTED_STRIDES,
                "one nested stride per bag size");
  *settings = (AutoplaySolverSettings){0};
  settings->endgame_plies = 0;
  settings->peg_max_bag = 0;
  settings->peg_num_stages =
      (int)(sizeof(default_stage_top_k) / sizeof(default_stage_top_k[0]));
  for (int i = 0; i < settings->peg_num_stages; i++) {
    settings->peg_stage_top_k[i] = default_stage_top_k[i];
  }
  settings->peg_scenario_stride = 1;
  settings->peg_pessimistic = false;
  settings->peg_nested = true;
  settings->peg_nested_num_cand_caps =
      (int)(sizeof(default_nested_cand_caps) /
            sizeof(default_nested_cand_caps[0]));
  for (int i = 0; i < settings->peg_nested_num_cand_caps; i++) {
    settings->peg_nested_cand_caps[i] = default_nested_cand_caps[i];
  }
  settings->peg_nested_max_depth = PEG_NESTED_DEFAULT_DEPTH;
  for (int i = 0; i < AUTOPLAY_SOLVER_NUM_NESTED_STRIDES; i++) {
    settings->peg_nested_strides[i] = default_nested_strides[i];
  }
}

// Whether a player with these settings solves anything.
static inline bool
autoplay_solver_settings_solves(const AutoplaySolverSettings *settings) {
  return settings->endgame_plies > 0;
}

// Whether a player with these settings runs PEG at a bag of `bag` tiles.
static inline bool
autoplay_solver_settings_runs_peg(const AutoplaySolverSettings *settings,
                                  int bag) {
  return settings->endgame_plies > 0 && settings->peg_max_bag > 0 &&
         bag >= PEG_MIN_BAG && bag <= settings->peg_max_bag &&
         bag <= PEG_MAX_BAG;
}

#endif
