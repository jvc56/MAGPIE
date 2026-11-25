#ifndef RLAMC_ARGS_H
#define RLAMC_ARGS_H

#include <stdbool.h>
#include <stdint.h>

// Recursive rollouts (Phase 2 of docs/lamc-plan.md): at the rollout plies it
// names, the player on turn chooses its play by a small search of its own
// instead of taking the top play by static equity. The search takes the
// player's best plays by static equity and gives each a few short recursive
// rollouts, in which the other player's rack is drawn again from what the
// player on turn cannot see. It plays the best by the recursive rollouts' mean
// utility.

enum {
  // The most plays a recursive search considers, samples it takes and static
  // plies a recursive rollout plays after its play.
  RLAMC_MAX_CANDIDATES = 64,
  RLAMC_MAX_SAMPLES = 4096,
  RLAMC_MAX_PLIES = 16,
  // The most plays a diverse pick chooses from.
  RLAMC_MAX_UNIVERSE = 5000,
  RLAMC_DEFAULT_STATIC_SEEDS = 3,
  // The LAMC search's bounds and defaults (RlamcArgs.lamc_search).
  RLAMC_MAX_POOL_SIZE = 64,
  RLAMC_MAX_CONTROL_SAMPLES = 64,
  RLAMC_MAX_ROUNDS = 32,
  RLAMC_MAX_EVALUATED = 256,
  RLAMC_MAX_COLUMNS = 64,
  RLAMC_DEFAULT_POOL_SIZE = 8,
  RLAMC_DEFAULT_KEEP = 3,
  RLAMC_DEFAULT_ROUNDS = 3,
  // A dynamic LAMC search's rollouts per play between refits.
  RLAMC_DEFAULT_STEP = 2,
  // Plies at or past this are never recursive.
  RLAMC_MAX_PLY_INDEX = 31,
  // The most levels of recursive search: a level's recursive rollouts may
  // choose plays by a search of the next level.
  RLAMC_MAX_DEPTH = 4,
  // The defaults are the best settings found (RLAMC.md): a LAMC search at
  // rollout plies 0 and 1 (bits 0 and 1), 4 rollouts a play, decisions
  // cached, no static finish.
  RLAMC_DEFAULT_PLIES_MASK = 3,
  RLAMC_DEFAULT_CANDIDATES = 6,
  RLAMC_DEFAULT_SAMPLES = 4,
  RLAMC_DEFAULT_PLIES = 1,
  // Points of static equity below the best a candidate may be.
  RLAMC_DEFAULT_MARGIN = 30,
};

typedef enum {
  // Every candidate gets the same number of recursive rollouts.
  RLAMC_ALLOC_UNIFORM,
  // Rollouts go to the candidates still in contention: after each round of
  // one rollout for every candidate, candidates more than a few standard
  // errors of their difference behind the best are dropped.
  RLAMC_ALLOC_RACE,
} rlamc_alloc_t;

