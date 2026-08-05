#ifndef POSITIONAL_EVAL_H
#define POSITIONAL_EVAL_H

#include "../def/board_defs.h"
#include "../def/equity_defs.h"
#include "../def/game_history_defs.h"
#include "bag.h"
#include "board.h"
#include "equity.h"
#include "game.h"
#include "letter_distribution.h"
#include "move.h"
#include "player.h"
#include <stdint.h>

enum {
  POSITIONAL_FEATURE_EXPOSURE = 0,
  POSITIONAL_FEATURE_EXPOSURE_SCORE,
  POSITIONAL_FEATURE_PREMIUM_ACCESS,
  POSITIONAL_FEATURE_PREMIUM_ACCESS_SCORE,
  POSITIONAL_FEATURE_WORD_END,
  POSITIONAL_FEATURE_COUNT,
  POSITIONAL_MIN_BAG_TILES = 29,
};

// Lexicon-exact features extracted from the post-play board. A directional
// hook is an empty square whose cross set is nontrivial: placing a tile there
// must form a valid perpendicular word. "Held" means at least one tile type
// in the mover's leave intersects that cross set. The per-letter tail lets
// training distinguish, for example, holding a unique S hook from holding a
// broad vowel hook without putting any English-specific logic in the
// extractor. "Live" means at least one legal tile remains in the public
// unseen pool (bag plus opponent rack); a non-live hook cannot be used by the
// opponent and therefore carries no defensive risk.
enum {
  POSITIONAL_HOOK_FEATURE_TOTAL = 0,
  POSITIONAL_HOOK_FEATURE_HELD,
  POSITIONAL_HOOK_FEATURE_HELD_OPTIONS,
  POSITIONAL_HOOK_FEATURE_UNIQUE_HELD,
  POSITIONAL_HOOK_FEATURE_PREMIUM_HELD,
  POSITIONAL_HOOK_FEATURE_SPECIFICITY_HELD,
  POSITIONAL_HOOK_FEATURE_LIVE,
  POSITIONAL_HOOK_FEATURE_LIVE_OPTIONS,
  POSITIONAL_HOOK_FEATURE_LIVE_COPIES,
  POSITIONAL_HOOK_FEATURE_PREMIUM_LIVE,
  POSITIONAL_HOOK_FEATURE_SPECIFICITY_LIVE,
  POSITIONAL_HOOK_FEATURE_DEAD,
  POSITIONAL_HOOK_FEATURE_HELD_SAFE,
  POSITIONAL_HOOK_FEATURE_HELD_CONTESTED,
  POSITIONAL_HOOK_FEATURE_OPPONENT_ONLY,
  POSITIONAL_HOOK_AGGREGATE_FEATURE_COUNT,
  POSITIONAL_HOOK_FEATURE_COUNT =
      POSITIONAL_HOOK_AGGREGATE_FEATURE_COUNT + MAX_ALPHABET_SIZE,
};

static inline int positional_hook_letter_feature(MachineLetter letter) {
  return POSITIONAL_HOOK_AGGREGATE_FEATURE_COUNT + letter;
}

// Quantized point coefficients learned from 2,000 positions, with eight
// candidates evaluated under 128 shared four-ply static rollout scenarios.
// These were refit jointly with the cross-set features below.
//
// Axes are bag phase (61+, 41..60, 29..40), score state (ahead by at least
// 25, within 24, behind by at least 25), then the five integer features above.
// Values are millionths of a point per feature unit. The finer scale matters
// because premium-access-score features can be large enough for rounding each
// coefficient directly to Equity resolution to shift a move by several
// tenths. Positions below 29 bag tiles were not represented in training and
// deliberately receive zero.
static const int32_t positional_weights[3][3][POSITIONAL_FEATURE_COUNT] = {
    {
        {-236809, 145423, -26472, 3054, 45569},
        {-132287, 138366, -26539, 504, 4687},
        {-82658, 149246, -23347, 1718, 238943},
    },
    {
        {-153857, 55863, -18828, 1162, 99428},
        {-49335, 48806, -18895, -1389, 58547},
        {295, 59686, -15703, -175, 292803},
    },
    {
        {-248769, 66151, -20985, -324, -130981},
        {-144247, 59094, -21052, -2874, -171863},
        {-94617, 69974, -17860, -1660, 62393},
    },
};

