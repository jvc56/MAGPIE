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
#include "klv.h"
#include "kwg.h"
#include "leave_map.h"
#include "move.h"
#include "movegen.h"
#include "player.h"
#include "rack.h"

#define INITIAL_LAST_ANCHOR_COL (BOARD_DIM)

struct Generator {
  int current_row_index;
  int current_anchor_col;
  int last_anchor_col;
  int dir;
  int tiles_played;
  int number_of_plays;
  int move_sort_type;
  int move_record_type;
  bool kwgs_are_distinct;
  bool apply_placement_adjustment;

  uint8_t row_letter_cache[(BOARD_DIM)];
  uint8_t strip[(BOARD_DIM)];
  uint8_t *exchange_strip;
  double preendgame_adjustment_values[PREENDGAME_ADJUSTMENT_VALUES_LENGTH];

  MoveList *move_list;
  Board *board;
  Bag *bag;

  LeaveMap *leave_map;
  LetterDistribution *letter_distribution;

  // Shadow plays
  int current_left_col;
  int current_right_col;
  double highest_shadow_equity;
  uint64_t rack_cross_set;
  int number_of_letters_on_rack;
  int descending_tile_scores[(RACK_SIZE)];
  double best_leaves[(RACK_SIZE)];
  AnchorList *anchor_list;
};

void go_on(const Rack *opp_rack, Generator *gen, int current_col, uint8_t L,
           Player *player, uint32_t new_node_index, bool accepts, int leftstrip,
           int rightstrip, bool unique_play);

Board *gen_get_board(Generator *gen) { return gen->board; }

Bag *gen_get_bag(Generator *gen) { return gen->bag; }

LetterDistribution *gen_get_ld(Generator *gen) {
  return gen->letter_distribution;
}

MoveList *gen_get_move_list(Generator *gen) { return gen->move_list; }

AnchorList *gen_get_anchor_list(Generator *gen) { return gen->anchor_list; }

double *gen_get_best_leaves(Generator *gen) { return gen->best_leaves; }

LeaveMap *gen_get_leave_map(Generator *gen) { return gen->leave_map; }

bool gen_get_kwgs_are_distinct(const Generator *gen) {
  return gen->kwgs_are_distinct;
}

int gen_get_current_row_index(const Generator *gen) {
  return gen->current_row_index;
}

int gen_get_current_anchor_col(const Generator *gen) {
  return gen->current_anchor_col;
}

void gen_set_move_sort_type(Generator *gen, move_sort_t move_sort_type) {
  gen->move_sort_type = move_sort_type;
}

void gen_set_move_record_type(Generator *gen, move_record_t move_record_type) {
  gen->move_record_type = move_record_type;
}

void gen_set_apply_placement_adjustment(Generator *gen,
                                        bool apply_placement_adjustment) {
  gen->apply_placement_adjustment = apply_placement_adjustment;
}

void gen_set_current_anchor_col(Generator *gen, int anchor_col) {
  gen->current_anchor_col = anchor_col;
}

void gen_set_last_anchor_col(Generator *gen, int anchor_col) {
  gen->last_anchor_col = anchor_col;
}

void gen_set_current_row_index(Generator *gen, int row_index) {
  gen->current_row_index = row_index;
}

void gen_set_dir(Generator *gen, int dir) { gen->dir = dir; }

int score_on_rack(const LetterDistribution *letter_distribution,
                  const Rack *rack) {
  int sum = 0;
  for (int i = 0; i < get_array_size(rack); i++) {
    sum += get_number_of_letter(rack, i) *
           letter_distribution_get_score(letter_distribution, i);
  }
  return sum;
}

int get_cross_set_index(const Generator *gen, int player_index) {
  return gen->kwgs_are_distinct && player_index;
}

