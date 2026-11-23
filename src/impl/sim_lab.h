#ifndef SIM_LAB_H
#define SIM_LAB_H

// LAB, NOT FOR MERGE: what two changes to how a sim values its horizon are
// worth (see test/sim_horizon_test.c).
//
// A sim's rollout ends at its horizon with the spread plus the value of each
// player's last leave, with its pool shift (see pool_shift.h). With a lab
// set (rv_sim_lab_set), every rollout also works out what the horizon would
// be worth with plain leave values, with the shift of the simming player's
// leave alone, and with the last play's PAT term, and some rollouts play on
// to the end of the game, which says what the horizon was really worth. The
// sim's own results do not change.

#include <stdint.h>

enum {
  SIM_LAB_MAX_PLAYS = 32,
  SIM_LAB_MAX_WORKERS = 32,
  // Rollouts are split in four by their seed, the same way for every play,
  // so that some parts choose a play and the others value the choice. (The
  // code calls a part a half.)
  SIM_LAB_NUM_HALVES = 4,
  // The horizon's leave values: plain, with the pool shift of both players'
  // last leaves, or with the shift of the simming player's last leave only.
  SIM_LAB_NUM_LEAVE_MODES = 3,
  // How much of the last play's PAT term the horizon charges: none, half or
  // all of it.
  SIM_LAB_NUM_PAT_SHARES = 3,
  // A variant's index is its leave mode times SIM_LAB_NUM_PAT_SHARES plus
  // its PAT share. Variant 0 is the sim as it was before its horizon had
  // the pool shift.
  SIM_LAB_NUM_VARIANTS = SIM_LAB_NUM_LEAVE_MODES * SIM_LAB_NUM_PAT_SHARES,
  // The variant the sim itself is: its horizon has the pool shift of both
  // players' last leaves and no PAT term.
  SIM_LAB_SIM_VARIANT = SIM_LAB_NUM_PAT_SHARES,
  // What follows the horizon in a rollout played on: the next ply's score,
  // the next two plies with their leaves, and the rest of the game, each
  // less the horizon's value as the sim has it.
  SIM_LAB_TARGET_NEXT_PLY = 0,
  SIM_LAB_TARGET_TWO_PLIES = 1,
  SIM_LAB_TARGET_GAME_END = 2,
  SIM_LAB_NUM_TARGETS = 3,
  // The horizon's shift in parts: the simming player's last leave and the
  // opponent's (already signed for the simming player), each as its mover
  // sees the unseen tiles (the bag and the other rack, which is what move
  // generation uses) and from the bag alone, which is what a leave really
  // draws from in a rollout.
  SIM_LAB_SPLIT_OWN = 0,
  SIM_LAB_SPLIT_OPPONENT = 1,
  SIM_LAB_SPLIT_OWN_FROM_BAG = 2,
  SIM_LAB_SPLIT_OPPONENT_FROM_BAG = 3,
  // The plain leave values of the same two leaves, signed the same way.
  SIM_LAB_SPLIT_OWN_PLAIN = 4,
  SIM_LAB_SPLIT_OPPONENT_PLAIN = 5,
  SIM_LAB_NUM_SPLITS = 6,
  // The plies of a rollout whose terms are kept: the sim's and two more.
  SIM_LAB_MAX_TRACKED_PLIES = 8,
  SIM_LAB_MAX_SIM_PLIES = SIM_LAB_MAX_TRACKED_PLIES - 2,
};

// Sums over the rollouts of one play in one half, on one worker. Spreads and
// terms are in points and from the simming player's side.
typedef struct SimLabCell {
  uint64_t rollouts;
  // Each variant's horizon: the equity, win fraction and utility the sim
  // would record.
  double equity_sum[SIM_LAB_NUM_VARIANTS];
  double win_sum[SIM_LAB_NUM_VARIANTS];
  double utility_sum[SIM_LAB_NUM_VARIANTS];
  // The two terms themselves, over every rollout.
  double shift_sum;
  double pat_sum;
  // The shift in parts (see SIM_LAB_SPLIT_*), over every rollout.
  double split_sum[SIM_LAB_NUM_SPLITS];
  // The rollouts played on to the end of the game.
  uint64_t continued;
  double final_spread_sum;
  double final_win_sum;
  double final_utility_sum;
  // The value of a sim two plies deeper, as the sim is and with both terms.
  double deep_plain_sum;
  double deep_full_sum;
  // Moments, over the rollouts played on, of the two terms and of each
  // target.
  double pat_term_sum;
  double shift_term_sum;
  double pat_square_sum;
  double shift_square_sum;
  double pat_shift_sum;
  double target_sum[SIM_LAB_NUM_TARGETS];
  double target_square_sum[SIM_LAB_NUM_TARGETS];
  double target_pat_sum[SIM_LAB_NUM_TARGETS];
  double target_shift_sum[SIM_LAB_NUM_TARGETS];
} SimLabCell;

// A worker's checks of the lab's own arithmetic against the sim's and the
// move generator's.
typedef struct SimLabChecks {
  // Tile placements from a bag with tiles: the play's equity against its
  // score, leave value, shift, PAT term and pre-endgame adjustment.
  uint64_t placements;
  uint64_t placement_mismatches;
  // Exchanges and passes from a bag with tiles, the same way.
  uint64_t other_plays;
  uint64_t other_play_mismatches;
  // The lab's plain horizon leave values against the sim's leftover.
  uint64_t horizons;
  uint64_t horizon_mismatches;
  uint64_t positive_pat_terms;
} SimLabChecks;

typedef struct SimLab {
  // One rollout seed in this many is played on to the end of the game; 0
  // plays none on.
  int continue_one_in;
  SimLabCell cells[SIM_LAB_MAX_WORKERS][SIM_LAB_MAX_PLAYS][SIM_LAB_NUM_HALVES];
  SimLabChecks checks[SIM_LAB_MAX_WORKERS];
} SimLab;

// Sets the lab every sim's rollouts report to from now on; NULL sets none.
// Set it only while no sim is running.
void rv_sim_lab_set(SimLab *lab);

#endif
