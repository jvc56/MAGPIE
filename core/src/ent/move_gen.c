#include <assert.h>
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../def/cross_set_defs.h"
#include "../def/players_data_defs.h"
#include "../def/rack_defs.h"

#include "../util/util.h"

#include "anchor.h"
#include "bag.h"
#include "config.h"
#include "game.h"
#include "klv.h"
#include "kwg.h"
#include "leave_map.h"
#include "move.h"
#include "move_gen.h"
#include "player.h"
#include "rack.h"

#define INITIAL_LAST_ANCHOR_COL (BOARD_DIM)

struct MoveGen {
  // Owned by this MoveGen struct
  int current_row_index;
  int current_anchor_col;
  int last_anchor_col;
  int dir;
  int tiles_played;
  int number_of_plays;
  int move_sort_type;
  int move_record_type;
  int number_of_tiles_in_bag;
  int player_index;
  bool kwgs_are_distinct;
  uint8_t row_letter_cache[(BOARD_DIM)];
  uint8_t strip[(BOARD_DIM)];
  uint8_t *exchange_strip;
  double preendgame_adjustment_values[PREENDGAME_ADJUSTMENT_VALUES_LENGTH];
  LeaveMap *leave_map;
  // Shadow plays
  int current_left_col;
  int current_right_col;
  double highest_shadow_equity;
  uint64_t rack_cross_set;
  int number_of_letters_on_rack;
  int descending_tile_scores[(RACK_SIZE)];
  double best_leaves[(RACK_SIZE)];
  AnchorList *anchor_list;

  // Owned by the caller
  const LetterDistribution *letter_distribution;
  const KLV *klv;
  const KWG *kwg;
  const Rack *opponent_rack;
  // Player rack is modified when generating exchanges
  Rack *player_rack;
  // Board is transposed during move generation
  Board *board;
  // Output owned by caller
  MoveList *move_list;
};

void go_on(MoveGen *gen, int current_col, uint8_t L, uint32_t new_node_index,
           bool accepts, int leftstrip, int rightstrip, bool unique_play);

MoveList *gen_get_move_list(MoveGen *gen) { return gen->move_list; }

AnchorList *gen_get_anchor_list(MoveGen *gen) { return gen->anchor_list; }

double *gen_get_best_leaves(MoveGen *gen) { return gen->best_leaves; }

LeaveMap *gen_get_leave_map(MoveGen *gen) { return gen->leave_map; }

bool gen_get_kwgs_are_distinct(const MoveGen *gen) {
  return gen->kwgs_are_distinct;
}

int gen_get_current_row_index(const MoveGen *gen) {
  return gen->current_row_index;
}

int gen_get_current_anchor_col(const MoveGen *gen) {
  return gen->current_anchor_col;
}

void gen_set_move_sort_type(MoveGen *gen, move_sort_t move_sort_type) {
  gen->move_sort_type = move_sort_type;
}

void gen_set_move_record_type(MoveGen *gen, move_record_t move_record_type) {
  gen->move_record_type = move_record_type;
}

void gen_set_current_anchor_col(MoveGen *gen, int anchor_col) {
  gen->current_anchor_col = anchor_col;
}

void gen_set_last_anchor_col(MoveGen *gen, int anchor_col) {
  gen->last_anchor_col = anchor_col;
}

void gen_set_current_row_index(MoveGen *gen, int row_index) {
  gen->current_row_index = row_index;
}

void gen_set_dir(MoveGen *gen, int dir) { gen->dir = dir; }

int score_on_rack(const LetterDistribution *letter_distribution,
                  const Rack *rack) {
  int sum = 0;
  for (int i = 0; i < get_array_size(rack); i++) {
    sum += get_number_of_letter(rack, i) *
           letter_distribution_get_score(letter_distribution, i);
  }
  return sum;
}

int get_cross_set_index(bool kwgs_are_distinct, int player_index) {
  return kwgs_are_distinct && player_index;
}

double placement_adjustment(const MoveGen *gen, const Move *move) {
  int start = get_col_start(move);
  int end = start + move_get_tiles_played(move);

  int j = start;
  double penalty = 0;
  double v_penalty = OPENING_HOTSPOT_PENALTY;

  while (j < end) {
    int tile = get_tile(move, j - start);
    if (is_blanked(tile)) {
      tile = get_unblanked_machine_letter(tile);
    }
    if (letter_distribution_get_is_vowel(gen->letter_distribution, tile) &&
        (j == 2 || j == 6 || j == 8 || j == 12)) {
      penalty += v_penalty;
    }
    j++;
  }
  return penalty;
}