// The total/dead and three held/live partition features are retained in the
// extractor for diagnostics but have zero coefficients. They are exact
// linear combinations of the held/live features and made rare exhausted-hook
// coefficients unstable. An exhausted hook contributes no live-risk terms;
// a hook held only by the mover contributes held-opportunity terms only.
static const int32_t
    positional_hook_weights[3][3][POSITIONAL_HOOK_AGGREGATE_FEATURE_COUNT] = {
        {
            {0, -38707, -4665, 129524, -9450, 351, 18539, -8110, -3725,
             -31527, 3671, 0, 0, 0, 0},
            {0, -45031, 2400, 90608, -24085, 563, 45457, -2249, -5299,
             -69668, 3263, 0, 0, 0, 0},
            {0, -5205, -1787, 91228, -20883, 788, 31323, -6610, -3852,
             -131203, 2901, 0, 0, 0, 0},
        },
        {
            {0, -21346, -14942, -31849, 22699, -204, 30924, -9912, -3765,
             -35167, 4376, 0, 0, 0, 0},
            {0, -27670, -7876, -70765, 8064, 8, 57841, -4051, -5338, -73308,
             3968, 0, 0, 0, 0},
            {0, 12157, -12063, -70145, 11265, 233, 43708, -8412, -3891,
             -134843, 3606, 0, 0, 0, 0},
        },
        {
            {0, -23806, -22473, -75479, 10417, -293, 40348, -11802, -2257,
             -31065, 4661, 0, 0, 0, 0},
            {0, -30130, -15408, -114396, -4218, -81, 67266, -5941, -3830,
             -69205, 4253, 0, 0, 0, 0},
            {0, 9697, -19594, -113776, -1017, 144, 53133, -10302, -2383,
             -130740, 3891, 0, 0, 0, 0},
        },
};

static inline int positional_eval_get_bag_phase(const Game *game) {
  const int bag_tiles = bag_get_letters(game_get_bag(game));
  return bag_tiles >= 61 ? 0 : bag_tiles >= 41 ? 1 : 2;
}

static inline int positional_eval_get_score_state(const Game *game) {
  const int player_index = game_get_player_on_turn_index(game);
  const Equity score_difference =
      player_get_score(game_get_player(game, player_index)) -
      player_get_score(game_get_player(game, 1 - player_index));
  return score_difference >= int_to_equity(25)    ? 0
         : score_difference <= int_to_equity(-25) ? 2
                                                  : 1;
}

static inline Equity
positional_eval_micropoints_to_equity(int64_t adjustment_micropoints) {
  const int64_t adjustment = adjustment_micropoints >= 0
                                 ? (adjustment_micropoints + 500) / 1000
                                 : (adjustment_micropoints - 500) / 1000;
  if (adjustment > EQUITY_MAX_VALUE) {
    return EQUITY_MAX_VALUE;
  }
  if (adjustment < EQUITY_MIN_VALUE) {
    return EQUITY_MIN_VALUE;
  }
  return (Equity)adjustment;
}

static inline int positional_premium_weight(BonusSquare bonus) {
  const int word_multiplier = bonus_square_get_word_multiplier(bonus);
  if (word_multiplier > 1) {
    return 3 * (word_multiplier - 1);
  }
  const int letter_multiplier = bonus_square_get_letter_multiplier(bonus);
  if (letter_multiplier >= 3) {
    return 3 * (letter_multiplier - 2);
  }
  return letter_multiplier == 2;
}

static inline bool positional_square_occupied_after_move(const Board *board,
                                                         const Move *move,
                                                         int row, int col) {
  if (board_is_nonempty_or_bricked(board, row, col)) {
    return true;
  }
  const int row_start = move_get_row_start(move);
  const int col_start = move_get_col_start(move);
  const int length = move_get_tiles_length(move);
  if (move_get_dir(move) == BOARD_HORIZONTAL_DIRECTION) {
    return row == row_start && col >= col_start && col < col_start + length;
  }
  return col == col_start && row >= row_start && row < row_start + length;
}