// The settings of one level of recursive search.
typedef struct RlamcLevel {
  // Bit p: rollout ply p (0 is the reply to the candidate) chooses by a
  // search of this level. For the first level these are the sim's rollout
  // plies; for a deeper level, the plies of the recursive rollouts of the level
  // above it.
  uint32_t plies_mask;
  // The most plays a recursive search considers, best by static equity first.
  int num_candidates;
  // Recursive rollouts per candidate.
  int num_samples;
  // Static plies a recursive rollout plays after its candidate.
  int horizon_plies;
  // Candidates more than this many points of static equity below the best
  // are not considered.
  int margin;
  // A recursive search runs only when the bag holds at most this many tiles;
  // 0 for no limit.
  int max_bag;
  // A recursive search runs only when the bag holds at least this many tiles.
  int min_bag;
  rlamc_alloc_t alloc;
  // With RLAMC_ALLOC_RACE, how many standard errors of the paired
  // difference behind the leader drop a candidate.
  double race_z;
  // A candidate other than the best by static equity must beat it by at
  // least this much recursive utility to be chosen.
  double switch_margin;
  // Whether recursive rollouts are shared between searches (see
  // rlamc.h): a candidate's value is its exact score and leave,
  // and the other player's best responses to the board it leaves, which are
  // cached by that board.
  bool share;
  // Whether a search's decision is cached, by the board, the rack of the
  // player on turn, the spread and the scoreless turns, and the search's
  // recursive rollouts are seeded from that key, so that a decision is the same
  // whichever rollout makes it. Late in the game, where few racks are
  // possible, decisions repeat often.
  bool decision_cache;
  // With universe > 0, the candidates are not the top num_candidates by
  // static equity but a diverse pick from the top universe: the best
  // static_seeds, then plays spread over the Phase 1 play embedding by
  // farthest-point sampling. margin does not apply.
  int universe;
  int static_seeds;
  // Whether a recursive search is a LAMC search (as Phase 1's, at the root):
  // over every play, at most universe of them if universe is set; a first
  // pool of pool_size plays (static_seeds best by static equity, the rest
  // spread over the play embedding), num_samples recursive rollouts each a
  // round; a Gaussian process with a prior mean linear in static equity;
  // then rounds that keep its best keep plays and add the unevaluated plays
  // of highest upper confidence bound (ucb_beta standard deviations). The
  // play is the best evaluated by the surrogate's mean.
  bool lamc_search;
  int pool_size;
  int keep;
  int rounds;
  double ucb_beta;
  // With stop_z > 0, a LAMC search stops before its rounds are done once
  // its best play by the surrogate's mean, less stop_z standard deviations,
  // leads every other play's mean plus stop_z standard deviations.
  double stop_z;
  // Whether a LAMC search models the luck its common seeds share: a play's
  // n-th rollout of every play draws the same racks, so each value is taken
  // as the play's mean plus the seed's effect plus noise, and the surrogate
  // fits the plays' means with the noise that is left.
  bool paired;
  // Whether a LAMC search's rollouts cycle the bag, as the sim's bag_cycle
  // does: a play's n-th rollout draws the other player's rack from shuffle
  // n / R rotated by n % R racks, where R racks cover every unseen letter,
  // so each R rollouts start every unseen letter on that rack once.
  bool bag_cycle;
  // Whether this depth's rollouts skip the static finish that -sfinish plays
  // from an empty bag, and score the position after their plies as rollouts
  // without -sfinish do.
  bool skip_finish;
  // Whether a LAMC search's pool changes dynamically: instead of rounds of
  // num_samples rollouts for every play in the pool, its budget (rounds x
  // pool_size x num_samples rollouts) goes out step rollouts at a time to
  // each play in the pool, and after each step the pool becomes the best
  // play by the surrogate's mean and the pool_size - 1 other plays,
  // evaluated or not, of highest upper confidence bound. A play the
  // surrogate shows beaten leaves the pool as soon as it is, and the best
  // unevaluated play takes its place.
  bool dynamic;
  int step;
} RlamcLevel;

typedef struct RlamcArgs {
  bool enabled;
  // The levels of recursive search, at least 1 when enabled. The sim's rollouts
  // choose by levels[0] at its plies; a recursive rollout of level d chooses by
  // levels[d + 1] at its plies, if there is such a level, and by static
  // equity otherwise.
  int depth;
  RlamcLevel levels[RLAMC_MAX_DEPTH];
  // With finalists > 0, a sim with more plays than that first simulates them
  // all without recursive rollouts, with stage_share of its iterations and
  // time, and then simulates its best finalists plays with recursive rollouts
  // and the rest. The recursive sim's results alone choose the play.
  int finalists;
  double stage_share;
  // With control_samples = k > 0, each of the sim's samples is taken with a
  // control variate: the recursive sample, less a static sample from the same
  // seed, plus the mean of k static samples from seeds of their own.
  int control_samples;
} RlamcArgs;

static inline void rlamc_level_set_defaults(RlamcLevel *args) {
  args->plies_mask = RLAMC_DEFAULT_PLIES_MASK;
  args->num_candidates = RLAMC_DEFAULT_CANDIDATES;
  args->num_samples = RLAMC_DEFAULT_SAMPLES;
  args->horizon_plies = RLAMC_DEFAULT_PLIES;
  args->margin = RLAMC_DEFAULT_MARGIN;
  args->max_bag = 0;
  args->min_bag = 0;
  args->alloc = RLAMC_ALLOC_UNIFORM;
  args->race_z = 2.0;
  args->switch_margin = 0.0;
  args->share = false;
  args->decision_cache = true;
  args->universe = 0;
  args->static_seeds = RLAMC_DEFAULT_STATIC_SEEDS;
  args->lamc_search = true;
  args->pool_size = RLAMC_DEFAULT_POOL_SIZE;
  args->keep = RLAMC_DEFAULT_KEEP;
  args->rounds = RLAMC_DEFAULT_ROUNDS;
  args->ucb_beta = 1.0;
  args->stop_z = 0.0;
  args->paired = false;
  args->bag_cycle = false;
  args->skip_finish = true;
  args->dynamic = false;
  args->step = RLAMC_DEFAULT_STEP;
}

static inline void rlamc_args_set_defaults(RlamcArgs *args) {
  args->enabled = false;
  args->depth = 1;
  for (int level_idx = 0; level_idx < RLAMC_MAX_DEPTH; level_idx++) {
    rlamc_level_set_defaults(&args->levels[level_idx]);
  }
  args->finalists = 0;
  args->control_samples = 0;
  args->stage_share = 0.5;
}

#endif
