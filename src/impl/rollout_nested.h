#ifndef ROLLOUT_NESTED_H
#define ROLLOUT_NESTED_H

#include "../ent/best_move_carry.h"
#include "../ent/cross_set_cache.h"
#include "../ent/game.h"
#include "../ent/move.h"
#include "../ent/nested_args.h"
#include "../ent/win_pct.h"
#include "../util/io_util.h"
#include "census_rollout.h"
#include <stdbool.h>
#include <stdint.h>

// Nested rollouts: a rollout policy that chooses a ply's play by a small
// search instead of by static equity alone (see nested_args.h and
// docs/lamc-plan.md, Phase 2).
//
// A search works on a copy of the rollout's game, so the rollout's own
// backups and board-undo record are never touched. Each nested rollout:
//   1. draws the other player's rack again, from the tiles the player on turn
//      cannot see (the bag and that rack), with the bag shuffled by the
//      sample's seed. Every candidate's j-th rollout uses the same seed, so
//      candidates are compared on common random numbers;
//   2. plays the candidate, then nested_plies plies of top static equity
//      plays;
//   3. scores the result for the player on turn as the sim scores a rollout:
//      the win percentage table at the spread plus the last two plays'
//      leaves, blended with spread by the sim's utility weights.

// With NestedArgs.share, a nested rollout plays only the other player's
// reply (nested_plies is 1), and the search shares those replies between its
// candidates and between searches. A candidate's value splits into what
// depends on the rollout (its score, and the leave it keeps from the player's
// rack) and what depends only on the board it leaves: the other player's best
// replies to that board, from racks drawn from the tiles not on it. The
// replies are cached by the board, so every rollout whose player can make the
// same play shares them, however its rack differs. Each cached sample j draws
// its racks by priorities fixed for each tile and j, so that the boards of
// two candidates, whose unseen tiles differ by the few each one plays, get
// nearly the same racks: their comparison is on common random numbers. A
// cache entry is a function of its board and the sim's seed alone, so
// results do not depend on which thread fills it. A candidate that would
// empty the bag is valued by plain nested rollouts instead: then the other
// player's rack is known to the player on turn.
//
// The racks drawn for a cached board come from every tile not on it, so the
// player on turn's own leave can appear in the other player's rack: an
// approximation that sharing between racks requires.

typedef struct NestedWorker NestedWorker;

// A worker for one thread's rollouts of games like game. The worker's copy
// shares cross_set_cache, which must be the thread's own.
NestedWorker *nested_worker_create(const Game *game,
                                   CrossSetCache *cross_set_cache);
void nested_worker_destroy(NestedWorker *worker);
// Starts a sim with seed sim_seed: empties the worker's cache.
void nested_worker_reset(NestedWorker *worker, uint64_t sim_seed);
// The census that steers the sim's rollouts, or NULL, and the rollout ply of
// the replies the next search plays: its nested rollouts' plays are then
// found with the census's help.
void nested_worker_set_census(NestedWorker *worker,
                              CensusRolloutWorker *census_worker,
                              const CensusRollout *census, int reply_ply);

// How a nested rollout is scored: the sim's own settings.
typedef struct NestedEval {
  const WinPct *win_pcts;
  double utility_w_winpct;
  double utility_w_spread;
  double utility_spread_scale;
  // SimArgs.finish_endgame and finish_max_bag: nested rollouts finish games
  // as the sim's rollouts do.
  bool finish_endgame;
  int finish_max_bag;
} NestedEval;

// Whether the rollout ply ply of game chooses by a nested search.
static inline bool nested_applies(const NestedArgs *args, const int ply,
                                  const int bag_tiles) {
  if (!args->enabled || ply > NESTED_MAX_PLY_INDEX ||
      ((args->plies_mask >> ply) & 1U) == 0) {
    return false;
  }
  if (args->max_bag > 0 && bag_tiles > args->max_bag) {
    return false;
  }
  return bag_tiles >= args->min_bag;
}

// The play the player on turn in game makes by a nested search. game is left
// unchanged. The play stays valid until the worker's next search.
const Move *nested_choose_move(NestedWorker *worker, const NestedArgs *args,
                               const NestedEval *eval, const Game *game,
                               uint64_t seed, BestMoveCarry *carry);

// Parses a nested search spec into args: "off", or comma-separated
// key=value settings, which turn nested rollouts on. Keys:
//   plies   the nested plies, joined by '+', for example 0 or 0+1
//   k       candidates            m       samples per candidate
//   h       static plies after the candidate in a nested rollout
//   margin  points of static equity below the best a candidate may be
//   maxbag  the most tiles in the bag for a nested search (0: no limit)
//   minbag  the fewest tiles in the bag for a nested search
//   alloc   uniform or race       z       race_z
//   switch  switch_margin
//   share   1 to share nested rollouts (see above), 0 not to
//   finalists, stage  NestedArgs.finalists and stage_share
//   dcache  1 to cache decisions (NestedArgs.decision_cache), 0 not to
//   universe, seeds  NestedArgs.universe and static_seeds: diverse candidates
//   lamc    1 for a LAMC search over every play (NestedArgs.lamc_search),
//           with pool, keep, rounds and beta
// Settings not named keep their defaults.
void nested_args_parse(const char *spec, NestedArgs *args,
                       ErrorStack *error_stack);

// Totals over every nested search since the last reset, for experiments.
typedef struct NestedTotals {
  uint64_t searches;
  uint64_t samples;
  uint64_t candidates;
  // Searches whose play was not the top play by static equity.
  uint64_t switched;
  // Shared searches' candidates found in the cache, and computed.
  uint64_t cache_hits;
  uint64_t cache_misses;
  // Searches answered by the decision cache.
  uint64_t decision_hits;
} NestedTotals;

void nested_totals_reset(void);
NestedTotals nested_get_totals(void);

#endif