double shadow_endgame_adjustment(const MoveGen *gen) {
  if (gen->number_of_letters_on_rack > gen->tiles_played) {
    // This play is not going out. We should penalize it by our own score
    // plus some constant.
    // However, we don't yet know the score of our own remaining tiles yet,
    // so we need to assume it is as small as possible for this anchor's
    // highest_possible_equity to be valid.
    int lowest_possible_rack_score = 0;
    for (int i = gen->tiles_played; i < gen->number_of_letters_on_rack; i++) {
      lowest_possible_rack_score += gen->descending_tile_scores[i];
    }
    return ((-(double)lowest_possible_rack_score) *
            NON_OUTPLAY_LEAVE_SCORE_MULTIPLIER_PENALTY) -
           NON_OUTPLAY_CONSTANT_PENALTY;
  }
  return 2 *
         ((double)score_on_rack(gen->letter_distribution, gen->opponent_rack));
}

double endgame_adjustment(const MoveGen *gen, const Rack *rack,
                          const Rack *opp_rack) {
  if (!rack_is_empty(rack)) {
    // This play is not going out. We should penalize it by our own score
    // plus some constant.
    return ((-(double)score_on_rack(gen->letter_distribution, rack)) *
            NON_OUTPLAY_LEAVE_SCORE_MULTIPLIER_PENALTY) -
           NON_OUTPLAY_CONSTANT_PENALTY;
  }
  return 2 * ((double)score_on_rack(gen->letter_distribution, opp_rack));
}

double get_spare_move_equity(const MoveGen *gen) {
  double leave_adjustment = 0;
  double other_adjustments = 0;

  Move *spare_move = get_spare_move(gen->move_list);
  if (get_tiles_played(gen->board) == 0 &&
      get_move_type(spare_move) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
    other_adjustments = placement_adjustment(gen, spare_move);
  }

  if (gen->number_of_tiles_in_bag > 0) {
    leave_adjustment = get_current_value(gen->leave_map);
    int bag_plus_rack_size = gen->number_of_tiles_in_bag -
                             move_get_tiles_played(spare_move) + RACK_SIZE;
    if (bag_plus_rack_size < PREENDGAME_ADJUSTMENT_VALUES_LENGTH) {
      other_adjustments +=
          gen->preendgame_adjustment_values[bag_plus_rack_size];
    }
  } else {
    other_adjustments +=
        endgame_adjustment(gen, gen->player_rack, gen->opponent_rack);
  }

  return ((double)get_score(spare_move)) + leave_adjustment + other_adjustments;
}

// this function assumes the word is always horizontal. If this isn't the case,
// the board needs to be transposed ahead of time.
int score_move(const Board *board,
               const LetterDistribution *letter_distribution, uint8_t word[],
               int word_start_index, int word_end_index, int row, int col,
               int tiles_played, int cross_dir, int cross_set_index) {
  int ls;
  int main_word_score = 0;
  int cross_scores = 0;
  int bingo_bonus = 0;
  if (tiles_played == RACK_SIZE) {
    bingo_bonus = DEFAULT_BINGO_BONUS;
  }
  int word_multiplier = 1;
  for (int idx = 0; idx < word_end_index - word_start_index + 1; idx++) {
    uint8_t ml = word[idx + word_start_index];
    uint8_t bonus_square = get_bonus_square(board, row, col + idx);
    int letter_multiplier = 1;
    int this_word_multiplier = 1;
    bool fresh_tile = false;
    if (ml == PLAYED_THROUGH_MARKER) {
      ml = get_letter(board, row, col + idx);
    } else {
      fresh_tile = true;
      this_word_multiplier = bonus_square >> 4;
      letter_multiplier = bonus_square & 0x0F;
      word_multiplier *= this_word_multiplier;
    }
    int cs = get_cross_score(board, row, col + idx, cross_dir, cross_set_index);
    if (is_blanked(ml)) {
      ls = 0;
    } else {
      ls = letter_distribution_get_score(letter_distribution, ml);
    }

    main_word_score += ls * letter_multiplier;
    bool actual_cross_word =
        (row > 0 && !is_empty(board, row - 1, col + idx)) ||
        ((row < BOARD_DIM - 1) && !is_empty(board, row + 1, col + idx));
    if (fresh_tile && actual_cross_word) {
      cross_scores += ls * letter_multiplier * this_word_multiplier +
                      cs * this_word_multiplier;
    }
  }
  return main_word_score * word_multiplier + cross_scores + bingo_bonus;
}

