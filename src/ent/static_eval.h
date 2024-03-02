#ifndef STATIC_EVAL_H
#define STATIC_EVAL_H

#include "../def/letter_distribution_defs.h"
#include "../def/rack_defs.h"
#include "../def/static_eval_defs.h"

#include "../ent/board.h"
#include "../ent/klv.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/rack.h"

// The length of this array should match PEG_ADJUST_VALUES_LENGTH
static const double peg_adjust_values[] = {0, 0, 0, 0, 0, 0, 0,
                                           0, 0, 0, 0, 0, 0};
// These are quackle values, but we can probably come up with our
// own at some point.
// static const double peg_adjust_values[] = {0, -8, 0, -0.5, -2, -3.5, -2,
//                                            2, 10, 7, 4, -1, -2};

static inline double placement_adjustment(const LetterDistribution *ld, const Move *move) {
  int start = move_get_col_start(move);
  int end = start + move_get_tiles_played(move);

  int j = start;
  double penalty = 0;
  double v_penalty = OPENING_HOTSPOT_PENALTY;

  while (j < end) {
    int tile = move_get_tile(move, j - start);
    if (get_is_blanked(tile)) {
      tile = get_unblanked_machine_letter(tile);
    }
    if (ld_get_is_vowel(ld, tile) && (j == 2 || j == 6 || j == 8 || j == 12)) {
      penalty += v_penalty;
    }
    j++;
  }
  return penalty;
}

static inline double endgame_nonoutplay_adjustment(int player_rack_score) {
  return ((-(double)player_rack_score) *
          NON_OUTPLAY_LEAVE_SCORE_MULTIPLIER_PENALTY) -
         NON_OUTPLAY_CONSTANT_PENALTY;
}

static inline double endgame_outplay_adjustment(int opponent_rack_score) {
  return 2 * ((double)opponent_rack_score);
}

static inline double standard_endgame_adjustment(const LetterDistribution *ld,
                                   const Rack *player_leave,
                                   const Rack *opp_rack) {
  if (!rack_is_empty(player_leave)) {
    // This play is not going out. We should penalize it by our own score
    // plus some constant.
    return endgame_nonoutplay_adjustment(rack_get_score(ld, player_leave));
  }
  return endgame_outplay_adjustment(rack_get_score(ld, opp_rack));
}

double static_eval_get_shadow_equity(const LetterDistribution *ld,
                                     const Rack *opp_rack,
                                     const double *best_leaves,
                                     const int *descending_tile_scores,
                                     int number_of_tiles_in_bag,
                                     int number_of_letters_on_rack,
                                     int tiles_played);

// Assumes all fields of the move are set except the equity.
static inline double static_eval_get_move_equity_with_leave_value(
    const LetterDistribution *ld, const Move *move, const Board *board,
    const Rack *player_leave, const Rack *opp_rack, int number_of_tiles_in_bag,
    double leave_value) {
  double leave_adjustment = 0;
  double other_adjustments = 0;

  if (board_get_tiles_played(board) == 0 &&
      move_get_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
    other_adjustments = placement_adjustment(ld, move);
  }

  if (number_of_tiles_in_bag > 0) {
    leave_adjustment = leave_value;
    int bag_plus_rack_size =
        number_of_tiles_in_bag - move_get_tiles_played(move) + RACK_SIZE;
    if (bag_plus_rack_size < PEG_ADJUST_VALUES_LENGTH) {
      other_adjustments += peg_adjust_values[bag_plus_rack_size];
    }
  } else {
    other_adjustments +=
        standard_endgame_adjustment(ld, player_leave, opp_rack);
  }

  return ((double)move_get_score(move)) + leave_adjustment + other_adjustments;
}

double static_eval_get_move_equity(const LetterDistribution *ld, const KLV *klv,
                                   const Move *move, const Board *board,
                                   const Rack *player_leave,
                                   const Rack *opp_rack,
                                   int number_of_tiles_in_bag);

int static_eval_get_move_score(const LetterDistribution *ld, const Move *move,
                               Board *board, int cross_set_index);
#endif
