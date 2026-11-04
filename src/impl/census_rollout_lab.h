#ifndef CENSUS_ROLLOUT_LAB_H
#define CENSUS_ROLLOUT_LAB_H

#include "../ent/best_move_carry.h"
#include "../ent/game.h"
#include "../ent/move.h"
#include <stdbool.h>
#include <stdint.h>

// An experiment, not a feature: what a census-built cache of each lane's best
// play for every rack would save a simulation's rollouts, priced before any
// such cache is built.
//
// A lane holds the same plays at the same scores while its squares stay as
// they were, so a census of a board, taken lane by lane, gives every rack's
// best equity in every lane of it. A rollout's move generation could then
// skip each lane that is still as it was on that board and is worse for its
// rack than the best such lane, and drop the anchors that cannot reach that
// lane's best, searching only the lanes the rollout's moves have changed. The
// census board is the sim's position (root) or the board after the candidate
// (cand), and the cache keeps the k best lanes of each rack.
//
// Recording replays a single-threaded sim and finds, for every rollout
// generation, the lanes and the floor each variant's cache would give it,
// from every play of the rack on the census board and on the current one (a
// lane's best is the same on both while its hash is). Replaying runs the sim
// again with one variant's skips and floor, timing its rollout generations;
// the timing modes time the sim's own generations, with and without the
// best-move carry. Every replayed generation must choose the recorded move.

typedef enum {
  CRL_MODE_OFF,
  CRL_MODE_TIME_CARRY,
  CRL_MODE_TIME_PLAIN,
  CRL_MODE_RECORD,
  CRL_MODE_REPLAY,
  // A real lazy census: per-anchor play scores of the root and of each
  // candidate's changed anchors, gathered for each rack drawn, steering the
  // carry's generation.
  CRL_MODE_LAZY,
} crl_mode_t;

enum {
  CRL_MAX_PLIES = 8,
  CRL_BOARD_ROOT = 0,
  CRL_BOARD_CAND = 1,
  CRL_NUM_BOARDS = 2,
  CRL_NUM_TOPK = 5,
  CRL_NUM_LANE_VARIANTS = CRL_NUM_BOARDS * CRL_NUM_TOPK,
  // After the lane variants, one a board that knows every anchor's best: of
  // the anchors of the lanes that are as they were, and of the anchors whose
  // plays reach no changed square of a changed lane.
  CRL_NUM_VARIANTS = CRL_NUM_LANE_VARIANTS + CRL_NUM_BOARDS,
};

// The k of each top-k index; the last keeps every lane.
extern const int crl_topk_values[CRL_NUM_TOPK];

static inline int crl_variant(int board, int topk_index) {
  return board * CRL_NUM_TOPK + topk_index;
}

static inline int crl_anchor_variant(int board) {
  return CRL_NUM_LANE_VARIANTS + board;
}

// Whether the rollouts go through the lab: read on the sim's hot path.
extern bool crl_active;

// The buckets of the best-move carry the next sim creates or resets, a
// power of two, or 0 for SIM_BEST_MOVE_CARRY_BUCKETS.
extern int crl_carry_buckets;

// Replay modes use variant and, with replay_with_carry, the best-move carry
// as well.
void crl_set_mode(crl_mode_t mode, int variant, bool replay_with_carry);

// Controls: which parts of a hit's record a replay applies (both by default,
// reset by crl_set_mode). With neither, a replay generates as plain does.
enum {
  CRL_REPLAY_SKIPS = 1,
  CRL_REPLAY_FLOOR = 2,
};
void crl_set_replay_parts(int parts);
crl_mode_t crl_get_mode(void);
// Before each sim: the next rollout's board is the new root, and replaying
// starts from the sim's first recorded generation.
void crl_new_sim(void);
// Forgets every recorded generation.
void crl_clear_records(void);
void crl_reset_stats(void);