void record_play(MoveGen *gen, int leftstrip, int rightstrip,
                 game_event_t move_type) {
  int start_row = gen->current_row_index;
  int tiles_played = gen->tiles_played;
  int start_col = leftstrip;
  int row = start_row;
  int col = start_col;

  if (dir_is_vertical(gen->dir)) {
    int temp = row;
    row = col;
    col = temp;
  }

  int score = 0;
  uint8_t *strip = NULL;

  if (move_type == GAME_EVENT_TILE_PLACEMENT_MOVE) {
    score = score_move(
        gen->board, gen->letter_distribution, gen->strip, leftstrip, rightstrip,
        start_row, start_col, tiles_played, !dir_is_vertical(gen->dir),
        get_cross_set_index(gen->kwgs_are_distinct, gen->player_index));
    strip = gen->strip;
  } else if (move_type == GAME_EVENT_EXCHANGE) {
    // ignore the empty exchange case
    if (rightstrip == 0) {
      return;
    }
    tiles_played = rightstrip;
    strip = gen->exchange_strip;
  }

  // Set the move to more easily handle equity calculations
  set_spare_move(gen->move_list, strip, leftstrip, rightstrip, score, row, col,
                 tiles_played, gen->dir, move_type);

  if (gen->move_record_type == MOVE_RECORD_ALL) {
    double equity;
    if (gen->move_sort_type == MOVE_SORT_EQUITY) {
      equity = get_spare_move_equity(gen);
    } else {
      equity = score;
    }
    insert_spare_move(gen->move_list, equity);
  } else {
    insert_spare_move_top_equity(gen->move_list, get_spare_move_equity(gen));
  }
}

void generate_exchange_moves(MoveGen *gen, uint8_t ml, int stripidx,
                             bool add_exchange) {
  uint32_t letter_distribution_size =
      letter_distribution_get_size(gen->letter_distribution);
  while (ml < letter_distribution_size &&
         get_number_of_letter(gen->player_rack, ml) == 0) {
    ml++;
  }
  if (ml == letter_distribution_size) {
    // The recording of an exchange should never require
    // the opponent's rack.

    // Ignore the empty exchange case for full racks
    // to avoid out of bounds errors for the best_leaves array
    int number_of_letters_on_rack = get_number_of_letters(gen->player_rack);
    if (number_of_letters_on_rack < RACK_SIZE) {
      double current_value = klv_get_leave_value(gen->klv, gen->player_rack);
      set_current_value(gen->leave_map, current_value);
      if (current_value > gen->best_leaves[number_of_letters_on_rack]) {
        gen->best_leaves[number_of_letters_on_rack] = current_value;
      }
      if (add_exchange) {
        record_play(gen, 0, stripidx, GAME_EVENT_EXCHANGE);
      }
    }
  } else {
    generate_exchange_moves(gen, ml + 1, stripidx, add_exchange);
    int num_this = get_number_of_letter(gen->player_rack, ml);
    for (int i = 0; i < num_this; i++) {
      gen->exchange_strip[stripidx] = ml;
      stripidx += 1;
      take_letter_and_update_current_index(gen->leave_map, gen->player_rack,
                                           ml);
      generate_exchange_moves(gen, ml + 1, stripidx, add_exchange);
    }
    for (int i = 0; i < num_this; i++) {
      add_letter_and_update_current_index(gen->leave_map, gen->player_rack, ml);
    }
  }
}

void load_row_letter_cache(MoveGen *gen, int row) {
  for (int col = 0; col < BOARD_DIM; col++) {
    gen->row_letter_cache[col] = get_letter(gen->board, row, col);
  }
}

uint8_t get_letter_cache(const MoveGen *gen, int col) {
  return gen->row_letter_cache[col];
}

int is_empty_cache(const MoveGen *gen, int col) {
  return get_letter_cache(gen, col) == ALPHABET_EMPTY_SQUARE_MARKER;
}

bool better_play_has_been_found(const MoveGen *gen,
                                double highest_possible_value) {
  Move *move = move_list_get_move(gen->move_list, 0);
  const double best_value_found = (gen->move_sort_type == MOVE_SORT_EQUITY)
                                      ? get_equity(move)
                                      : get_score(move);
  return highest_possible_value + COMPARE_MOVES_EPSILON <= best_value_found;
}

int allowed(uint64_t cross_set, uint8_t letter) {
  return (cross_set & ((uint64_t)1 << letter)) != 0;
}

