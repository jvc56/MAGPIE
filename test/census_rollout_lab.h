#ifndef CENSUS_ROLLOUT_LAB_H
#define CENSUS_ROLLOUT_LAB_H

#include <stdbool.h>
#include <stdint.h>

// An experiment, not a feature: what a census-built cache of each lane's best
// play for every rack would save a simulation's rollouts, priced before any
// such cache is built, and the census rollout (census_rollout.h) measured
// and verified against it.
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
//
// The lab takes over the sims' rollouts through rv_sim_set_rollout_hooks
// while its mode is not CRL_MODE_OFF.

typedef enum {
  CRL_MODE_OFF,
  CRL_MODE_TIME_CARRY,
  CRL_MODE_TIME_PLAIN,
  CRL_MODE_RECORD,
  CRL_MODE_REPLAY,
  // The census rollout, with the lab's options, timed and optionally
  // verified.
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

// The buckets of the best-move carry the next sim creates or resets, a
// power of two, or 0 for SIM_BEST_MOVE_CARRY_BUCKETS. Set it before
// crl_set_mode.
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

// The census rollout's options in the lazy mode: it serves the eligible
// generations of rollout plies up to max_ply (with low_bag, any bag with
// tiles; else bags of PEG_ADJUST_VALUES_LENGTH tiles or more), and with
// verify the lab also generates each one plainly and from every play
// (untimed), counting the moves that differ, and checks the census's anchor
// values against every play.
void crl_set_lazy_options(int max_ply, bool verify, bool low_bag);

typedef struct CrlLazyStats {
  // The census builds at the sims' starts.
  int64_t build_ns;
  int candidate_builds;
  uint64_t root_entries;
  uint64_t candidate_entries;
  uint64_t root_gather_hits;
  uint64_t root_gather_lookups;
} CrlLazyStats;

const CrlLazyStats *crl_get_lazy_stats(void);

// Whether the census rollout's generations use the best-move carry (the
// default) or are plain.
void crl_set_lazy_carry(bool with_carry);
// The threads that build the census's tables at each sim's start (1 by
// default).
void crl_set_build_threads(int build_threads);

typedef struct CrlPlyStats {
  uint64_t queries;
  uint64_t eligible;
  // The time of the generations, census included in the lazy mode.
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
  // Lazy census, verifying: the generations whose plain move or
  // census-steered move is not the best of every play.
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