double placement_adjustment(const Generator *gen, const Move *move) {
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

double shadow_endgame_adjustment(const Generator *gen, const Rack *opp_rack,
                                 int num_tiles_played) {
  if (gen->number_of_letters_on_rack > num_tiles_played) {
    // This play is not going out. We should penalize it by our own score
    // plus some constant.
    // However, we don't yet know the score of our own remaining tiles yet,
    // so we need to assume it is as small as possible for this anchor's
    // highest_possible_equity to be valid.
    int lowest_possible_rack_score = 0;
    for (int i = num_tiles_played; i < gen->number_of_letters_on_rack; i++) {
      lowest_possible_rack_score += gen->descending_tile_scores[i];
    }
    return ((-(double)lowest_possible_rack_score) *
            NON_OUTPLAY_LEAVE_SCORE_MULTIPLIER_PENALTY) -
           NON_OUTPLAY_CONSTANT_PENALTY;
  }
  return 2 * ((double)score_on_rack(gen->letter_distribution, opp_rack));
}

double endgame_adjustment(const Generator *gen, const Rack *rack,
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

double get_spare_move_equity(const Generator *gen, const Player *player,
                             const Rack *opp_rack) {
  double leave_adjustment = 0;
  double other_adjustments = 0;

  Move *spare_move = get_spare_move(gen->move_list);
  if (gen->apply_placement_adjustment && get_tiles_played(gen->board) == 0 &&
      get_move_type(spare_move) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
    other_adjustments = placement_adjustment(gen, spare_move);
  }

  if (!bag_is_empty(gen->bag)) {
    leave_adjustment = get_current_value(gen->leave_map);
    int bag_plus_rack_size = get_tiles_remaining(gen->bag) -
                             move_get_tiles_played(spare_move) + RACK_SIZE;
    if (bag_plus_rack_size < PREENDGAME_ADJUSTMENT_VALUES_LENGTH) {
      other_adjustments +=
          gen->preendgame_adjustment_values[bag_plus_rack_size];
    }
  } else {
    other_adjustments +=
        endgame_adjustment(gen, player_get_rack(player), opp_rack);
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

void record_play(const Player *player, const Rack *opp_rack, Generator *gen,
                 int leftstrip, int rightstrip, game_event_t move_type) {
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
    score = score_move(gen->board, gen->letter_distribution, gen->strip,
                       leftstrip, rightstrip, start_row, start_col,
                       tiles_played, !dir_is_vertical(gen->dir),
                       get_cross_set_index(gen, player_get_index(player)));
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
      equity = get_spare_move_equity(gen, player, opp_rack);
    } else {
      equity = score;
    }
    insert_spare_move(gen->move_list, equity);
  } else {
    insert_spare_move_top_equity(gen->move_list,
                                 get_spare_move_equity(gen, player, opp_rack));
  }
}

void generate_exchange_moves(Generator *gen, Player *player, uint8_t ml,
                             int stripidx, bool add_exchange) {
  uint32_t letter_distribution_size =
      letter_distribution_get_size(gen->letter_distribution);
  Rack *player_rack = player_get_rack(player);
  while (ml < letter_distribution_size &&
         get_number_of_letter(player_rack, ml) == 0) {
    ml++;
  }
  if (ml == letter_distribution_size) {
    // The recording of an exchange should never require
    // the opponent's rack.

    // Ignore the empty exchange case for full racks
    // to avoid out of bounds errors for the best_leaves array
    int number_of_letters_on_rack = get_number_of_letters(player_rack);
    if (number_of_letters_on_rack < RACK_SIZE) {
      double current_value =
          klv_get_leave_value(player_get_klv(player), player_rack);
      set_current_value(gen->leave_map, current_value);
      if (current_value > gen->best_leaves[number_of_letters_on_rack]) {
        gen->best_leaves[number_of_letters_on_rack] = current_value;
      }
      if (add_exchange) {
        record_play(player, NULL, gen, 0, stripidx, GAME_EVENT_EXCHANGE);
      }
    }
  } else {
    generate_exchange_moves(gen, player, ml + 1, stripidx, add_exchange);
    int num_this = get_number_of_letter(player_rack, ml);
    for (int i = 0; i < num_this; i++) {
      gen->exchange_strip[stripidx] = ml;
      stripidx += 1;
      take_letter_and_update_current_index(gen->leave_map, player_rack, ml);
      generate_exchange_moves(gen, player, ml + 1, stripidx, add_exchange);
    }
    for (int i = 0; i < num_this; i++) {
      add_letter_and_update_current_index(gen->leave_map, player_rack, ml);
    }
  }
}

void load_row_letter_cache(Generator *gen, int row) {
  for (int col = 0; col < BOARD_DIM; col++) {
    gen->row_letter_cache[col] = get_letter(gen->board, row, col);
  }
}

uint8_t get_letter_cache(const Generator *gen, int col) {
  return gen->row_letter_cache[col];
}

int is_empty_cache(const Generator *gen, int col) {
  return get_letter_cache(gen, col) == ALPHABET_EMPTY_SQUARE_MARKER;
}

bool better_play_has_been_found(const Generator *gen,
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

void recursive_gen(const Rack *opp_rack, Generator *gen, int col,
                   Player *player, uint32_t node_index, int leftstrip,
                   int rightstrip, bool unique_play) {
  Rack *player_rack = player_get_rack(player);
  const KWG *kwg = player_get_kwg(player);
  int player_index = player_get_index(player);
  int cs_dir;
  uint8_t current_letter = get_letter_cache(gen, col);
  if (dir_is_vertical(gen->dir)) {
    cs_dir = BOARD_HORIZONTAL_DIRECTION;
  } else {
    cs_dir = BOARD_VERTICAL_DIRECTION;
  }
  uint64_t cross_set =
      get_cross_set(gen->board, gen->current_row_index, col, cs_dir,
                    get_cross_set_index(gen, player_index));
  if (current_letter != ALPHABET_EMPTY_SQUARE_MARKER) {
    int raw = get_unblanked_machine_letter(current_letter);
    int next_node_index = 0;
    bool accepts = false;
    for (int i = node_index;; i++) {
      if (kwg_tile(kwg, i) == raw) {
        next_node_index = kwg_arc_index(kwg, i);
        accepts = kwg_accepts(kwg, i);
        break;
      }
      if (kwg_is_end(kwg, i)) {
        break;
      }
    }
    go_on(opp_rack, gen, col, current_letter, player, next_node_index, accepts,
          leftstrip, rightstrip, unique_play);
  } else if (!rack_is_empty(player_rack)) {
    for (int i = node_index;; i++) {
      int ml = kwg_tile(kwg, i);
      int number_of_ml = get_number_of_letter(player_rack, ml);
      if (ml != 0 &&
          (number_of_ml != 0 ||
           get_number_of_letter(player_rack, BLANK_MACHINE_LETTER) != 0) &&
          allowed(cross_set, ml)) {
        int next_node_index = kwg_arc_index(kwg, i);
        bool accepts = kwg_accepts(kwg, i);
        if (number_of_ml > 0) {
          take_letter_and_update_current_index(gen->leave_map, player_rack, ml);
          gen->tiles_played++;
          go_on(opp_rack, gen, col, ml, player, next_node_index, accepts,
                leftstrip, rightstrip, unique_play);
          gen->tiles_played--;
          add_letter_and_update_current_index(gen->leave_map, player_rack, ml);
        }
        // check blank
        if (get_number_of_letter(player_rack, BLANK_MACHINE_LETTER) > 0) {
          take_letter_and_update_current_index(gen->leave_map, player_rack,
                                               BLANK_MACHINE_LETTER);
          gen->tiles_played++;
          go_on(opp_rack, gen, col, get_blanked_machine_letter(ml), player,
                next_node_index, accepts, leftstrip, rightstrip, unique_play);
          gen->tiles_played--;
          add_letter_and_update_current_index(gen->leave_map, player_rack,
                                              BLANK_MACHINE_LETTER);
        }
      }
      if (kwg_is_end(kwg, i)) {
        break;
      }
    }
  }
}

void go_on(const Rack *opp_rack, Generator *gen, int current_col, uint8_t L,
           Player *player, uint32_t new_node_index, bool accepts, int leftstrip,
           int rightstrip, bool unique_play) {
  const KWG *kwg = player_get_kwg(player);
  int player_index = player_get_index(player);
  if (current_col <= gen->current_anchor_col) {
    if (!is_empty_cache(gen, current_col)) {
      gen->strip[current_col] = PLAYED_THROUGH_MARKER;
    } else {
      gen->strip[current_col] = L;
      if (gen->dir && (get_cross_set(gen->board, gen->current_row_index,
                                     current_col, BOARD_HORIZONTAL_DIRECTION,
                                     get_cross_set_index(gen, player_index)) ==
                       TRIVIAL_CROSS_SET)) {
        unique_play = true;
      }
    }
    leftstrip = current_col;
    bool no_letter_directly_left =
        (current_col == 0) || is_empty_cache(gen, current_col - 1);

    if (accepts && no_letter_directly_left && gen->tiles_played > 0 &&
        (unique_play || gen->tiles_played > 1)) {
      record_play(player, opp_rack, gen, leftstrip, rightstrip,
                  GAME_EVENT_TILE_PLACEMENT_MOVE);
    }

    if (new_node_index == 0) {
      return;
    }

    if (current_col > 0 && current_col - 1 != gen->last_anchor_col) {
      recursive_gen(opp_rack, gen, current_col - 1, player, new_node_index,
                    leftstrip, rightstrip, unique_play);
    }

    uint32_t separation_node_index =
        kwg_get_next_node_index(kwg, new_node_index, SEPARATION_MACHINE_LETTER);
    if (separation_node_index != 0 && no_letter_directly_left &&
        gen->current_anchor_col < BOARD_DIM - 1) {
      recursive_gen(opp_rack, gen, gen->current_anchor_col + 1, player,
                    separation_node_index, leftstrip, rightstrip, unique_play);
    }
  } else {
    if (!is_empty_cache(gen, current_col)) {
      gen->strip[current_col] = PLAYED_THROUGH_MARKER;
    } else {
      gen->strip[current_col] = L;
      if (dir_is_vertical(gen->dir) &&
          (get_cross_set(gen->board, gen->current_row_index, current_col,
                         BOARD_HORIZONTAL_DIRECTION,
                         get_cross_set_index(gen, player_index)) ==
           TRIVIAL_CROSS_SET)) {
        unique_play = true;
      }
    }
    rightstrip = current_col;
    bool no_letter_directly_right =
        (current_col == BOARD_DIM - 1) || is_empty_cache(gen, current_col + 1);

    if (accepts && no_letter_directly_right && gen->tiles_played > 0 &&
        (unique_play || gen->tiles_played > 1)) {
      record_play(player, opp_rack, gen, leftstrip, rightstrip,
                  GAME_EVENT_TILE_PLACEMENT_MOVE);
    }

    if (new_node_index != 0 && current_col < BOARD_DIM - 1) {
      recursive_gen(opp_rack, gen, current_col + 1, player, new_node_index,
                    leftstrip, rightstrip, unique_play);
    }
  }
}

bool shadow_allowed_in_cross_set(const Generator *gen, int col,
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

void shadow_record(const Rack *opp_rack, Generator *gen, int left_col,
                   int right_col, int main_played_through_score,
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
    if (!bag_is_empty(gen->bag)) {
      // Bag is not empty: use leave values
      equity +=
          gen->best_leaves[gen->number_of_letters_on_rack - gen->tiles_played];
      // Apply preendgame heuristic if this play would empty the bag or leave
      // few enough tiles remaining.
      int bag_plus_rack_size =
          get_tiles_remaining(gen->bag) - gen->tiles_played + RACK_SIZE;
      if (bag_plus_rack_size < PREENDGAME_ADJUSTMENT_VALUES_LENGTH) {
        equity += gen->preendgame_adjustment_values[bag_plus_rack_size];
      }
    } else {
      // Bag is empty: add double opponent's rack if playing out, otherwise
      // deduct a penalty based on the score of our tiles left after this play.
      equity += shadow_endgame_adjustment(gen, opp_rack, gen->tiles_played);
    }
  }
  if (equity > gen->highest_shadow_equity) {
    gen->highest_shadow_equity = equity;
  }
}

void shadow_play_right(const Rack *opp_rack, Generator *gen,
                       int main_played_through_score,
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
      shadow_record(opp_rack, gen, gen->current_left_col,
                    gen->current_right_col, main_played_through_score,
                    perpendicular_additional_score, word_multiplier);
    }

    shadow_play_right(opp_rack, gen, main_played_through_score,
                      perpendicular_additional_score, word_multiplier,
                      is_unique, cross_set_index);
  }
  gen->tiles_played--;
  gen->current_right_col = original_current_right_col;
}

void shadow_play_left(const Rack *opp_rack, Generator *gen,
                      int main_played_through_score,
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
      shadow_record(opp_rack, gen, gen->current_left_col,
                    gen->current_right_col, main_played_through_score,
                    perpendicular_additional_score, word_multiplier);
    }
    shadow_play_left(opp_rack, gen, main_played_through_score,
                     perpendicular_additional_score, word_multiplier, is_unique,
                     cross_set_index);
  }
  shadow_play_right(opp_rack, gen, main_played_through_score,
                    perpendicular_additional_score, word_multiplier, is_unique,
                    cross_set_index);
  gen->current_left_col = original_current_left_col;
  gen->tiles_played--;
}

