#ifndef RLAMC_H
#define RLAMC_H

#include "../ent/best_move_carry.h"
#include "../ent/cross_set_cache.h"
#include "../ent/game.h"
#include "../ent/move.h"
#include "../ent/rlamc_args.h"
#include "../ent/win_pct.h"
#include "../util/io_util.h"
#include "census_rollout.h"
#include <stdbool.h>
#include <stdint.h>

// Recursive rollouts: a rollout policy that chooses a ply's play by a small
// search instead of by static equity alone (see rlamc_args.h and
// docs/lamc-plan.md, Phase 2).
//
// A search works on a copy of the rollout's game, so the rollout's own
// backups and board-undo record are never touched. Each recursive rollout:
//   1. draws the other player's rack again, from the tiles the player on turn
//      cannot see (the bag and that rack), with the bag shuffled by the
//      sample's seed. Every candidate's j-th rollout uses the same seed, so
//      candidates are compared on common random numbers;
//   2. plays the candidate, then horizon_plies plies: top static equity
//      plays, or, at the plies a deeper level names (RlamcArgs.levels),
//      plays chosen by a search of that level, on its own worker and its own
//      copy of the game;
//   3. scores the result for the player on turn as the sim scores a rollout:
//      the win percentage table at the spread plus the last two plays'
//      leaves, blended with spread by the sim's utility weights.

// With RlamcLevel.share, a recursive rollout plays only the other player's
// reply (horizon_plies is 1), and the search shares those replies between its
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
// empty the bag is valued by plain recursive rollouts instead: then the other
// player's rack is known to the player on turn.
//
// The racks drawn for a cached board come from every tile not on it, so the
// player on turn's own leave can appear in the other player's rack: an
// approximation that sharing between racks requires.

typedef struct RlamcWorker RlamcWorker;

// A worker for one thread's rollouts of games like game. The worker's copy
// shares cross_set_cache, which must be the thread's own.
RlamcWorker *rlamc_worker_create(const Game *game,
                                 CrossSetCache *cross_set_cache);
void rlamc_worker_destroy(RlamcWorker *worker);
// Starts a sim with seed sim_seed: empties the worker's cache.
void rlamc_worker_reset(RlamcWorker *worker, uint64_t sim_seed);
// The census that steers the sim's rollouts, or NULL, and the rollout ply of
// the replies the next search plays: its recursive rollouts' plays are then
// found with the census's help.
void rlamc_worker_set_census(RlamcWorker *worker,
                             CensusRolloutWorker *census_worker,
                             const CensusRollout *census, int reply_ply);

// How a recursive rollout is scored: the sim's own settings.
typedef struct RlamcEval {
  const WinPct *win_pcts;
  double utility_w_winpct;
  double utility_w_spread;
  double utility_spread_scale;
  // SimArgs.finish_endgame and finish_max_bag: recursive rollouts finish games
  // as the sim's rollouts do.
  bool finish_endgame;
  int finish_max_bag;
} RlamcEval;

// Whether a search of level chooses the play at ply (of the rollout of the
// level above it) with bag_tiles in the bag.
static inline bool rlamc_level_applies(const RlamcLevel *level, const int ply,
                                       const int bag_tiles) {
  if (ply > RLAMC_MAX_PLY_INDEX || ((level->plies_mask >> ply) & 1U) == 0) {
    return false;
  }
  if (level->max_bag > 0 && bag_tiles > level->max_bag) {
    return false;
  }
  return bag_tiles >= level->min_bag;
}

// Whether the sim's rollout chooses its play at ply by a recursive search.
static inline bool rlamc_applies(const RlamcArgs *args, const int ply,
                                 const int bag_tiles) {
  return args->enabled && rlamc_level_applies(&args->levels[0], ply, bag_tiles);
}

// The play the player on turn in game makes by a recursive search. game is left
// unchanged. The play stays valid until the worker's next search.
const Move *rlamc_choose_move(RlamcWorker *worker, const RlamcArgs *args,
                              const RlamcEval *eval, const Game *game,
                              uint64_t seed, BestMoveCarry *carry);

// Parses a recursive search spec into args: "off"; "on", which turns recursive
// rollouts on with the defaults (plies=0+1,lamc=1,dcache=1,m=4,h=2,fin=1;
// the best setting found has h=1 and fin=0); or levels separated by '/', each
// comma-separated key=value settings that change the defaults, which also turn
// recursive rollouts on. A level starts from the settings of the level before
// it. Keys:
//   plies   the recursive plies, joined by '+', for example 0 or 0+1
//   k       candidates            m       samples per candidate
//   h       static plies after the candidate in a recursive rollout
//   margin  points of static equity below the best a candidate may be
//   maxbag  the most tiles in the bag for a recursive search (0: no limit)
//   minbag  the fewest tiles in the bag for a recursive search
//   alloc   uniform or race       z       race_z
//   switch  switch_margin
//   share   1 to share recursive rollouts (see above), 0 not to; only for the
//           deepest level
//   finalists, stage  RlamcArgs.finalists and stage_share (any level)
//   cv      RlamcArgs.control_samples (any level)
//   dcache  1 to cache decisions (RlamcLevel.decision_cache), 0 not to
//   universe, seeds  RlamcLevel.universe and static_seeds: diverse
//           candidates
//   lamc    1 for a LAMC search over every play (RlamcLevel.lamc_search),
//           with pool, keep, rounds and beta, and stop (RlamcLevel.stop_z);
//           0 for the top k plays by static equity (k, margin, alloc, z, share)
//   pair    1 for a LAMC search that models the luck of common seeds
//           (RlamcLevel.paired)
//   cycle   1 for a LAMC search whose rollouts cycle the bag
//           (RlamcLevel.bag_cycle)
//   fin     0 for rollouts that skip the static finish from an empty bag,
//           1 (the default) for rollouts that play it (RlamcLevel.skip_finish)
//   dyn, step  1 for a LAMC search whose pool changes after every step
//           rollouts a play (RlamcLevel.dynamic and step)
// For example "plies=0+1/plies=0,lamc=0,k=3,m=2" runs a LAMC search at the
// sim's first two rollout plies, and a search of the top 3 static plays, of 2
// samples each, at the reply of each of its recursive rollouts.
// Settings not named keep their defaults.
void rlamc_args_parse(const char *spec, RlamcArgs *args,
                      ErrorStack *error_stack);

// Totals over every recursive search since the last reset, for experiments.
typedef struct RlamcTotals {
  uint64_t searches;
  // The searches of a level below the first.
  uint64_t inner_searches;
  uint64_t samples;
  uint64_t candidates;
  // Searches whose play was not the top play by static equity.
  uint64_t switched;
  // Shared searches' candidates found in the cache, and computed.
  uint64_t cache_hits;
  uint64_t cache_misses;
  // Searches answered by the decision cache.
  uint64_t decision_hits;
  // LAMC searches stopped early (RlamcLevel.stop_z).
  uint64_t stopped;
  // Finishes from an empty bag (see SimArgs.finish_endgame), and those found
  // in a worker's finish cache.
  uint64_t finishes;
  uint64_t finish_hits;
  // Rollouts whose bag is empty after the candidate and whose plays left are
  // all static, and those found in a worker's rollout cache.
  uint64_t static_rollouts;
  uint64_t rollout_hits;
} RlamcTotals;

void rlamc_totals_reset(void);
RlamcTotals rlamc_get_totals(void);

#endif
