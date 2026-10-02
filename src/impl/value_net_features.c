#include "value_net_features.h"

#include "../def/board_defs.h"
#include "../def/game_history_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/rack_defs.h"
#include "../def/value_net_defs.h"
#include "../ent/bag.h"
#include "../ent/board.h"
#include "../ent/bonus_square.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/klv.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/player.h"
#include "../ent/rack.h"
#include "gameplay.h"
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <string.h>

// Input row layout (see Macondo's game/mlhelper.go BuildMLVector).
enum {
  // Planes.
  VNF_PLANE_LETTERS = 0,
  VNF_PLANE_BLANK = 26,
  // Letters valid with the square's left and right neighbors (Macondo's
  // horizontal cross set), then with its upper and lower neighbors.
  VNF_PLANE_LEFT_RIGHT_CROSS = 27,
  VNF_PLANE_UP_DOWN_CROSS = 53,
  VNF_PLANE_DOUBLE_LETTER = 79,
  VNF_PLANE_TRIPLE_LETTER = 80,
  VNF_PLANE_DOUBLE_WORD = 81,
  VNF_PLANE_TRIPLE_WORD = 82,
  VNF_PLANE_CANDIDATE = 83,
  VNF_PLANE_OPPONENT_LAST = 84,
  VNF_LETTERS = 26,
  // Scalars.
  VNF_SCALAR_LEAVE = 0,
  VNF_SCALAR_UNSEEN = 27,
  VNF_SCALAR_OPPONENT_SCORE = 54,
  VNF_SCALAR_OPPONENT_PLAYED = 55,
  VNF_SCALAR_OPPONENT_EXCHANGED = 56,
  VNF_SCALAR_POWER_TILES = 57,
  VNF_SCALAR_UNSEEN_VOWELS = 63,
  VNF_SCALAR_UNSEEN_CONSONANTS = 64,
  VNF_SCALAR_LEAVE_VOWELS = 65,
  VNF_SCALAR_LEAVE_CONSONANTS = 66,
  VNF_SCALAR_SINCE_BINGO = 67,
  VNF_SCALAR_CANDIDATE_SCORE = 68,
  VNF_SCALAR_LEAVE_VALUE = 69,
  VNF_SCALAR_UNSEEN_COUNT = 70,
  VNF_SCALAR_SPREAD = 71,
  // English machine letters: blank 0, A 1 .. Z 26.
  VNF_ML_A = 1,
  VNF_ML_E = 5,
  VNF_ML_I = 9,
  VNF_ML_J = 10,
  VNF_ML_O = 15,
  VNF_ML_Q = 17,
  VNF_ML_S = 19,
  VNF_ML_U = 21,
  VNF_ML_X = 24,
  VNF_ML_Z = 26,
};

#define VNF_SCORE_CENTER 45.0
#define VNF_SCORE_SCALE 40.0
#define VNF_LEAVE_CENTER 10.0
#define VNF_LEAVE_SCALE 20.0
#define VNF_SPREAD_SCALE 130.0
#define VNF_SINCE_BINGO_SCALE 25.0
#define VNF_UNSEEN_COUNT_SCALE 100.0

static float vnf_tanh_scaled(double x, double center, double scale) {
  return (float)tanh((x - center) / scale);
}

// Marks the squares a placement put tiles on in the given plane.
static void vnf_mark_placement(const Move *move, float *plane) {
  if (move_get_type(move) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
    return;
  }
  const bool vertical = board_is_dir_vertical(move_get_dir(move));
  for (int tile_idx = 0; tile_idx < move_get_tiles_length(move); tile_idx++) {
    if (move_get_tile(move, tile_idx) == PLAYED_THROUGH_MARKER) {
      continue;
    }
    const int row = move_get_row_start(move) + (vertical ? tile_idx : 0);
    const int col = move_get_col_start(move) + (vertical ? 0 : tile_idx);
    plane[(row * VALUE_NET_BOARD_DIM) + col] = 1.0F;
  }
}

// The mover's leave: the rack less the tiles the move uses (for an
// exchange, the exchanged tiles, a blank being machine letter 0).
static void vnf_leave(const Game *game, const Move *move, Rack *leave) {
  if (move_get_type(move) != GAME_EVENT_EXCHANGE) {
    get_leave_for_move(move, game, leave);
    return;
  }
  rack_copy(leave, player_get_rack(game_get_player(
                       game, game_get_player_on_turn_index(game))));
  for (int tile_idx = 0; tile_idx < move_get_tiles_length(move); tile_idx++) {
    rack_take_letter(leave, move_get_tile(move, tile_idx));
  }
}