void shadow_start(const Rack *opp_rack, Generator *gen, int cross_set_index) {
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
        shadow_record(opp_rack, gen, gen->current_left_col,
                      gen->current_right_col, main_played_through_score,
                      perpendicular_additional_score, 0);
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
  shadow_play_left(opp_rack, gen, main_played_through_score,
                   perpendicular_additional_score, word_multiplier,
                   !dir_is_vertical(gen->dir), cross_set_index);
  shadow_play_right(opp_rack, gen, main_played_through_score,
                    perpendicular_additional_score, word_multiplier,
                    !dir_is_vertical(gen->dir), cross_set_index);
}

void shadow_play_for_anchor(const Rack *opp_rack, Generator *gen, int col,
                            Player *player) {
  Rack *player_rack = player_get_rack(player);
  // set cols
  gen->current_left_col = col;
  gen->current_right_col = col;

  // Reset shadow score
  gen->highest_shadow_equity = 0;

  // Set the number of letters
  gen->number_of_letters_on_rack = get_number_of_letters(player_rack);

  // Set the current anchor column
  gen->current_anchor_col = col;

  // Reset tiles played
  gen->tiles_played = 0;

  // Set rack cross set
  gen->rack_cross_set = 0;
  for (int i = 0; i < letter_distribution_get_size(gen->letter_distribution);
       i++) {
    if (get_number_of_letter(player_rack, i) > 0) {
      gen->rack_cross_set = gen->rack_cross_set | ((uint64_t)1 << i);
    }
  }

  shadow_start(opp_rack, gen,
               get_cross_set_index(gen, player_get_index(player)));
  add_anchor(gen->anchor_list, gen->current_row_index, col,
             gen->last_anchor_col, get_transpose(gen->board),
             dir_is_vertical(gen->dir), gen->highest_shadow_equity);
}

