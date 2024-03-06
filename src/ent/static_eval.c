#include "../def/letter_distribution_defs.h"
#include "../def/rack_defs.h"
#include "../def/static_eval_defs.h"

#include "../ent/board.h"
#include "../ent/klv.h"
#include "../ent/move.h"
#include "../ent/rack.h"

// The length of this array should match PEG_ADJUST_VALUES_LENGTH
static const double peg_adjust_values[] = {0, 0, 0, 0, 0, 0, 0,
                                           0, 0, 0, 0, 0, 0};
// These are quackle values, but we can probably come up with our
// own at some point.
// static const double peg_adjust_values[] = {0, -8, 0, -0.5, -2, -3.5, -2,
//                                             2, 10, 7, 4,    -1, -2};

double placement_adjustment(const LetterDistribution *ld, const Move *move) {
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

double endgame_nonoutplay_adjustment(int player_rack_score) {
  return ((-(double)player_rack_score) *
          NON_OUTPLAY_LEAVE_SCORE_MULTIPLIER_PENALTY) -
         NON_OUTPLAY_CONSTANT_PENALTY;
}

double endgame_outplay_adjustment(int opponent_rack_score) {
  return 2 * ((double)opponent_rack_score);
}

double shadow_endgame_adjustment(const LetterDistribution *ld,
                                 const Rack *opp_rack,
                                 int number_of_letters_on_rack,
                                 int tiles_played,
                                 int lowest_possible_rack_score) {
  if (number_of_letters_on_rack > tiles_played) {
    // This play is not going out. We should penalize it by our own score
    // plus some constant.
    // However, we don't yet know the score of our own remaining tiles yet,
    // so we need to assume it is as small as possible for this anchor's
    // highest_possible_equity to be valid.
    return endgame_nonoutplay_adjustment(lowest_possible_rack_score);
  }
  return endgame_outplay_adjustment(rack_get_score(ld, opp_rack));
}

double static_eval_get_shadow_equity(const LetterDistribution *ld,
                                     const Rack *opp_rack,
                                     const double *best_leaves,
                                     const int *descending_tile_scores,
                                     int number_of_tiles_in_bag,
                                     int number_of_letters_on_rack,
                                     int tiles_played) {
  double equity = 0;
  if (number_of_tiles_in_bag > 0) {
    // Bag is not empty: use leave values
    equity += best_leaves[number_of_letters_on_rack - tiles_played];
    // Apply preendgame heuristic if this play would empty the bag or leave
    // few enough tiles remaining.
    int bag_plus_rack_size = number_of_tiles_in_bag - tiles_played + RACK_SIZE;
    if (bag_plus_rack_size < PEG_ADJUST_VALUES_LENGTH) {
      equity += peg_adjust_values[bag_plus_rack_size];
    }
  } else {
    // Bag is empty: add double opponent's rack if playing out, otherwise
    // deduct a penalty based on the score of our tiles left after this play.
    int lowest_possible_rack_score = 0;
    for (int i = tiles_played; i < number_of_letters_on_rack; i++) {
      lowest_possible_rack_score += descending_tile_scores[i];
    }
    equity +=
        shadow_endgame_adjustment(ld, opp_rack, number_of_letters_on_rack,
                                  tiles_played, lowest_possible_rack_score);
  }
  return equity;
}

double standard_endgame_adjustment(const LetterDistribution *ld,
                                   const Rack *player_leave,
                                   const Rack *opp_rack) {
  if (!rack_is_empty(player_leave)) {
    // This play is not going out. We should penalize it by our own score
    // plus some constant.
    return endgame_nonoutplay_adjustment(rack_get_score(ld, player_leave));
  }
  return endgame_outplay_adjustment(rack_get_score(ld, opp_rack));
}

// Assumes all fields of the move are set except the equity.
double static_eval_get_move_equity_with_leave_value(
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

// Assumes all fields of the move are set except the equity.
double static_eval_get_move_equity(const LetterDistribution *ld, const KLV *klv,
                                   const Move *move, const Board *board,
                                   const Rack *player_leave,
                                   const Rack *opp_rack,
                                   int number_of_tiles_in_bag) {
  double leave_equity = 0;
  if (player_leave && !rack_is_empty(player_leave)) {
    leave_equity = klv_get_leave_value(klv, player_leave);
  }
  return static_eval_get_move_equity_with_leave_value(
      ld, move, board, player_leave, opp_rack, number_of_tiles_in_bag,
      leave_equity);
}

int static_eval_get_move_score(const LetterDistribution *ld, const Move *move,
                               Board *board, int cross_set_index) {
  int tiles_played = move_get_tiles_played(move);
  int tiles_length = move_get_tiles_length(move);
  int row_start = move_get_row_start(move);
  int col_start = move_get_col_start(move);
  int move_dir = move_get_dir(move);

  bool board_was_transposed = false;
  if (!board_matches_dir(board, move_dir)) {
    board_transpose(board);
    board_was_transposed = true;
    int tmp_start = row_start;
    row_start = col_start;
    col_start = tmp_start;
  }

  int ls;
  int main_word_score = 0;
  int cross_scores = 0;
  int bingo_bonus = 0;
  int word_multiplier = 1;

  if (tiles_played == RACK_SIZE) {
    bingo_bonus = DEFAULT_BINGO_BONUS;
  }

  for (int idx = 0; idx < tiles_length; idx++) {
    uint8_t ml = move_get_tile(move, idx);
    uint8_t bonus_square =
        board_get_bonus_square(board, row_start, col_start + idx);
    int letter_multiplier = 1;
    int this_word_multiplier = 1;
    bool fresh_tile = false;
    if (ml == PLAYED_THROUGH_MARKER) {
      ml = board_get_letter(board, row_start, col_start + idx);
    } else {
      fresh_tile = true;
      this_word_multiplier = bonus_square >> 4;
      letter_multiplier = bonus_square & 0x0F;
      word_multiplier *= this_word_multiplier;
    }

    // Always use the vertical direction to get the cross score since
    // scoring is done row-wise and the direction switches with
    // transposition. The conditional transposition at the
    // start of this function ensures that the indexing
    // board_get_cross_score function is correct.
    int cs = board_get_cross_score(board, row_start, col_start + idx,
                                   BOARD_HORIZONTAL_DIRECTION, cross_set_index);
    if (get_is_blanked(ml)) {
      ls = 0;
    } else {
      ls = ld_get_score(ld, ml);
    }

    main_word_score += ls * letter_multiplier;
    bool actual_cross_word =
        (row_start > 0 &&
         !board_is_empty(board, row_start - 1, col_start + idx)) ||
        ((row_start < BOARD_DIM - 1) &&
         !board_is_empty(board, row_start + 1, col_start + idx));
    if (fresh_tile && actual_cross_word) {
      cross_scores += ls * letter_multiplier * this_word_multiplier +
                      cs * this_word_multiplier;
    }
  }

  if (board_was_transposed) {
    board_transpose(board);
  }

  return main_word_score * word_multiplier + cross_scores + bingo_bonus;
}