void recursive_gen(MoveGen *gen, int col, uint32_t node_index, int leftstrip,
                   int rightstrip, bool unique_play) {
  int cs_dir;
  uint8_t current_letter = get_letter_cache(gen, col);
  if (dir_is_vertical(gen->dir)) {
    cs_dir = BOARD_HORIZONTAL_DIRECTION;
  } else {
    cs_dir = BOARD_VERTICAL_DIRECTION;
  }
  uint64_t cross_set = get_cross_set(
      gen->board, gen->current_row_index, col, cs_dir,
      get_cross_set_index(gen->kwgs_are_distinct, gen->player_index));
  if (current_letter != ALPHABET_EMPTY_SQUARE_MARKER) {
    int raw = get_unblanked_machine_letter(current_letter);
    int next_node_index = 0;
    bool accepts = false;
    for (int i = node_index;; i++) {
      if (kwg_tile(gen->kwg, i) == raw) {
        next_node_index = kwg_arc_index(gen->kwg, i);
        accepts = kwg_accepts(gen->kwg, i);
        break;
      }
      if (kwg_is_end(gen->kwg, i)) {
        break;
      }
    }
    go_on(gen, col, current_letter, next_node_index, accepts, leftstrip,
          rightstrip, unique_play);
  } else if (!rack_is_empty(gen->player_rack)) {
    for (int i = node_index;; i++) {
      int ml = kwg_tile(gen->kwg, i);
      int number_of_ml = get_number_of_letter(gen->player_rack, ml);
      if (ml != 0 &&
          (number_of_ml != 0 ||
           get_number_of_letter(gen->player_rack, BLANK_MACHINE_LETTER) != 0) &&
          allowed(cross_set, ml)) {
        int next_node_index = kwg_arc_index(gen->kwg, i);
        bool accepts = kwg_accepts(gen->kwg, i);
        if (number_of_ml > 0) {
          take_letter_and_update_current_index(gen->leave_map, gen->player_rack,
                                               ml);
          gen->tiles_played++;
          go_on(gen, col, ml, next_node_index, accepts, leftstrip, rightstrip,
                unique_play);
          gen->tiles_played--;
          add_letter_and_update_current_index(gen->leave_map, gen->player_rack,
                                              ml);
        }
        // check blank
        if (get_number_of_letter(gen->player_rack, BLANK_MACHINE_LETTER) > 0) {
          take_letter_and_update_current_index(gen->leave_map, gen->player_rack,
                                               BLANK_MACHINE_LETTER);
          gen->tiles_played++;
          go_on(gen, col, get_blanked_machine_letter(ml), next_node_index,
                accepts, leftstrip, rightstrip, unique_play);
          gen->tiles_played--;
          add_letter_and_update_current_index(gen->leave_map, gen->player_rack,
                                              BLANK_MACHINE_LETTER);
        }
      }
      if (kwg_is_end(gen->kwg, i)) {
        break;
      }
    }
  }
}

void go_on(MoveGen *gen, int current_col, uint8_t L, uint32_t new_node_index,
           bool accepts, int leftstrip, int rightstrip, bool unique_play) {
  if (current_col <= gen->current_anchor_col) {
    if (!is_empty_cache(gen, current_col)) {
      gen->strip[current_col] = PLAYED_THROUGH_MARKER;
    } else {
      gen->strip[current_col] = L;
      if (gen->dir && (get_cross_set(gen->board, gen->current_row_index,
                                     current_col, BOARD_HORIZONTAL_DIRECTION,
                                     get_cross_set_index(gen->kwgs_are_distinct,
                                                         gen->player_index)) ==
                       TRIVIAL_CROSS_SET)) {
        unique_play = true;
      }
    }
    leftstrip = current_col;
    bool no_letter_directly_left =
        (current_col == 0) || is_empty_cache(gen, current_col - 1);

    if (accepts && no_letter_directly_left && gen->tiles_played > 0 &&
        (unique_play || gen->tiles_played > 1)) {
      record_play(gen, leftstrip, rightstrip, GAME_EVENT_TILE_PLACEMENT_MOVE);
    }

    if (new_node_index == 0) {
      return;
    }

    if (current_col > 0 && current_col - 1 != gen->last_anchor_col) {
      recursive_gen(gen, current_col - 1, new_node_index, leftstrip, rightstrip,
                    unique_play);
    }

    uint32_t separation_node_index = kwg_get_next_node_index(
        gen->kwg, new_node_index, SEPARATION_MACHINE_LETTER);
    if (separation_node_index != 0 && no_letter_directly_left &&
        gen->current_anchor_col < BOARD_DIM - 1) {
      recursive_gen(gen, gen->current_anchor_col + 1, separation_node_index,
                    leftstrip, rightstrip, unique_play);
    }
  } else {
    if (!is_empty_cache(gen, current_col)) {
      gen->strip[current_col] = PLAYED_THROUGH_MARKER;
    } else {
      gen->strip[current_col] = L;
      if (dir_is_vertical(gen->dir) &&
          (get_cross_set(gen->board, gen->current_row_index, current_col,
                         BOARD_HORIZONTAL_DIRECTION,
                         get_cross_set_index(gen->kwgs_are_distinct,
                                             gen->player_index)) ==
           TRIVIAL_CROSS_SET)) {
        unique_play = true;
      }
    }
    rightstrip = current_col;
    bool no_letter_directly_right =
        (current_col == BOARD_DIM - 1) || is_empty_cache(gen, current_col + 1);

    if (accepts && no_letter_directly_right && gen->tiles_played > 0 &&
        (unique_play || gen->tiles_played > 1)) {
      record_play(gen, leftstrip, rightstrip, GAME_EVENT_TILE_PLACEMENT_MOVE);
    }

    if (new_node_index != 0 && current_col < BOARD_DIM - 1) {
      recursive_gen(gen, current_col + 1, new_node_index, leftstrip, rightstrip,
                    unique_play);
    }
  }
}

