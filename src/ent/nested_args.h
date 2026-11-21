#ifndef NESTED_ARGS_H
#define NESTED_ARGS_H

#include <stdbool.h>
#include <stdint.h>

// Nested rollouts (Phase 2 of docs/lamc-plan.md): at the rollout plies it
// names, the player on turn chooses its play by a small search of its own
// instead of taking the top play by static equity. The search takes the
// player's best plays by static equity and gives each a few short nested
// rollouts, in which the other player's rack is drawn again from what the
// player on turn cannot see. It plays the best by the nested rollouts' mean
// utility.

enum {
  // The most plays a nested search considers, samples it takes and static
  // plies a nested rollout plays after its play.
  NESTED_MAX_CANDIDATES = 64,
  NESTED_MAX_SAMPLES = 4096,
  NESTED_MAX_PLIES = 16,
  // The most plays a diverse pick chooses from.
  NESTED_MAX_UNIVERSE = 5000,
  NESTED_DEFAULT_STATIC_SEEDS = 3,
  // The LAMC search's bounds and defaults (NestedArgs.lamc_search).
  NESTED_MAX_POOL_SIZE = 64,
  NESTED_MAX_ROUNDS = 32,
  NESTED_MAX_EVALUATED = 256,
  NESTED_MAX_COLUMNS = 64,
  NESTED_DEFAULT_POOL_SIZE = 8,
  NESTED_DEFAULT_KEEP = 3,
  NESTED_DEFAULT_ROUNDS = 3,
  // Plies at or past this are never nested.
  NESTED_MAX_PLY_INDEX = 31,
  NESTED_DEFAULT_CANDIDATES = 6,
  NESTED_DEFAULT_SAMPLES = 8,
  NESTED_DEFAULT_PLIES = 1,
  // Points of static equity below the best a candidate may be.
  NESTED_DEFAULT_MARGIN = 30,
};

typedef enum {
  // Every candidate gets the same number of nested rollouts.
  NESTED_ALLOC_UNIFORM,
  // Rollouts go to the candidates still in contention: after each round of
  // one rollout for every candidate, candidates more than a few standard
  // errors of their difference behind the best are dropped.
  NESTED_ALLOC_RACE,
} nested_alloc_t;

typedef struct NestedArgs {
  bool enabled;
  // Bit p: rollout ply p (0 is the reply to the candidate) chooses by a
  // nested search.
  uint32_t plies_mask;
  // The most plays a nested search considers, best by static equity first.
  int num_candidates;
  // Nested rollouts per candidate.
  int num_samples;
  // Static plies a nested rollout plays after its candidate.
  int nested_plies;
  // Candidates more than this many points of static equity below the best
  // are not considered.
  int margin;
  // A nested search runs only when the bag holds at most this many tiles;
  // 0 for no limit.
  int max_bag;
  // A nested search runs only when the bag holds at least this many tiles.
  int min_bag;
  nested_alloc_t alloc;
  // With NESTED_ALLOC_RACE, how many standard errors of the paired
  // difference behind the leader drop a candidate.
  double race_z;
  // A candidate other than the best by static equity must beat it by at
  // least this much nested utility to be chosen.
  double switch_margin;
  // Whether nested rollouts are shared between searches (see
  // rollout_nested.h): a candidate's value is its exact score and leave,
  // and the other player's best responses to the board it leaves, which are
  // cached by that board.
  bool share;
  // With finalists > 0, a sim with more plays than that first simulates them
  // all without nested rollouts, with stage_share of its iterations and
  // time, and then simulates its best finalists plays with nested rollouts
  // and the rest. The nested sim's results alone choose the play.
  int finalists;
  double stage_share;
  // Whether a search's decision is cached, by the board, the rack of the
  // player on turn, the spread and the scoreless turns, and the search's
  // nested rollouts are seeded from that key, so that a decision is the same
  // whichever rollout makes it. Late in the game, where few racks are
  // possible, decisions repeat often.
  bool decision_cache;
  // With universe > 0, the candidates are not the top num_candidates by
  // static equity but a diverse pick from the top universe: the best
  // static_seeds, then plays spread over the Phase 1 play embedding by
  // farthest-point sampling. margin does not apply.
  int universe;
  int static_seeds;
  // Whether a nested search is a LAMC search (as Phase 1's, at the root):
  // over every play, at most universe of them if universe is set; a first
  // pool of pool_size plays (static_seeds best by static equity, the rest
  // spread over the play embedding), num_samples nested rollouts each a
  // round; a Gaussian process with a prior mean linear in static equity;
  // then rounds that keep its best keep plays and add the unevaluated plays
  // of highest upper confidence bound (ucb_beta standard deviations). The
  // play is the best evaluated by the surrogate's mean.
  bool lamc_search;
  int pool_size;
  int keep;
  int rounds;
  double ucb_beta;
} NestedArgs;

static inline void nested_args_set_defaults(NestedArgs *args) {
  args->enabled = false;
  args->plies_mask = 1;
  args->num_candidates = NESTED_DEFAULT_CANDIDATES;
  args->num_samples = NESTED_DEFAULT_SAMPLES;
  args->nested_plies = NESTED_DEFAULT_PLIES;
  args->margin = NESTED_DEFAULT_MARGIN;
  args->max_bag = 0;
  args->min_bag = 0;
  args->alloc = NESTED_ALLOC_UNIFORM;
  args->race_z = 2.0;
  args->switch_margin = 0.0;
  args->share = false;
  args->finalists = 0;
  args->stage_share = 0.5;
  args->decision_cache = false;
  args->universe = 0;
  args->static_seeds = NESTED_DEFAULT_STATIC_SEEDS;
  args->lamc_search = false;
  args->pool_size = NESTED_DEFAULT_POOL_SIZE;
  args->keep = NESTED_DEFAULT_KEEP;
  args->rounds = NESTED_DEFAULT_ROUNDS;
  args->ucb_beta = 1.0;
}

#endif
