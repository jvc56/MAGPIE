#include "play_feature_board.h"

#include "../def/board_defs.h"
#include "../def/game_history_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/rack_defs.h"
#include "../ent/board.h"
#include "../ent/bonus_square.h"
#include "../ent/game.h"
#include "../ent/move.h"
#include "../ent/play_feature.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

enum {
  // Premium word squares are counted in a play's reach out to this many
  // squares from a placed tile.
  PLAY_FEATURE_PREMIUM_REACH = RACK_SIZE,
};

// Writes the rows and columns of the tiles the play places, and returns how
// many there are: 0 for an exchange or a pass.
static int play_feature_placed_squares(const Move *move, int *rows, int *cols) {
  if (move_get_type(move) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
    return 0;
  }
  const bool vertical = board_is_dir_vertical(move_get_dir(move));
  const int row_start = move_get_row_start(move);
  const int col_start = move_get_col_start(move);
  const int tiles_length = move_get_tiles_length(move);
  int num_placed = 0;
  for (int tile_idx = 0; tile_idx < tiles_length; tile_idx++) {
    if (move_get_tile(move, tile_idx) == PLAYED_THROUGH_MARKER) {
      continue;
    }
    rows[num_placed] = vertical ? row_start + tile_idx : row_start;
    cols[num_placed] = vertical ? col_start : col_start + tile_idx;
    num_placed++;
  }
  return num_placed;
}

void play_feature_extract_location(const PlayFeatureCtx
                                       __attribute__((unused)) *
                                       ctx,
                                   const Move *move, double *out) {
  int rows[BOARD_DIM];
  int cols[BOARD_DIM];
  const int num_placed = play_feature_placed_squares(move, rows, cols);
  if (num_placed == 0) {
    out[0] = (double)(BOARD_DIM - 1) / 2.0;
    out[1] = (double)(BOARD_DIM - 1) / 2.0;
    return;
  }
  double row_sum = 0.0;
  double col_sum = 0.0;
  for (int placed_idx = 0; placed_idx < num_placed; placed_idx++) {
    row_sum += rows[placed_idx];
    col_sum += cols[placed_idx];
  }
  out[0] = row_sum / num_placed;
  out[1] = col_sum / num_placed;
}

void play_feature_extract_direction(const PlayFeatureCtx
                                        __attribute__((unused)) *
                                        ctx,
                                    const Move *move, double *out) {
  out[0] = 0.0;
  out[1] = 0.0;
  if (move_get_type(move) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
    return;
  }
  if (board_is_dir_vertical(move_get_dir(move))) {
    out[1] = 1.0;
  } else {
    out[0] = 1.0;
  }
}

void play_feature_prepare_premium_access(PlayFeatureCtx *ctx) {
  const Board *board = game_get_board(ctx->game);
  ctx->num_premium_squares = 0;
  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      if (!board_is_empty(board, row, col)) {
        continue;
      }
      const BonusSquare bonus_square = board_get_bonus_square(board, row, col);
      if (bonus_square_is_brick(bonus_square)) {
        continue;
      }
      const int word_multiplier =
          bonus_square_get_word_multiplier(bonus_square);
      if (word_multiplier < 2) {
        continue;
      }
      ctx->premium_rows[ctx->num_premium_squares] = row;
      ctx->premium_cols[ctx->num_premium_squares] = col;
      ctx->premium_word_multipliers[ctx->num_premium_squares] = word_multiplier;
      ctx->num_premium_squares++;
    }
  }
}

void play_feature_extract_premium_access(const PlayFeatureCtx *ctx,
                                         const Move *move, double *out) {
  int rows[BOARD_DIM];
  int cols[BOARD_DIM];
  const int num_placed = play_feature_placed_squares(move, rows, cols);
  int triple_words = 0;
  int double_words = 0;
  for (int premium_idx = 0; premium_idx < ctx->num_premium_squares;
       premium_idx++) {
    const int premium_row = ctx->premium_rows[premium_idx];
    const int premium_col = ctx->premium_cols[premium_idx];
    bool in_reach = false;
    for (int placed_idx = 0; placed_idx < num_placed && !in_reach;
         placed_idx++) {
      if (premium_row == rows[placed_idx]) {
        in_reach =
            abs(premium_col - cols[placed_idx]) <= PLAY_FEATURE_PREMIUM_REACH;
      } else if (premium_col == cols[placed_idx]) {
        in_reach =
            abs(premium_row - rows[placed_idx]) <= PLAY_FEATURE_PREMIUM_REACH;
      }
    }
    if (!in_reach) {
      continue;
    }
    if (ctx->premium_word_multipliers[premium_idx] >= 3) {
      triple_words++;
    } else {
      double_words++;
    }
  }
  out[0] = (double)triple_words;
  out[1] = (double)double_words;
}

void play_feature_extract_perimeter(const PlayFeatureCtx *ctx, const Move *move,
                                    double *out) {
  int rows[BOARD_DIM];
  int cols[BOARD_DIM];
  const int num_placed = play_feature_placed_squares(move, rows, cols);
  const Board *board = game_get_board(ctx->game);
  bool counted[BOARD_DIM][BOARD_DIM];
  memset(counted, 0, sizeof(counted));
  for (int placed_idx = 0; placed_idx < num_placed; placed_idx++) {
    counted[rows[placed_idx]][cols[placed_idx]] = true;
  }
  static const int row_steps[] = {-1, 1, 0, 0};
  static const int col_steps[] = {0, 0, -1, 1};
  int perimeter = 0;
  for (int placed_idx = 0; placed_idx < num_placed; placed_idx++) {
    for (int step_idx = 0; step_idx < 4; step_idx++) {
      const int row = rows[placed_idx] + row_steps[step_idx];
      const int col = cols[placed_idx] + col_steps[step_idx];
      if (row < 0 || row >= BOARD_DIM || col < 0 || col >= BOARD_DIM ||
          counted[row][col] || !board_is_empty(board, row, col) ||
          board_get_is_brick(board, row, col)) {
        continue;
      }
      counted[row][col] = true;
      perimeter++;
    }
  }
  out[0] = (double)perimeter;
}