bool shadow_allowed_in_cross_set(const MoveGen *gen, int col,
                                 int cross_set_index) {
  uint64_t cross_set =
      get_cross_set(gen->board, gen->current_row_index, col,
                    !dir_is_vertical(gen->dir), cross_set_index);
  // Allowed if
  // there is a letter on the rack in the cross set or,
  // there is anything in the cross set and the rack has a blank.
  return (cross_set & gen->rack_cross_set) != 0 ||
         ((gen->rack_cross_set & 1) && cross_set);
}

void shadow_record(MoveGen *gen, int left_col, int right_col,
                   int main_played_through_score,
                   int perpendicular_additional_score, int word_multiplier) {
  int sorted_effective_letter_multipliers[(RACK_SIZE)];
  int current_tiles_played = 0;
  for (int current_col = left_col; current_col <= right_col; current_col++) {
    uint8_t current_letter = get_letter_cache(gen, current_col);
    if (current_letter == ALPHABET_EMPTY_SQUARE_MARKER) {
      uint8_t bonus_square =
          get_bonus_square(gen->board, gen->current_row_index, current_col);
      int this_word_multiplier = bonus_square >> 4;
      int letter_multiplier = bonus_square & 0x0F;
      bool is_cross_word =
          (gen->current_row_index > 0 &&
           !is_empty(gen->board, gen->current_row_index - 1, current_col)) ||
          ((gen->current_row_index < BOARD_DIM - 1) &&
           !is_empty(gen->board, gen->current_row_index + 1, current_col));
      int effective_letter_multiplier =
          letter_multiplier *
          ((this_word_multiplier * is_cross_word) + word_multiplier);
      // Insert the effective multiplier.
      int insert_index = current_tiles_played;
      for (; insert_index > 0 &&
             sorted_effective_letter_multipliers[insert_index - 1] <
                 effective_letter_multiplier;
           insert_index--) {
        sorted_effective_letter_multipliers[insert_index] =
            sorted_effective_letter_multipliers[insert_index - 1];
      }
      sorted_effective_letter_multipliers[insert_index] =
          effective_letter_multiplier;
      current_tiles_played++;
    }
  }

  int tiles_played_score = 0;
  for (int i = 0; i < current_tiles_played; i++) {
    tiles_played_score +=
        gen->descending_tile_scores[i] * sorted_effective_letter_multipliers[i];
  }

  int bingo_bonus = 0;
  if (gen->tiles_played == RACK_SIZE) {
    bingo_bonus = DEFAULT_BINGO_BONUS;
  }

  int score = tiles_played_score +
              (main_played_through_score * word_multiplier) +
              perpendicular_additional_score + bingo_bonus;
  double equity = (double)score;
  if (gen->move_sort_type == MOVE_SORT_EQUITY) {
    if (gen->number_of_tiles_in_bag > 0) {
      // Bag is not empty: use leave values
      equity +=
          gen->best_leaves[gen->number_of_letters_on_rack - gen->tiles_played];
      // Apply preendgame heuristic if this play would empty the bag or leave
      // few enough tiles remaining.
      int bag_plus_rack_size =
          gen->number_of_tiles_in_bag - gen->tiles_played + RACK_SIZE;
      if (bag_plus_rack_size < PREENDGAME_ADJUSTMENT_VALUES_LENGTH) {
        equity += gen->preendgame_adjustment_values[bag_plus_rack_size];
      }
    } else {
      // Bag is empty: add double opponent's rack if playing out, otherwise
      // deduct a penalty based on the score of our tiles left after this play.
      equity += shadow_endgame_adjustment(gen);
    }
  }
  if (equity > gen->highest_shadow_equity) {
    gen->highest_shadow_equity = equity;
  }
}