static void vnf_board_planes(const Board *board, float *row) {
  float *plane_base = row;
  for (int board_row = 0; board_row < VALUE_NET_BOARD_DIM; board_row++) {
    for (int col = 0; col < VALUE_NET_BOARD_DIM; col++) {
      const int square = (board_row * VALUE_NET_BOARD_DIM) + col;
      const MachineLetter ml = board_get_letter(board, board_row, col);
      if (ml != ALPHABET_EMPTY_SQUARE_MARKER) {
        if (get_is_blanked(ml)) {
          plane_base[(VNF_PLANE_BLANK * VALUE_NET_SQUARES) + square] = 1.0F;
        }
        const int letter = get_unblanked_machine_letter(ml) - VNF_ML_A;
        plane_base[((VNF_PLANE_LETTERS + letter) * VALUE_NET_SQUARES) +
                   square] = 1.0F;
      } else {
        const BonusSquare bonus = board_get_bonus_square(board, board_row, col);
        const int word = bonus_square_get_word_multiplier(bonus);
        const int letter = bonus_square_get_letter_multiplier(bonus);
        int plane = -1;
        if (letter == 2) {
          plane = VNF_PLANE_DOUBLE_LETTER;
        } else if (letter == 3) {
          plane = VNF_PLANE_TRIPLE_LETTER;
        } else if (word == 2) {
          plane = VNF_PLANE_DOUBLE_WORD;
        } else if (word == 3) {
          plane = VNF_PLANE_TRIPLE_WORD;
        }
        if (plane >= 0) {
          plane_base[(plane * VALUE_NET_SQUARES) + square] = 1.0F;
        }
      }
      // MAGPIE's vertical-direction cross set holds the letters that fit
      // with the left and right neighbors (Macondo's horizontal one), and
      // its horizontal-direction set those that fit above and below. Both
      // are empty on occupied squares and full with no neighbors.
      const uint64_t left_right = board_get_cross_set(
          board, board_row, col, BOARD_VERTICAL_DIRECTION, 0);
      const uint64_t up_down = board_get_cross_set(
          board, board_row, col, BOARD_HORIZONTAL_DIRECTION, 0);
      for (int letter_idx = 0; letter_idx < VNF_LETTERS; letter_idx++) {
        const int cross_ml = VNF_ML_A + letter_idx;
        if ((left_right >> cross_ml) & 1) {
          plane_base[((VNF_PLANE_LEFT_RIGHT_CROSS + letter_idx) *
                      VALUE_NET_SQUARES) +
                     square] = 1.0F;
        }
        if ((up_down >> cross_ml) & 1) {
          plane_base[((VNF_PLANE_UP_DOWN_CROSS + letter_idx) *
                      VALUE_NET_SQUARES) +
                     square] = 1.0F;
        }
      }
    }
  }
}

static bool vnf_is_vowel(int ml) {
  return ml == VNF_ML_A || ml == VNF_ML_E || ml == VNF_ML_I || ml == VNF_ML_O ||
         ml == VNF_ML_U;
}

