#ifndef CENSUS_ROLLOUT_H
#define CENSUS_ROLLOUT_H

#include "../def/board_defs.h"
#include "../ent/game.h"
#include "../ent/move.h"
#include "../ent/rack.h"
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
  // The rollout plies served: 0 (the reply to the candidate) through 3.
  // Later plies gain little: their lanes have mostly changed.
  CENSUS_ROLLOUT_MAX_PLY = 3,
  // The threads that build a sim's tables, at most: each holds a
  // lattice-sized scratch table.
  CENSUS_ROLLOUT_MAX_BUILD_THREADS = 16,
};

// The census of one sim at a time: built by census_rollout_prepare, then only
// read by the sim's workers.
typedef struct CensusRollout CensusRollout;

// The census builds each sim's tables on number_of_build_threads threads, at
// most CENSUS_ROLLOUT_MAX_BUILD_THREADS: a sim's workers would wait for the
// builds anyway.
CensusRollout *census_rollout_create(int number_of_build_threads);
void census_rollout_destroy(CensusRollout *census);

// At the start of a sim, on the thread that starts it, before any rollout:
// builds the play scores of the position and of each candidate's board.
void census_rollout_prepare(CensusRollout *census, const Game *game,
                            const MoveList *candidates);

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
// the candidate.
const Move *census_rollout_get_best_move(CensusRolloutWorker *worker,
                                         const CensusRollout *census,
                                         Game *game, MoveList *move_list,
                                         int ply);

// About the worker's last census_rollout_get_best_move: whether the census
// served it (a rollout ply up to CENSUS_ROLLOUT_MAX_PLY, from a bag with
// tiles, with PAT off), and whether it steered it (knew some anchor's best).
bool census_rollout_worker_last_served(const CensusRolloutWorker *worker);
bool census_rollout_worker_last_steered(const CensusRolloutWorker *worker);

// The two words a worker remembers a rack's bests in the position's anchors
// by, for a bag class: 0 for the bags that give no play a pre-endgame
// adjustment, else the bag. Exposed for its test.
void census_rollout_root_cache_key(int bag_class, const Rack *rack,
                                   uint64_t *key, uint64_t *check);

#endif