void shadow_play_right(MoveGen *gen, int main_played_through_score,
                       int perpendicular_additional_score, int word_multiplier,
                       bool is_unique, int cross_set_index) {
  if (gen->current_right_col == (BOARD_DIM - 1) ||
      gen->tiles_played >= gen->number_of_letters_on_rack) {
    // We have gone all the way left or right.
    return;
  }

  int original_current_right_col = gen->current_right_col;
  gen->current_right_col++;
  gen->tiles_played++;

  uint64_t cross_set =
      get_cross_set(gen->board, gen->current_row_index, gen->current_right_col,
                    !dir_is_vertical(gen->dir), cross_set_index);
  // Allowed if
  // there is a letter on the rack in the cross set or,
  // there is anything in the cross set and the rack has a blank.
  if ((cross_set & gen->rack_cross_set) != 0 ||
      ((gen->rack_cross_set & 1) && cross_set)) {
    // Play tile and update scoring parameters

    uint8_t bonus_square = get_bonus_square(gen->board, gen->current_row_index,
                                            gen->current_right_col);
    int cross_score = get_cross_score(
        gen->board, gen->current_row_index, gen->current_right_col,
        !dir_is_vertical(gen->dir), cross_set_index);
    int this_word_multiplier = bonus_square >> 4;
    perpendicular_additional_score += cross_score * this_word_multiplier;
    word_multiplier *= this_word_multiplier;

    if (cross_set == TRIVIAL_CROSS_SET) {
      // If the horizontal direction is the trivial cross-set, this means
      // that there are no letters perpendicular to where we just placed
      // this letter. So any play we generate here should be unique.
      // We use this to avoid generating duplicates of single-tile plays.
      is_unique = true;
    }
    // Continue playing right until an empty square or the edge of board is hit
    while (gen->current_right_col + 1 < BOARD_DIM) {
      uint8_t next_letter = get_letter_cache(gen, gen->current_right_col + 1);
      if (next_letter == ALPHABET_EMPTY_SQUARE_MARKER) {
        break;
      }
      if (!is_blanked(next_letter)) {
        main_played_through_score += letter_distribution_get_score(
            gen->letter_distribution, next_letter);
      }
      gen->current_right_col++;
    }

    if (gen->tiles_played + is_unique >= 2) {
      shadow_record(gen, gen->current_left_col, gen->current_right_col,
                    main_played_through_score, perpendicular_additional_score,
                    word_multiplier);
    }

    shadow_play_right(gen, main_played_through_score,
                      perpendicular_additional_score, word_multiplier,
                      is_unique, cross_set_index);
  }
  gen->tiles_played--;
  gen->current_right_col = original_current_right_col;
}

void shadow_play_left(MoveGen *gen, int main_played_through_score,
                      int perpendicular_additional_score, int word_multiplier,
                      bool is_unique, int cross_set_index) {
  // Go left until hitting an empty square or the edge of the board.

  if (gen->current_left_col == 0 ||
      gen->current_left_col == gen->last_anchor_col + 1 ||
      gen->tiles_played >= gen->number_of_letters_on_rack) {
    // We have gone all the way left or right.
    return;
  }

  int original_current_left_col = gen->current_left_col;
  gen->current_left_col--;
  gen->tiles_played++;
  uint64_t cross_set =
      get_cross_set(gen->board, gen->current_row_index, gen->current_left_col,
                    !dir_is_vertical(gen->dir), cross_set_index);
  // Allowed if
  // there is a letter on the rack in the cross set or,
  // there is anything in the cross set and the rack has a blank.
  if ((cross_set & gen->rack_cross_set) != 0 ||
      ((gen->rack_cross_set & 1) && cross_set)) {
    // Play tile and update scoring parameters

    uint8_t bonus_square = get_bonus_square(gen->board, gen->current_row_index,
                                            gen->current_left_col);
    int cross_score = get_cross_score(
        gen->board, gen->current_row_index, gen->current_left_col,
        !dir_is_vertical(gen->dir), cross_set_index);
    int this_word_multiplier = bonus_square >> 4;
    perpendicular_additional_score += cross_score * this_word_multiplier;
    word_multiplier *= this_word_multiplier;

    if (cross_set == TRIVIAL_CROSS_SET) {
      // See equivalent in shadow_play_right for the reasoning here.
      is_unique = true;
    }

    if (gen->tiles_played + is_unique >= 2) {
      shadow_record(gen, gen->current_left_col, gen->current_right_col,
                    main_played_through_score, perpendicular_additional_score,
                    word_multiplier);
    }
    shadow_play_left(gen, main_played_through_score,
                     perpendicular_additional_score, word_multiplier, is_unique,
                     cross_set_index);
  }
  shadow_play_right(gen, main_played_through_score,
                    perpendicular_additional_score, word_multiplier, is_unique,
                    cross_set_index);
  gen->current_left_col = original_current_left_col;
  gen->tiles_played--;
}