void shadow_by_orientation(const Rack *opp_rack, Generator *gen, Player *player,
                           int dir) {
  for (int row = 0; row < BOARD_DIM; row++) {
    gen->current_row_index = row;
    gen->last_anchor_col = INITIAL_LAST_ANCHOR_COL;
    load_row_letter_cache(gen, gen->current_row_index);
    for (int col = 0; col < BOARD_DIM; col++) {
      if (get_anchor(gen->board, row, col, dir)) {
        shadow_play_for_anchor(opp_rack, gen, col, player);
        gen->last_anchor_col = col;
      }
    }
  }
}

void set_descending_tile_scores(Generator *gen, Player *player) {
  int i = 0;
  Rack *player_rack = player_get_rack(player);
  for (int j = 0;
       j < (int)letter_distribution_get_size(gen->letter_distribution); j++) {
    int j_score_order =
        letter_distribution_get_score_order(gen->letter_distribution, j);
    for (int k = 0; k < get_number_of_letter(player_rack, j_score_order); k++) {
      gen->descending_tile_scores[i] = letter_distribution_get_score(
          gen->letter_distribution, j_score_order);
      i++;
    }
  }
}

void generate_moves(const Rack *opp_rack, Generator *gen, Player *player,
                    bool add_exchange, move_record_t move_record_type,
                    move_sort_t move_sort_type,
                    bool apply_placement_adjustment) {
  gen->move_sort_type = move_sort_type;
  gen->move_record_type = move_record_type;
  gen->apply_placement_adjustment = apply_placement_adjustment;

  Rack *player_rack = player_get_rack(player);
  // Reset the best leaves
  for (int i = 0; i < (RACK_SIZE); i++) {
    gen->best_leaves[i] = (double)(INITIAL_TOP_MOVE_EQUITY);
  }

  init_leave_map(player_rack, gen->leave_map);
  if (get_number_of_letters(player_rack) < RACK_SIZE) {
    set_current_value(gen->leave_map,
                      klv_get_leave_value(player_get_klv(player), player_rack));
  } else {
    set_current_value(gen->leave_map, INITIAL_TOP_MOVE_EQUITY);
  }

  // Set the best leaves and maybe add exchanges.
  generate_exchange_moves(gen, player, 0, 0, add_exchange);

  reset_anchor_list(gen->anchor_list);
  set_descending_tile_scores(gen, player);

  for (int dir = 0; dir < 2; dir++) {
    gen->dir = dir % 2 != 0;
    shadow_by_orientation(opp_rack, gen, player, dir);
    transpose(gen->board);
  }

  // Reset the reused generator fields
  gen->tiles_played = 0;

  sort_anchor_list(gen->anchor_list);
  const AnchorList *anchor_list = gen->anchor_list;
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
    recursive_gen(opp_rack, gen, gen->current_anchor_col, player,
                  kwg_get_root_node_index(player_get_kwg(player)),
                  gen->current_anchor_col, gen->current_anchor_col, !gen->dir);

    if (gen->move_record_type == MOVE_RECORD_BEST) {
      // If a better play has been found than should have been possible for this
      // anchor, highest_possible_equity was invalid.
      assert(!better_play_has_been_found(gen, anchor_highest_possible_equity));
    }
  }

  reset_transpose(gen->board);

  Move *top_move = move_list_get_move(gen->move_list, 0);
  // Add the pass move
  if (move_record_type == MOVE_RECORD_ALL ||
      get_equity(top_move) < PASS_MOVE_EQUITY) {
    set_spare_move_as_pass(gen->move_list);
    insert_spare_move(gen->move_list, PASS_MOVE_EQUITY);
  }
}