// A sim worker's state in the lab: what its rollouts read and count, so
// that workers on different threads share nothing they write. A sim worker
// creates one and destroys it with itself.
typedef struct CrlWorker CrlWorker;
CrlWorker *crl_worker_create(void);
void crl_worker_destroy(CrlWorker *worker);

// At the start of a sim, on the thread that starts it, before any rollout:
// the lazy census builds the play scores of the position and of each
// candidate's board here, which every worker then only reads.
void crl_prepare_sim(const Game *game, const MoveList *candidates);

// In sim_rollout: the game before the candidate is played, and after.
void crl_before_candidate(CrlWorker *worker, const Game *game);
void crl_after_candidate(CrlWorker *worker, const Game *game,
                         const Move *candidate);

// The lazy census's options: it serves the eligible generations of rollout
// plies up to max_ply (with low_bag, any bag with tiles, the pre-endgame
// adjustments added by tiles played; else bags of PEG_ADJUST_VALUES_LENGTH
// tiles or more), and with verify also generates each one plainly (untimed)
// and counts the moves that differ as mismatches.
void crl_set_lazy_options(int max_ply, bool verify, bool low_bag);

typedef struct CrlLazyStats {
  int64_t root_build_ns;
  int64_t candidate_build_ns;
  int candidate_builds;
  uint64_t root_entries;
  uint64_t candidate_entries;
  uint64_t root_gather_hits;
  uint64_t root_gather_lookups;
} CrlLazyStats;

const CrlLazyStats *crl_get_lazy_stats(void);

// Whether the lazy census steers the carry's generations (the default) or
// plain ones.
void crl_set_lazy_carry(bool with_carry);
// In sim_rollout: the best move of the player on turn, ply 0 being the reply
// to the candidate.
const Move *crl_get_best_move(CrlWorker *worker, Game *game,
                              MoveList *move_list, BestMoveCarry *carry,
                              int ply);

typedef struct CrlPlyStats {
  uint64_t queries;
  uint64_t eligible;
  int64_t generation_ns;
  uint64_t mismatches;
  uint64_t replay_hits;
  // Recording only.
  uint64_t incomplete;
  uint64_t lanes_with_anchors;
  uint64_t dirty_lanes[CRL_NUM_BOARDS];
  uint64_t best_in_clean_lane[CRL_NUM_BOARDS];
  uint64_t self_check_violations[CRL_NUM_BOARDS];
  uint64_t variant_hits[CRL_NUM_VARIANTS];
  uint64_t variant_searched_lanes[CRL_NUM_VARIANTS];
  int64_t hash_ns;
  // Anchors, recording only: every anchor of the board now, the anchors of
  // changed lanes whose plays reach no changed square, the best moves in a
  // known anchor and those that touch no changed square at all, the anchors
  // each variant searches, and clean anchors whose best differs.
  uint64_t anchors;
  uint64_t clean_anchors_in_dirty_lanes[CRL_NUM_BOARDS];
  uint64_t best_in_known_anchor[CRL_NUM_BOARDS];
  uint64_t best_untouched[CRL_NUM_BOARDS];
  uint64_t anchor_self_check_violations[CRL_NUM_BOARDS];
  uint64_t variant_searched_anchors[CRL_NUM_VARIANTS];
  // Lazy census: the time of the gathers within hash_ns, and, verifying,
  // the generations whose plain move or census-steered move is not the best
  // of every play.
  int64_t gather_ns;
  uint64_t plain_vs_exhaustive;
  uint64_t census_vs_exhaustive;
  // Verifying: the known anchors whose census values were checked against
  // every play, and those outside the bounds the plays give.
  uint64_t anchors_checked;
  uint64_t anchor_value_violations;
} CrlPlyStats;

// The stats of a ply summed over every worker; read it with no sim running.
const CrlPlyStats *crl_get_ply_stats(int ply);
uint64_t crl_get_number_of_records(void);

#endif