void shadow_start(MoveGen *gen, int cross_set_index) {
  int main_played_through_score = 0;
  int perpendicular_additional_score = 0;
  int word_multiplier = 1;
  uint8_t current_letter = get_letter_cache(gen, gen->current_left_col);

  if (current_letter == ALPHABET_EMPTY_SQUARE_MARKER) {
    // Only play a letter if a letter from the rack fits in the cross set
    if (shadow_allowed_in_cross_set(gen, gen->current_left_col,
                                    cross_set_index)) {
      // Play tile and update scoring parameters

      uint8_t bonus_square = get_bonus_square(
          gen->board, gen->current_row_index, gen->current_left_col);
      int cross_score = get_cross_score(
          gen->board, gen->current_row_index, gen->current_left_col,
          !dir_is_vertical(gen->dir), cross_set_index);
      int this_word_multiplier = bonus_square >> 4;
      perpendicular_additional_score += (cross_score * this_word_multiplier);
      word_multiplier = this_word_multiplier;
      gen->tiles_played++;
      if (!dir_is_vertical(gen->dir)) {
        // word_multiplier is always hard-coded as 0 since we are recording a
        // single tile
        shadow_record(gen, gen->current_left_col, gen->current_right_col,
                      main_played_through_score, perpendicular_additional_score,
                      0);
      }
    } else {
      // Nothing hooks here, return
      return;
    }
  } else {
    // Traverse the full length of the tiles on the board until hitting an empty
    // square
    while (1) {
      if (!is_blanked(current_letter)) {
        main_played_through_score += letter_distribution_get_score(
            gen->letter_distribution, current_letter);
      }
      if (gen->current_left_col == 0 ||
          gen->current_left_col == gen->last_anchor_col + 1) {
        break;
      }
      gen->current_left_col--;
      current_letter = get_letter_cache(gen, gen->current_left_col);
      if (current_letter == ALPHABET_EMPTY_SQUARE_MARKER) {
        gen->current_left_col++;
        break;
      }
    }
  }
  shadow_play_left(gen, main_played_through_score,
                   perpendicular_additional_score, word_multiplier,
                   !dir_is_vertical(gen->dir), cross_set_index);
  shadow_play_right(gen, main_played_through_score,
                    perpendicular_additional_score, word_multiplier,
                    !dir_is_vertical(gen->dir), cross_set_index);
}

void shadow_play_for_anchor(MoveGen *gen, int col) {
  // set cols
  gen->current_left_col = col;
  gen->current_right_col = col;

  // Reset shadow score
  gen->highest_shadow_equity = 0;

  // Set the number of letters
  gen->number_of_letters_on_rack = get_number_of_letters(gen->player_rack);

  // Set the current anchor column
  gen->current_anchor_col = col;

  // Reset tiles played
  gen->tiles_played = 0;

  // Set rack cross set
  gen->rack_cross_set = 0;
  for (int i = 0; i < letter_distribution_get_size(gen->letter_distribution);
       i++) {
    if (get_number_of_letter(gen->player_rack, i) > 0) {
      gen->rack_cross_set = gen->rack_cross_set | ((uint64_t)1 << i);
    }
  }

  shadow_start(gen,
               get_cross_set_index(gen->kwgs_are_distinct, gen->player_index));
  add_anchor(gen->anchor_list, gen->current_row_index, col,
             gen->last_anchor_col, get_transpose(gen->board),
             dir_is_vertical(gen->dir), gen->highest_shadow_equity);
}

void shadow_by_orientation(MoveGen *gen, int dir) {
  for (int row = 0; row < BOARD_DIM; row++) {
    gen->current_row_index = row;
    gen->last_anchor_col = INITIAL_LAST_ANCHOR_COL;
    load_row_letter_cache(gen, gen->current_row_index);
    for (int col = 0; col < BOARD_DIM; col++) {
      if (get_anchor(gen->board, row, col, dir)) {
        shadow_play_for_anchor(gen, col);
        gen->last_anchor_col = col;
      }
    }
  }
}

void set_descending_tile_scores(MoveGen *gen) {
  int i = 0;
  for (int j = 0;
       j < (int)letter_distribution_get_size(gen->letter_distribution); j++) {
    int j_score_order =
        letter_distribution_get_score_order(gen->letter_distribution, j);
    for (int k = 0; k < get_number_of_letter(gen->player_rack, j_score_order);
         k++) {
      gen->descending_tile_scores[i] = letter_distribution_get_score(
          gen->letter_distribution, j_score_order);
      i++;
    }
  }
}