void reset_generator(Generator *gen) {
  reset_bag(gen->letter_distribution, gen->bag);
  reset_board(gen->board);
  reset_move_list(gen->move_list);
}

void load_quackle_preendgame_adjustment_values(Generator *gen) {
  double values[] = {0, -8, 0, -0.5, -2, -3.5, -2, 2, 10, 7, 4, -1, -2};
  for (int i = 0; i < PREENDGAME_ADJUSTMENT_VALUES_LENGTH; i++) {
    gen->preendgame_adjustment_values[i] = values[i];
  }
}

void load_zero_preendgame_adjustment_values(Generator *gen) {
  for (int i = 0; i < PREENDGAME_ADJUSTMENT_VALUES_LENGTH; i++) {
    gen->preendgame_adjustment_values[i] = 0;
  }
}

void update_generator(const Config *config, Generator *gen) {
  gen->letter_distribution = config_get_letter_distribution(config);
  update_move_list(gen->move_list, config_get_num_plays(config));
  update_bag(gen->letter_distribution, gen->bag);
  update_leave_map(gen->leave_map,
                   letter_distribution_get_size(gen->letter_distribution));
}

Generator *create_generator(const Config *config, int move_list_capacity) {
  Generator *generator = malloc_or_die(sizeof(Generator));
  LetterDistribution *ld = config_get_letter_distribution(config);
  uint32_t letter_distribution_size = letter_distribution_get_size(ld);
  generator->bag = create_bag(config_get_letter_distribution(config));
  generator->board = create_board();
  generator->move_list = create_move_list(move_list_capacity);
  generator->anchor_list = create_anchor_list();
  generator->leave_map = create_leave_map(letter_distribution_size);
  generator->letter_distribution = ld;
  generator->tiles_played = 0;
  generator->dir = BOARD_HORIZONTAL_DIRECTION;
  generator->last_anchor_col = 0;
  generator->kwgs_are_distinct = !players_data_get_is_shared(
      config_get_players_data(config), PLAYERS_DATA_TYPE_KWG);

  // On by default
  generator->apply_placement_adjustment = true;

  generator->exchange_strip =
      (uint8_t *)malloc_or_die(letter_distribution_size * sizeof(uint8_t));
  // Just load the zero values for now
  load_zero_preendgame_adjustment_values(generator);

  return generator;
}