static inline Equity positional_eval_get_adjustment(const Game *game,
                                                    const Move *move) {
  if (move_get_type(move) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
    return 0;
  }
  const int bag_tiles = bag_get_letters(game_get_bag(game));
  if (bag_tiles < POSITIONAL_MIN_BAG_TILES) {
    return 0;
  }

  const Board *board = game_get_board(game);
  const LetterDistribution *ld = game_get_ld(game);
  const int row_start = move_get_row_start(move);
  const int col_start = move_get_col_start(move);
  const int dr = move_get_dir(move) == BOARD_VERTICAL_DIRECTION ? 1 : 0;
  const int dc = move_get_dir(move) == BOARD_HORIZONTAL_DIRECTION ? 1 : 0;
  int features[POSITIONAL_FEATURE_COUNT] = {0};
  int first_new_offset = -1;
  int last_new_offset = -1;

  for (int offset = 0; offset < move_get_tiles_length(move); offset++) {
    MachineLetter tile = move_get_tile(move, offset);
    if (tile == PLAYED_THROUGH_MARKER) {
      continue;
    }
    if (first_new_offset < 0) {
      first_new_offset = offset;
    }
    last_new_offset = offset;
    const int row = row_start + offset * dr;
    const int col = col_start + offset * dc;
    const int tile_score = equity_to_int(
        get_is_blanked(tile) ? ld_get_score(ld, BLANK_MACHINE_LETTER)
                             : ld_get_score(ld, tile));
    const int perp_dr = dc;
    const int perp_dc = dr;
    int open_sides = 0;
    for (int side = -1; side <= 1; side += 2) {
      const int near_row = row + side * perp_dr;
      const int near_col = col + side * perp_dc;
      if (near_row >= 0 && near_row < BOARD_DIM && near_col >= 0 &&
          near_col < BOARD_DIM &&
          !positional_square_occupied_after_move(board, move, near_row,
                                                 near_col)) {
        open_sides++;
      }
    }
    features[POSITIONAL_FEATURE_EXPOSURE] += open_sides;
    features[POSITIONAL_FEATURE_EXPOSURE_SCORE] += open_sides * tile_score;

    static const int sight_directions[4][2] = {
        {1, 0},
        {-1, 0},
        {0, 1},
        {0, -1},
    };
    for (int direction = 0; direction < 4; direction++) {
      for (int distance = 1; distance <= RACK_SIZE; distance++) {
        const int sight_row = row + distance * sight_directions[direction][0];
        const int sight_col = col + distance * sight_directions[direction][1];
        if (sight_row < 0 || sight_row >= BOARD_DIM || sight_col < 0 ||
            sight_col >= BOARD_DIM ||
            positional_square_occupied_after_move(board, move, sight_row,
                                                  sight_col)) {
          break;
        }
        const int premium_weight =
            positional_premium_weight(
                board_get_bonus_square(board, sight_row, sight_col)) *
            (RACK_SIZE + 1 - distance);
        features[POSITIONAL_FEATURE_PREMIUM_ACCESS] += premium_weight;
        features[POSITIONAL_FEATURE_PREMIUM_ACCESS_SCORE] +=
            premium_weight * tile_score;
      }
    }
  }

  const int endpoint_offsets[2] = {first_new_offset, last_new_offset};
  const int endpoint_signs[2] = {-1, 1};
  for (int endpoint = 0; endpoint < 2; endpoint++) {
    if (endpoint_offsets[endpoint] < 0) {
      continue;
    }
    const int row =
        row_start +
        (endpoint_offsets[endpoint] + endpoint_signs[endpoint]) * dr;
    const int col =
        col_start +
        (endpoint_offsets[endpoint] + endpoint_signs[endpoint]) * dc;
    if (row >= 0 && row < BOARD_DIM && col >= 0 && col < BOARD_DIM &&
        !positional_square_occupied_after_move(board, move, row, col)) {
      features[POSITIONAL_FEATURE_WORD_END] +=
          positional_premium_weight(board_get_bonus_square(board, row, col));
    }
  }

  const int bag_phase = positional_eval_get_bag_phase(game);
  const int score_state = positional_eval_get_score_state(game);
  int64_t adjustment_micropoints = 0;
  for (int feature = 0; feature < POSITIONAL_FEATURE_COUNT; feature++) {
    adjustment_micropoints +=
        (int64_t)positional_weights[bag_phase][score_state][feature] *
        features[feature];
  }
  return positional_eval_micropoints_to_equity(adjustment_micropoints);
}

#endif
