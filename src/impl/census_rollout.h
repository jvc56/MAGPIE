#ifndef CENSUS_ROLLOUT_H
#define CENSUS_ROLLOUT_H

#include "../def/board_defs.h"
#include "../ent/equity.h"
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
// The census leaves out PAT's term. With board-only PAT (see
// pat_board_cache.h) the term has no positive part, so a known anchor's best
// equity caps its plays' equity without the term, and the generation adds
// the lane's bound on the term (see MoveGenArgs.anchor_equity_caps): it
// skips only the anchors without a play, and searches the others only while
// their caps can still beat the best move. With the term static evaluation
// uses the two do not mix: a sim refuses the census while its rollouts use
// that PAT (see the patrollout and patrolloutfast options), and the census
// steers no generation of a player whose PAT is on without a board cache.

enum {
  CENSUS_ROLLOUT_NUM_LANES = 2 * BOARD_DIM,
  // The rollout plies served: 0 (the reply to the candidate) through 3.
  // Later plies gain little: their lanes have mostly changed.
  CENSUS_ROLLOUT_MAX_PLY = 3,
  // The threads that build a sim's tables, at most: each holds a
  // lattice-sized scratch table.
  CENSUS_ROLLOUT_MAX_BUILD_THREADS = 16,
  // For -scensus auto (see census_rollout_build_pays): the table entries
  // whose build one rollout ply's steering pays for, and the plies of a
  // rollout that pay. On CSW24 an entry takes about 0.3 us to build, and
  // steering saves about 22 us in each of a rollout's first two plies, and
  // little in the later ones.
  CENSUS_ROLLOUT_AUTO_ENTRIES_PER_PLY = 60,
  CENSUS_ROLLOUT_AUTO_PAYING_PLIES = 2,
  // For -scensus auto (see census_rollout_build_may_pay): a table holds at
  // least one entry an anchor for every 2^shift full racks the unseen letters
  // make. On CSW24 the position's tables hold from 1 in 670 (mid-game) to 1
  // in 60 (an open early board).
  CENSUS_ROLLOUT_AUTO_FEWEST_ENTRIES_SHIFT = 10,
};

// The memory -scensus auto leaves to the system and other programs, as the
// tables -wmp, -wit and -rit auto choose do.
#define CENSUS_ROLLOUT_MEMORY_RESERVE_BYTES 1000000000ULL

// Whether the census can steer the game's rollouts: a classic game whose
// players share a word graph and leaves, since the census reads the first
// player's, in an alphabet a census lattice holds.
bool census_rollout_supports(const Game *game);

// The memory, in bytes, that the census of a sim of number_of_candidates
// candidates from the game's position takes on number_of_threads threads,
// estimated from above: its leave and scratch tables, a table for the
// position and each candidate, and each worker's cache of the position's
// anchors. 0 if census_rollout_supports does not hold.
uint64_t census_rollout_memory_bytes(const Game *game, int number_of_threads,
                                     int number_of_candidates);

// Whether a census of census_bytes fits in this machine's memory, as
// -scensus auto decides: within a quarter of physical_bytes, and within
// available_bytes less a reserve of CENSUS_ROLLOUT_MEMORY_RESERVE_BYTES for
// the system and other programs. A negative available_bytes (where it cannot
// be read) leaves the quarter alone.
bool census_rollout_fits_in_memory(uint64_t census_bytes,
                                   uint64_t physical_bytes,
                                   int64_t available_bytes);

// For -scensus auto: whether the census of this sim fits in this machine's
// memory, from what census_rollout_supports, census_rollout_memory_bytes and
// census_rollout_fits_in_memory say, held_bytes being what a census already
// holds for the sim (which it reuses).
bool census_rollout_auto_fits(const Game *game, int number_of_threads,
                              int number_of_candidates, uint64_t held_bytes);

// For -scensus auto: whether the candidates' tables are worth building.
// Their entries are predicted from the position's table, at its root_entries
// per root_anchors for each of the candidate_anchors their tables are built
// for, and they are worth building when that takes less time than steering
// saves in rollouts rollouts of plies plies, counting up to
// CENSUS_ROLLOUT_AUTO_PAYING_PLIES plies of each.
bool census_rollout_build_pays(uint64_t root_entries, int root_anchors,
                               int candidate_anchors, uint64_t rollouts,
                               int plies);

// For -scensus auto, before building anything: whether the candidates' tables
// may be worth building (see census_rollout_build_pays), from the fewest
// entries they are likely to hold, one for every
// 2^CENSUS_ROLLOUT_AUTO_FEWEST_ENTRIES_SHIFT of the full_racks the unseen
// letters make, in each of the candidate_anchors.
bool census_rollout_build_may_pay(uint64_t full_racks, int candidate_anchors,
                                  uint64_t rollouts, int plies);

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

// For -scensus auto: census_rollout_prepare when the build pays for itself.
// For a sim of at least rollouts rollouts of plies plies, it builds the
// position's table if census_rollout_build_may_pay holds, then the
// candidates' if census_rollout_build_pays holds. Returns whether it built
// them; if not, the census steers none of the sim's rollouts.
bool census_rollout_prepare_if_it_pays(CensusRollout *census, const Game *game,
                                       const MoveList *candidates,
                                       uint64_t rollouts, int plies);

// The memory, in bytes, that a census holds: what census_rollout_memory_bytes
// estimates, as allocated.
uint64_t census_rollout_held_bytes(const CensusRollout *census);

// A sim worker's state: what its rollouts read and remember, so that workers
// on different threads share nothing they write. A worker serves any census;
// it forgets what it remembered when the census it is given prepares a new
// sim.
typedef struct CensusRolloutWorker CensusRolloutWorker;

CensusRolloutWorker *census_rollout_worker_create(void);
void census_rollout_worker_destroy(CensusRolloutWorker *worker);
uint64_t census_rollout_worker_held_bytes(const CensusRolloutWorker *worker);

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
// by, given peg_by_tiles_played, the mover's pre-endgame adjustments from the
// bag as peg_adjust_fill_by_tiles_played sets them (entries 1 to RACK_SIZE
// are read): the bests are the same wherever the rack and they are. Exposed
// for its test.
void census_rollout_root_cache_key(const Equity *peg_by_tiles_played,
                                   const Rack *rack, uint64_t *key,
                                   uint64_t *check);

#endif