Generator *generate_duplicate(const Generator *gen, int move_list_capacity) {
  Generator *new_generator = malloc_or_die(sizeof(Generator));
  uint32_t letter_distribution_size =
      letter_distribution_get_size(gen->letter_distribution);
  new_generator->bag = bag_duplicate(gen->bag);
  new_generator->board = board_duplicate(gen->board);
  // Move list, anchor list, and leave map can be new
  new_generator->move_list = create_move_list(move_list_capacity);
  new_generator->anchor_list = create_anchor_list();
  new_generator->leave_map = create_leave_map(letter_distribution_size);
  // KWG and letter distribution are read only and can share pointers
  new_generator->letter_distribution = gen->letter_distribution;
  new_generator->tiles_played = 0;
  new_generator->dir = BOARD_HORIZONTAL_DIRECTION;
  new_generator->last_anchor_col = 0;
  new_generator->kwgs_are_distinct = gen->kwgs_are_distinct;

  new_generator->apply_placement_adjustment = gen->apply_placement_adjustment;

  new_generator->exchange_strip =
      (uint8_t *)malloc_or_die(letter_distribution_size * sizeof(uint8_t));
  // Just load the zero values for now
  load_zero_preendgame_adjustment_values(new_generator);

  return new_generator;
}

void destroy_generator(Generator *gen) {
  destroy_bag(gen->bag);
  destroy_board(gen->board);
  destroy_move_list(gen->move_list);
  destroy_anchor_list(gen->anchor_list);
  destroy_leave_map(gen->leave_map);
  free(gen->exchange_strip);
  free(gen);
}