void generate_moves(const LetterDistribution *ld, const KWG *kwg,
                    const KLV *klv, const Rack *opponent_rack, MoveGen *gen,
                    MoveList *move_list, Board *board, Rack *player_rack,
                    int player_index, int number_of_tiles_in_bag,
                    move_record_t move_record_type, move_sort_t move_sort_type,
                    bool kwgs_are_distinct) {
  gen->letter_distribution = ld;
  gen->kwg = kwg;
  gen->klv = klv;
  gen->opponent_rack = opponent_rack;
  gen->move_list = move_list;
  gen->board = board;
  gen->player_index = player_index;
  gen->player_rack = player_rack;
  gen->number_of_tiles_in_bag = number_of_tiles_in_bag;
  gen->kwgs_are_distinct = kwgs_are_distinct;
  gen->move_sort_type = move_sort_type;
  gen->move_record_type = move_record_type;

  // Reset the move list
  reset_move_list(gen->move_list);

  init_leave_map(player_rack, gen->leave_map);
  if (get_number_of_letters(player_rack) < RACK_SIZE) {
    set_current_value(gen->leave_map, klv_get_leave_value(klv, player_rack));
  } else {
    set_current_value(gen->leave_map, INITIAL_TOP_MOVE_EQUITY);
  }

  // FIXME: check if this is necessary
  // since they are just overwritten by generate
  // exchanges anyway
  // Reset the best leaves so generate exchanges
  for (int i = 0; i < (RACK_SIZE); i++) {
    gen->best_leaves[i] = (double)(INITIAL_TOP_MOVE_EQUITY);
  }

  // Set the best leaves and maybe add exchanges.
  generate_exchange_moves(gen, 0, 0, gen->number_of_tiles_in_bag >= RACK_SIZE);

  reset_anchor_list(gen->anchor_list);
  set_descending_tile_scores(gen);

  for (int dir = 0; dir < 2; dir++) {
    gen->dir = dir % 2 != 0;
    shadow_by_orientation(gen, dir);
    transpose(gen->board);
  }

  // Reset the reused generator fields
  gen->tiles_played = 0;

  sort_anchor_list(gen->anchor_list);
  const AnchorList *anchor_list = gen->anchor_list;
  const int kwg_root_node_index = kwg_get_root_node_index(gen->kwg);
  for (int i = 0; i < get_number_of_anchors(anchor_list); i++) {
    double anchor_highest_possible_equity =
        get_anchor_highest_possible_equity(anchor_list, i);
    if (gen->move_record_type == MOVE_RECORD_BEST &&
        better_play_has_been_found(gen, anchor_highest_possible_equity)) {
      break;
    }
    gen->current_anchor_col = get_anchor_col(anchor_list, i);
    gen->current_row_index = get_anchor_row(anchor_list, i);
    gen->last_anchor_col = get_anchor_last_anchor_col(anchor_list, i);
    gen->dir = get_anchor_dir(anchor_list, i);
    set_transpose(gen->board, get_anchor_transposed(anchor_list, i));
    load_row_letter_cache(gen, gen->current_row_index);
    recursive_gen(gen, gen->current_anchor_col, kwg_root_node_index,
                  gen->current_anchor_col, gen->current_anchor_col,
                  gen->dir == BOARD_HORIZONTAL_DIRECTION);

    if (gen->move_record_type == MOVE_RECORD_BEST) {
      // If a better play has been found than should have been possible for this
      // anchor, highest_possible_equity was invalid.
      assert(!better_play_has_been_found(gen, anchor_highest_possible_equity));
    }
  }

  // FIXME: this should set the to original transposition
  reset_transpose(gen->board);

  Move *top_move = move_list_get_move(gen->move_list, 0);
  // Add the pass move
  if (gen->move_record_type == MOVE_RECORD_ALL ||
      get_equity(top_move) < PASS_MOVE_EQUITY) {
    set_spare_move_as_pass(gen->move_list);
    insert_spare_move(gen->move_list, PASS_MOVE_EQUITY);
  }
}

void load_quackle_preendgame_adjustment_values(MoveGen *gen) {
  double values[] = {0, -8, 0, -0.5, -2, -3.5, -2, 2, 10, 7, 4, -1, -2};
  for (int i = 0; i < PREENDGAME_ADJUSTMENT_VALUES_LENGTH; i++) {
    gen->preendgame_adjustment_values[i] = values[i];
  }
}

void load_zero_preendgame_adjustment_values(MoveGen *gen) {
  for (int i = 0; i < PREENDGAME_ADJUSTMENT_VALUES_LENGTH; i++) {
    gen->preendgame_adjustment_values[i] = 0;
  }
}

void update_generator(const Config *config, MoveGen *gen) {
  update_leave_map(gen->leave_map, letter_distribution_get_size(
                                       config_get_letter_distribution(config)));
}

MoveGen *generate_duplicate(const MoveGen *gen) {
  MoveGen *new_generator = malloc_or_die(sizeof(MoveGen));
  uint32_t letter_distribution_size =
      letter_distribution_get_size(gen->letter_distribution);
  // Allocations
  new_generator->anchor_list = create_anchor_list();
  new_generator->leave_map = create_leave_map(letter_distribution_size);
  new_generator->exchange_strip =
      (uint8_t *)malloc_or_die(letter_distribution_size * sizeof(uint8_t));

  // Just load the zero values for now
  load_zero_preendgame_adjustment_values(new_generator);

  return new_generator;
}

MoveGen *create_generator(int letter_distribution_size) {
  MoveGen *generator = malloc_or_die(sizeof(MoveGen));
  generator->anchor_list = create_anchor_list();
  generator->leave_map = create_leave_map(letter_distribution_size);
  generator->tiles_played = 0;
  generator->dir = BOARD_HORIZONTAL_DIRECTION;
  generator->exchange_strip =
      (uint8_t *)malloc_or_die(letter_distribution_size * sizeof(uint8_t));
  // Just load the zero values for now
  load_zero_preendgame_adjustment_values(generator);

  return generator;
}

void destroy_generator(MoveGen *gen) {
  destroy_anchor_list(gen->anchor_list);
  destroy_leave_map(gen->leave_map);
  free(gen->exchange_strip);
  free(gen);
}