#ifndef OUTCOME_FEATURES_H
#define OUTCOME_FEATURES_H

#include "../ent/game.h"
#include "../ent/move.h"
#include "../ent/rack.h"
#include "../ent/xoshiro.h"
#include <stdint.h>

// Features computed at the post-move-pre-draw moment from us's
// perspective.
typedef struct {
  // single-tile features (us + opp)
  double us_st_frac_playable;
  double us_st_top1;
  double us_st_top2;
  double opp_st_frac_playable;
  double opp_st_top1;
  double opp_st_top2;
  // bingo prob features (sampled)
  double us_bingo_prob;
  double opp_bingo_prob;
  // misc scalars
  int unplayed_blanks; // count of blanks in the unseen pool
  int tiles_unseen;    // total tiles in unseen pool (bag + opp rack)
  int score_diff;      // us_score - opp_score (signed) at this position
  int us_leave_value;  // KLV leave value for us's leave (Equity millipoints)
} OutcomeFeatures;

// Computes all features for the recorded position.
//
// Caller responsibilities:
// - The game's board is already in its post-move state with
//   cross_sets updated (true after play_move).
// - us_player_index is the player who just played (whose features and
//   eventual outcome we're recording).
// - us_leave is the actual leave from the move (size 0..6).
// - pool_counts is the reconstructed pre-draw unseen pool from us's
//   POV: bag (post-draw, current) + tiles drawn during play_move +
//   opp's rack. Length MAX_ALPHABET_SIZE; pool_size = sum.
// - Scores at us_player_index already include the played move (true
//   after play_move).
//
// Mutates and restores game's on-turn-index and both players' racks.
//
// thread_index, prng: per-thread state for bingo_exists / sampling.
// bingo_samples: number of random racks per side for bingo prob (the
//                project notes settled on 14).
void outcome_features_compute(Game *game, MoveList *mv_list, int thread_index,
                              int us_player_index, const Rack *us_leave,
                              const uint8_t *pool_counts, int pool_size,
                              int bingo_samples, XoshiroPRNG *prng,
                              OutcomeFeatures *out);

#endif
