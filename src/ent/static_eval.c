#include "../def/letter_distribution_defs.h"
#include "../def/rack_defs.h"
#include "../def/static_eval_defs.h"

#include "../ent/board.h"
#include "../ent/klv.h"
#include "../ent/move.h"
#include "../ent/rack.h"

#include "static_eval.h"

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
  int cross_dir = 1 - move_dir;

  bool board_was_transposed = false;
  if (!board_matches_dir(board, move_dir)) {
    board_transpose(board);
    board_was_transposed = true;
  }

  if (move_dir == BOARD_VERTICAL_DIRECTION) {
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
    int cs = board_get_cross_score(board, row_start, col_start + idx, cross_dir,
                                   cross_set_index);
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