void value_net_features_for_move(const Game *game, const Move *move,
                                 const ValueNetHistory *history, Game *scratch,
                                 float *board_row, float *scalars_row) {
  assert(BOARD_DIM == VALUE_NET_BOARD_DIM);
  assert(ld_get_size(game_get_ld(game)) == VALUE_NET_TILE_TYPES);
  memset(board_row, 0, sizeof(float) * VALUE_NET_BOARD_FLOATS);
  memset(scalars_row, 0, sizeof(float) * VALUE_NET_SCALARS);
  const int mover = game_get_player_on_turn_index(game);
  const Player *mover_player = game_get_player(game, mover);
  const Player *opponent = game_get_player(game, 1 - mover);

  // The board after the move, with its cross sets.
  game_copy(scratch, game);
  if (move_get_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
    play_move_on_board(move, scratch);
    update_cross_set_for_move(move, scratch);
  }
  vnf_board_planes(game_get_board(scratch), board_row);
  vnf_mark_placement(
      move, board_row + ((size_t)VNF_PLANE_CANDIDATE * VALUE_NET_SQUARES));
  if (history->has_opponent_last_move) {
    vnf_mark_placement(
        &history->opponent_last_move,
        board_row + ((size_t)VNF_PLANE_OPPONENT_LAST * VALUE_NET_SQUARES));
  }

  Rack leave;
  vnf_leave(game, move, &leave);
  const int leave_size = rack_get_total_letters(&leave);
  // Unseen: the bag plus the opponent's rack. Exchanged tiles are not
  // returned, as when the move has just been made.
  const Bag *bag = game_get_bag(game);
  const Rack *opponent_rack = player_get_rack(opponent);
  int unseen[VALUE_NET_TILE_TYPES];
  int unseen_count = 0;
  for (int ml = 0; ml < VALUE_NET_TILE_TYPES; ml++) {
    unseen[ml] = bag_get_letter(bag, (MachineLetter)ml) +
                 rack_get_letter(opponent_rack, (MachineLetter)ml);
    unseen_count += unseen[ml];
  }
  int unseen_vowels = 0;
  int leave_vowels = 0;
  for (int ml = 0; ml < VALUE_NET_TILE_TYPES; ml++) {
    scalars_row[VNF_SCALAR_LEAVE + ml] =
        (float)rack_get_letter(&leave, (MachineLetter)ml) / (float)RACK_SIZE;
    if (unseen_count > 0) {
      scalars_row[VNF_SCALAR_UNSEEN + ml] =
          (float)unseen[ml] / (float)unseen_count;
    }
    if (vnf_is_vowel(ml)) {
      unseen_vowels += unseen[ml];
      leave_vowels += rack_get_letter(&leave, (MachineLetter)ml);
    }
  }
  if (history->has_opponent_last_move) {
    const Move *last = &history->opponent_last_move;
    scalars_row[VNF_SCALAR_OPPONENT_SCORE] =
        vnf_tanh_scaled(equity_to_double(move_get_score(last)),
                        VNF_SCORE_CENTER, VNF_SCORE_SCALE);
    if (move_get_type(last) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
      scalars_row[VNF_SCALAR_OPPONENT_PLAYED] =
          (float)move_get_tiles_played(last) / (float)RACK_SIZE;
    } else if (move_get_type(last) == GAME_EVENT_EXCHANGE) {
      scalars_row[VNF_SCALAR_OPPONENT_EXCHANGED] =
          (float)move_get_tiles_length(last) / (float)RACK_SIZE;
    }
  }
  scalars_row[VNF_SCALAR_POWER_TILES + 0] = (float)unseen[0] / 2.0F;
  scalars_row[VNF_SCALAR_POWER_TILES + 1] = (float)unseen[VNF_ML_J];
  scalars_row[VNF_SCALAR_POWER_TILES + 2] = (float)unseen[VNF_ML_Q];
  scalars_row[VNF_SCALAR_POWER_TILES + 3] = (float)unseen[VNF_ML_S] / 4.0F;
  scalars_row[VNF_SCALAR_POWER_TILES + 4] = (float)unseen[VNF_ML_X];
  scalars_row[VNF_SCALAR_POWER_TILES + 5] = (float)unseen[VNF_ML_Z];
  if (unseen_count > 0) {
    scalars_row[VNF_SCALAR_UNSEEN_VOWELS] =
        (float)unseen_vowels / (float)unseen_count;
    scalars_row[VNF_SCALAR_UNSEEN_CONSONANTS] =
        (float)(unseen_count - unseen_vowels) / (float)unseen_count;
  }
  if (leave_size > 0) {
    scalars_row[VNF_SCALAR_LEAVE_VOWELS] =
        (float)leave_vowels / (float)leave_size;
    scalars_row[VNF_SCALAR_LEAVE_CONSONANTS] =
        (float)(leave_size - leave_vowels) / (float)leave_size;
  }
  scalars_row[VNF_SCALAR_SINCE_BINGO] =
      (float)history->opponent_moves_since_bingo / (float)VNF_SINCE_BINGO_SCALE;
  const double score = equity_to_double(move_get_score(move));
  scalars_row[VNF_SCALAR_CANDIDATE_SCORE] =
      vnf_tanh_scaled(score, VNF_SCORE_CENTER, VNF_SCORE_SCALE);
  const double leave_value = leave_size > 0
                                 ? equity_to_double(klv_get_leave_value(
                                       player_get_klv(mover_player), &leave))
                                 : 0.0;
  scalars_row[VNF_SCALAR_LEAVE_VALUE] =
      vnf_tanh_scaled(leave_value, VNF_LEAVE_CENTER, VNF_LEAVE_SCALE);
  scalars_row[VNF_SCALAR_UNSEEN_COUNT] =
      (float)unseen_count / (float)VNF_UNSEEN_COUNT_SCALE;
  const double spread = equity_to_double(player_get_score(mover_player)) +
                        score - equity_to_double(player_get_score(opponent));
  scalars_row[VNF_SCALAR_SPREAD] =
      vnf_tanh_scaled(spread, 0.0, VNF_SPREAD_SCALE);
}
