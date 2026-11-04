#ifndef CENSUS_ROLLOUT_H
#define CENSUS_ROLLOUT_H

#include "../def/board_defs.h"
#include "../ent/best_move_carry.h"
#include "../ent/board.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/move.h"
#include <stdbool.h>
#include <stdint.h>

// A census of a sim's position that steers its rollouts' move generation.
//
// At the start of a sim, before any rollout, it builds per-anchor play-score
// tables: for every placed multiset P, the anchors P is played from and its
// best score in each, for the position and, for each candidate, for the
// anchors that candidate changes. Each rollout generation then gathers its
// rack's best equity in every anchor from those tables (score, leave, and the
// pre-endgame adjustment by tiles played) and skips every anchor known to be
// worse than the best known one. An anchor is known when its lane is as it
// was on the candidate's board, or when its zone (the squares its plays can
// cover or border with the rack's tiles) misses every changed square on both
// boards. Every steered generation finds the move a plain one finds.
//
// The census leaves out PAT's term, so the two do not mix: a sim refuses the
// census while its rollouts use PAT (see the patrollout option), and the
// census steers no generation of a player whose PAT is on.

enum {
  CENSUS_ROLLOUT_NUM_LANES = 2 * BOARD_DIM,
  // The rollout plies served by default: 0 (the reply to the candidate)
  // through 3. Later plies gain little: their lanes have mostly changed.
  CENSUS_ROLLOUT_DEFAULT_MAX_PLY = 3,
};

// An anchor or lane with no play: below every equity a move has.
#define CENSUS_ROLLOUT_NO_PLAY EQUITY_MIN_VALUE

typedef struct CensusRolloutOptions {
  // The census serves rollout plies 0 through max_ply.
  int max_ply;
  // Whether generations with fewer than PEG_ADJUST_VALUES_LENGTH tiles in
  // the bag are served too, their plays' pre-endgame adjustments added by
  // tiles played. Without it, only bags that adjust no play are served.
  bool low_bag;
  // Whether the census's generations use the sim's best-move carry. Without
  // the carry they are faster: its buckets thrash once racks stop recurring.
  bool with_carry;
} CensusRolloutOptions;

static inline CensusRolloutOptions census_rollout_default_options(void) {
  return (CensusRolloutOptions){.max_ply = CENSUS_ROLLOUT_DEFAULT_MAX_PLY,
                                .low_bag = true,
                                .with_carry = false};
}

// The census of one sim at a time: built by census_rollout_prepare, then only
// read by the sim's workers.
typedef struct CensusRollout CensusRollout;

CensusRollout *census_rollout_create(const CensusRolloutOptions *options);
void census_rollout_destroy(CensusRollout *census);

// At the start of a sim, on the thread that starts it, before any rollout:
// builds the play scores of the position and of each candidate's board.
void census_rollout_prepare(CensusRollout *census, const Game *game,
                            const MoveList *candidates);

// The census's builds since it was created or reset, summed over its sims.
typedef struct CensusRolloutBuildStats {
  int candidate_builds;
  uint64_t root_entries;
  uint64_t candidate_entries;
} CensusRolloutBuildStats;

const CensusRolloutBuildStats *
census_rollout_get_build_stats(const CensusRollout *census);
void census_rollout_reset_build_stats(CensusRollout *census);

// A sim worker's state: what its rollouts read and remember, so that workers
// on different threads share nothing they write. A worker serves any census;
// it forgets what it remembered when the census it is given prepares a new
// sim.
typedef struct CensusRolloutWorker CensusRolloutWorker;

CensusRolloutWorker *census_rollout_worker_create(void);
void census_rollout_worker_destroy(CensusRolloutWorker *worker);

// In a rollout, after its candidate is played: the census board the
// rollout's generations are steered from.
void census_rollout_worker_set_candidate(CensusRolloutWorker *worker,
                                         const CensusRollout *census,
                                         const Move *candidate);

// In a rollout: the best move of the player on turn, ply 0 being the reply to
// the candidate. carry is the sim's best-move carry, used only with the
// with_carry option.
const Move *census_rollout_get_best_move(CensusRolloutWorker *worker,
                                         const CensusRollout *census,
                                         Game *game, MoveList *move_list,
                                         BestMoveCarry *carry, int ply);

// About the worker's last census_rollout_get_best_move: whether the census
// served it, and whether it steered it (knew some anchor's best).
bool census_rollout_worker_last_served(const CensusRolloutWorker *worker);
bool census_rollout_worker_last_steered(const CensusRolloutWorker *worker);
// For a steered generation: by lane, the anchors whose bests the census knew,
// and the best of the anchor at col (CENSUS_ROLLOUT_NO_PLAY if none).
uint16_t
census_rollout_worker_get_known_anchors(const CensusRolloutWorker *worker,
                                        int lane);
Equity census_rollout_worker_get_known_value(const CensusRolloutWorker *worker,
                                             int lane, int col);

// The worker's lookups of a rack's root-table bests, and those it had
// remembered, since it was created or reset.
uint64_t
census_rollout_worker_get_root_lookups(const CensusRolloutWorker *worker);
uint64_t census_rollout_worker_get_root_hits(const CensusRolloutWorker *worker);
void census_rollout_worker_reset_counts(CensusRolloutWorker *worker);

// The board geometry the census reads, for experiments that compare against
// it.

// A lane's squares as the generator reads them (lane BOARD_DIM * dir + row).
const Square *census_rollout_lane_squares(const Game *game, int lane);
// The columns of a lane's anchors.
uint16_t census_rollout_anchor_cols(const Square *squares);
// The squares of a lane whose letter, cross set, cross score, cross word flag
// or anchor differ between the two boards: what decides which plays a lane
// holds and what they score, square by square.
uint16_t census_rollout_changed_cols(const Square *now, const Square *then);
// For each anchor of a lane, the squares its plays can cover or border with
// rack_tiles tiles (see census_rollout.c).
void census_rollout_anchor_zones(const Square *squares, uint16_t anchor_cols,
                                 int rack_tiles, uint16_t *zones);
// A hash of a move's placement, score and equity, which tells candidates
// apart.
uint64_t census_rollout_move_signature(const Move *move);

#endif
