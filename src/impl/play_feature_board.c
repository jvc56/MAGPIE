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
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
  // Premium word squares are counted in a play's reach out to this many
  // squares from a placed tile.
  PLAY_FEATURE_PREMIUM_REACH = RACK_SIZE,
  // The most premium squares the per-square masks hold.
  PLAY_FEATURE_MAX_MASKED_PREMIUMS = 64,
};

// The number of set bits, following the guarded-intrinsic pattern.
static inline int play_feature_bit_count(uint64_t bits) {
#if defined(__has_builtin) && __has_builtin(__builtin_popcountll)
  return __builtin_popcountll(bits);
#else
  int count = 0;
  while (bits != 0) {
    bits &= bits - 1;
    count++;
  }
  return count;
#endif
}

// Whether the premium square at premium_row, premium_col is in reach of a
// tile placed at row, col.
static bool play_feature_premium_in_reach(const int premium_row,
                                          const int premium_col, const int row,
                                          const int col) {
  if (premium_row == row) {
    return abs(premium_col - col) <= PLAY_FEATURE_PREMIUM_REACH;
  }
  if (premium_col == col) {
    return abs(premium_row - row) <= PLAY_FEATURE_PREMIUM_REACH;
  }
  return false;
}

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
  ctx->premium_masks_ready =
      ctx->num_premium_squares <= PLAY_FEATURE_MAX_MASKED_PREMIUMS;
  if (!ctx->premium_masks_ready) {
    return;
  }
  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      uint64_t triple_mask = 0;
      uint64_t double_mask = 0;
      for (int premium_idx = 0; premium_idx < ctx->num_premium_squares;
           premium_idx++) {
        if (!play_feature_premium_in_reach(ctx->premium_rows[premium_idx],
                                           ctx->premium_cols[premium_idx], row,
                                           col)) {
          continue;
        }
        const uint64_t bit = (uint64_t)1 << premium_idx;
        if (ctx->premium_word_multipliers[premium_idx] >= 3) {
          triple_mask |= bit;
        } else {
          double_mask |= bit;
        }
      }
      ctx->premium_triple_masks[row * BOARD_DIM + col] = triple_mask;
      ctx->premium_double_masks[row * BOARD_DIM + col] = double_mask;
      ctx->premium_square_bits[row * BOARD_DIM + col] = 0;
    }
  }
  for (int premium_idx = 0; premium_idx < ctx->num_premium_squares;
       premium_idx++) {
    ctx->premium_square_bits[ctx->premium_rows[premium_idx] * BOARD_DIM +
                             ctx->premium_cols[premium_idx]] = (uint64_t)1
                                                               << premium_idx;
  }
}

void play_feature_extract_premium_access(const PlayFeatureCtx *ctx,
                                         const Move *move, double *out) {
  int rows[BOARD_DIM];
  int cols[BOARD_DIM];
  const int num_placed = play_feature_placed_squares(move, rows, cols);
  if (ctx->premium_masks_ready) {
    uint64_t triple_mask = 0;
    uint64_t double_mask = 0;
    uint64_t covered = 0;
    for (int placed_idx = 0; placed_idx < num_placed; placed_idx++) {
      const int square = rows[placed_idx] * BOARD_DIM + cols[placed_idx];
      triple_mask |= ctx->premium_triple_masks[square];
      double_mask |= ctx->premium_double_masks[square];
      covered |= ctx->premium_square_bits[square];
    }
    triple_mask &= ~covered;
    double_mask &= ~covered;
    out[0] = (double)play_feature_bit_count(triple_mask);
    out[1] = (double)play_feature_bit_count(double_mask);
    return;
  }
  int triple_words = 0;
  int double_words = 0;
  for (int premium_idx = 0; premium_idx < ctx->num_premium_squares;
       premium_idx++) {
    const int premium_row = ctx->premium_rows[premium_idx];
    const int premium_col = ctx->premium_cols[premium_idx];
    bool in_reach = false;
    bool covered = false;
    for (int placed_idx = 0; placed_idx < num_placed && !covered;
         placed_idx++) {
      in_reach = in_reach || play_feature_premium_in_reach(
                                 premium_row, premium_col, rows[placed_idx],
                                 cols[placed_idx]);
      covered =
          premium_row == rows[placed_idx] && premium_col == cols[placed_idx];
    }
    if (!in_reach || covered) {
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
