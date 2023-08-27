#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "constants.h"
#include "cross_set.h"
#include "klv.h"
#include "kwg.h"
#include "leave_map.h"
#include "movegen.h"
#include "player.h"
#include "rack.h"

void go_on(Generator *gen, int current_col, uint8_t L, Player *player,
           Rack *opp_rack, uint32_t new_node_index, int accepts, int leftstrip,
           int rightstrip, int unique_play);

double placement_adjustment(Generator *gen, Move *move) {
  int start = move->col_start;
  int end = start + move->tiles_played;

  int j = start;
  double penalty = 0;
  double v_penalty = OPENING_HOTSPOT_PENALTY;

  while (j < end) {
    int tile = move->tiles[j - start];
    if (is_blanked(tile)) {
      tile = get_unblanked_machine_letter(tile);
    }
    if (gen->letter_distribution->is_vowel[tile] &&
        (j == 2 || j == 6 || j == 8 || j == 12)) {
      penalty += v_penalty;
    }
    j++;
  }
  return penalty;
}

double endgame_adjustment(Generator *gen, Rack *rack, Rack *opp_rack) {
  if (!rack->empty) {
    // This play is not going out. We should penalize it by our own score
    // plus some constant.
    return ((-(double)score_on_rack(gen->letter_distribution, rack)) * 2) - 10;
  }
  return 2 * ((double)score_on_rack(gen->letter_distribution, opp_rack));
}

double get_spare_move_equity(Generator *gen, Player *player, Rack *opp_rack) {
  double leave_adjustment = 0;
  double other_adjustments = 0;

  if (gen->apply_placement_adjustment && gen->board->tiles_played == 0 &&
      gen->move_list->spare_move->move_type == MOVE_TYPE_PLAY) {
    other_adjustments = placement_adjustment(gen, gen->move_list->spare_move);
  }

  if (gen->bag->last_tile_index >= 0) {
    leave_adjustment = get_current_value(gen->leave_map);
    int bag_plus_rack_size = (gen->bag->last_tile_index + 1) -
                             gen->move_list->spare_move->tiles_played +
                             RACK_SIZE;
    if (bag_plus_rack_size < PREENDGAME_ADJUSTMENT_VALUES_LENGTH) {
      other_adjustments +=
          gen->preendgame_adjustment_values[bag_plus_rack_size];
    }
  } else {
    other_adjustments += endgame_adjustment(gen, player->rack, opp_rack);
  }

  return ((double)gen->move_list->spare_move->score) + leave_adjustment +
         other_adjustments;
}

void record_play(Generator *gen, Player *player, Rack *opp_rack, int leftstrip,
                 int rightstrip, int move_type) {
  int start_row = gen->current_row_index;
  int tiles_played = gen->tiles_played;
  int start_col = leftstrip;
  int row = start_row;
  int col = start_col;

  if (gen->vertical) {
    int temp = row;
    row = col;
    col = temp;
  }

  int score = 0;
  uint8_t *strip = NULL;

  if (move_type == MOVE_TYPE_PLAY) {
    score = score_move(gen->board, gen->strip, leftstrip, rightstrip, start_row,
                       start_col, tiles_played, !gen->vertical,
                       gen->letter_distribution);
    strip = gen->strip;
  } else if (move_type == MOVE_TYPE_EXCHANGE) {
    // ignore the empty exchange case
    if (rightstrip == 0) {
      return;
    }
    tiles_played = rightstrip;
    strip = gen->exchange_strip;
  }

  // Set the move to more easily handle equity calculations
  set_spare_move(gen->move_list, strip, leftstrip, rightstrip, score, row, col,
                 tiles_played, gen->vertical, move_type);

  if (player->strategy_params->play_recorder_type == PLAY_RECORDER_TYPE_ALL) {
    double equity;
    if (player->strategy_params->move_sorting == SORT_BY_EQUITY) {
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
                             int stripidx, int add_exchange) {
  while (ml < (gen->letter_distribution->size) &&
         player->rack->array[ml] == 0) {
    ml++;
  }
  if (ml == (gen->letter_distribution->size)) {
    // The recording of an exchange should never require
    // the opponent's rack.

    // Ignore the empty exchange case for full racks
    // to avoid out of bounds errors for the best_leaves array
    if (player->rack->number_of_letters < RACK_SIZE) {
      double current_value =
          get_leave_value(player->strategy_params->klv, player->rack);
      set_current_value(gen->leave_map, current_value);
      if (current_value > gen->best_leaves[player->rack->number_of_letters]) {
        gen->best_leaves[player->rack->number_of_letters] = current_value;
      }
      if (add_exchange) {
        record_play(gen, player, NULL, 0, stripidx, MOVE_TYPE_EXCHANGE);
      }
    }
  } else {
    generate_exchange_moves(gen, player, ml + 1, stripidx, add_exchange);
    int num_this = player->rack->array[ml];
    for (int i = 0; i < num_this; i++) {
      gen->exchange_strip[stripidx] = ml;
      stripidx += 1;
      take_letter_and_update_current_index(gen->leave_map, player->rack, ml);
      generate_exchange_moves(gen, player, ml + 1, stripidx, add_exchange);
    }
    for (int i = 0; i < num_this; i++) {
      add_letter_and_update_current_index(gen->leave_map, player->rack, ml);
    }
  }
}

void load_row_letter_cache(Generator *gen, int row) {
  for (int col = 0; col < BOARD_DIM; col++) {
    gen->row_letter_cache[col] = get_letter(gen->board, row, col);
  }
}

uint8_t get_letter_cache(Generator *gen, int col) {
  return gen->row_letter_cache[col];
}

int is_empty_cache(Generator *gen, int col) {
  return get_letter_cache(gen, col) == ALPHABET_EMPTY_SQUARE_MARKER;
}

void recursive_gen(Generator *gen, int col, Player *player, Rack *opp_rack,
                   uint32_t node_index, int leftstrip, int rightstrip,
                   int unique_play) {
  int cs_direction;
  uint8_t current_letter = get_letter_cache(gen, col);
  if (gen->vertical) {
    cs_direction = BOARD_HORIZONTAL_DIRECTION;
  } else {
    cs_direction = BOARD_VERTICAL_DIRECTION;
  }
  uint64_t cross_set =
      get_cross_set(gen->board, gen->current_row_index, col, cs_direction);
  if (current_letter != ALPHABET_EMPTY_SQUARE_MARKER) {
    int raw = get_unblanked_machine_letter(current_letter);
    int next_node_index = 0;
    int accepts = 0;
    for (int i = node_index;; i++) {
      if (kwg_tile(player->strategy_params->kwg, i) == raw) {
        next_node_index = kwg_arc_index(player->strategy_params->kwg, i);
        accepts = kwg_accepts(player->strategy_params->kwg, i);
        break;
      }
      if (kwg_is_end(player->strategy_params->kwg, i)) {
        break;
      }
    }
    go_on(gen, col, current_letter, player, opp_rack, next_node_index, accepts,
          leftstrip, rightstrip, unique_play);
  } else if (!player->rack->empty) {
    for (int i = node_index;; i++) {
      int ml = kwg_tile(player->strategy_params->kwg, i);
      if (ml != 0 &&
          (player->rack->array[ml] != 0 || player->rack->array[0] != 0) &&
          allowed(cross_set, ml)) {
        int next_node_index = kwg_arc_index(player->strategy_params->kwg, i);
        int accepts = kwg_accepts(player->strategy_params->kwg, i);
        if (player->rack->array[ml] > 0) {
          take_letter_and_update_current_index(gen->leave_map, player->rack,
                                               ml);
          gen->tiles_played++;
          go_on(gen, col, ml, player, opp_rack, next_node_index, accepts,
                leftstrip, rightstrip, unique_play);
          gen->tiles_played--;
          add_letter_and_update_current_index(gen->leave_map, player->rack, ml);
        }
        // check blank
        if (player->rack->array[0] > 0) {
          take_letter_and_update_current_index(gen->leave_map, player->rack,
                                               BLANK_MACHINE_LETTER);
          gen->tiles_played++;
          go_on(gen, col, get_blanked_machine_letter(ml), player, opp_rack,
                next_node_index, accepts, leftstrip, rightstrip, unique_play);
          gen->tiles_played--;
          add_letter_and_update_current_index(gen->leave_map, player->rack,
                                              BLANK_MACHINE_LETTER);
        }
      }
      if (kwg_is_end(player->strategy_params->kwg, i)) {
        break;
      }
    }
  }
}

void go_on(Generator *gen, int current_col, uint8_t L, Player *player,
           Rack *opp_rack, uint32_t new_node_index, int accepts, int leftstrip,
           int rightstrip, int unique_play) {
  if (current_col <= gen->current_anchor_col) {
    if (!is_empty_cache(gen, current_col)) {
      gen->strip[current_col] = PLAYED_THROUGH_MARKER;
    } else {
      gen->strip[current_col] = L;
      if (gen->vertical &&
          (get_cross_set(gen->board, gen->current_row_index, current_col,
                         BOARD_HORIZONTAL_DIRECTION) == TRIVIAL_CROSS_SET)) {
        unique_play = 1;
      }
    }
    leftstrip = current_col;
    int no_letter_directly_left =
        (current_col == 0) || is_empty_cache(gen, current_col - 1);

    if (accepts && no_letter_directly_left && gen->tiles_played > 0 &&
        (unique_play || gen->tiles_played > 1)) {
      record_play(gen, player, opp_rack, leftstrip, rightstrip, MOVE_TYPE_PLAY);
    }

    if (new_node_index == 0) {
      return;
    }

    if (current_col > 0 && current_col - 1 != gen->last_anchor_col) {
      recursive_gen(gen, current_col - 1, player, opp_rack, new_node_index,
                    leftstrip, rightstrip, unique_play);
    }

    uint32_t separation_node_index =
        kwg_get_next_node_index(player->strategy_params->kwg, new_node_index,
                                SEPARATION_MACHINE_LETTER);
    if (separation_node_index != 0 && no_letter_directly_left &&
        gen->current_anchor_col < BOARD_DIM - 1) {
      recursive_gen(gen, gen->current_anchor_col + 1, player, opp_rack,
                    separation_node_index, leftstrip, rightstrip, unique_play);
    }
  } else {
    if (!is_empty_cache(gen, current_col)) {
      gen->strip[current_col] = PLAYED_THROUGH_MARKER;
    } else {
      gen->strip[current_col] = L;
      if (gen->vertical &&
          (get_cross_set(gen->board, gen->current_row_index, current_col,
                         BOARD_HORIZONTAL_DIRECTION) == TRIVIAL_CROSS_SET)) {
        unique_play = 1;
      }
    }
    rightstrip = current_col;
    int no_letter_directly_right =
        (current_col == BOARD_DIM - 1) || is_empty_cache(gen, current_col + 1);

    if (accepts && no_letter_directly_right && gen->tiles_played > 0 &&
        (unique_play || gen->tiles_played > 1)) {
      record_play(gen, player, opp_rack, leftstrip, rightstrip, MOVE_TYPE_PLAY);
    }

    if (new_node_index != 0 && current_col < BOARD_DIM - 1) {
      recursive_gen(gen, current_col + 1, player, opp_rack, new_node_index,
                    leftstrip, rightstrip, unique_play);
    }
  }
}

int shadow_allowed_in_cross_set(Generator *gen, int col) {
  uint64_t cross_set =
      get_cross_set(gen->board, gen->current_row_index, col, !gen->vertical);
  // Allowed if
  // there is a letter on the rack in the cross set or,
  // there is anything in the cross set and the rack has a blank.
  return (cross_set & gen->rack_cross_set) != 0 ||
         ((gen->rack_cross_set & 1) && cross_set);
}

void shadow_record(Generator *gen, int left_col, int right_col,
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
      int is_cross_word =
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
    bingo_bonus = BINGO_BONUS;
  }

  int score = tiles_played_score +
              (main_played_through_score * word_multiplier) +
              perpendicular_additional_score + bingo_bonus;
  double equity = (double)score;
  if (gen->move_sorting_type == SORT_BY_EQUITY) {
    equity +=
        gen->best_leaves[gen->number_of_letters_on_rack - gen->tiles_played];
  }
  if (equity > gen->highest_shadow_equity) {
    gen->highest_shadow_equity = equity;
  }
}

void shadow_play_right(Generator *gen, int main_played_through_score,
                       int perpendicular_additional_score, int word_multiplier,
                       int is_unique) {

  if (gen->current_right_col == (BOARD_DIM - 1) ||
      gen->tiles_played >= gen->number_of_letters_on_rack) {
    // We have gone all the way left or right.
    return;
  }

  int original_current_right_col = gen->current_right_col;
  gen->current_right_col++;
  gen->tiles_played++;

  uint64_t cross_set = get_cross_set(gen->board, gen->current_row_index,
                                     gen->current_right_col, !gen->vertical);
  // Allowed if
  // there is a letter on the rack in the cross set or,
  // there is anything in the cross set and the rack has a blank.
  if ((cross_set & gen->rack_cross_set) != 0 ||
      ((gen->rack_cross_set & 1) && cross_set)) {
    // Play tile and update scoring parameters

    uint8_t bonus_square = get_bonus_square(gen->board, gen->current_row_index,
                                            gen->current_right_col);
    int cross_score = get_cross_score(gen->board, gen->current_row_index,
                                      gen->current_right_col, !gen->vertical);
    int this_word_multiplier = bonus_square >> 4;
    perpendicular_additional_score += cross_score * this_word_multiplier;
    word_multiplier *= this_word_multiplier;

    if (cross_set == TRIVIAL_CROSS_SET) {
      // If the horizontal direction is the trivial cross-set, this means
      // that there are no letters perpendicular to where we just placed
      // this letter. So any play we generate here should be unique.
      // We use this to avoid generating duplicates of single-tile plays.
      is_unique = 1;
    }
    // Continue playing right until an empty square or the edge of board is hit
    while (gen->current_right_col + 1 < BOARD_DIM) {
      uint8_t next_letter = get_letter_cache(gen, gen->current_right_col + 1);
      if (next_letter == ALPHABET_EMPTY_SQUARE_MARKER) {
        break;
      }
      if (!is_blanked(next_letter)) {
        main_played_through_score +=
            gen->letter_distribution->scores[next_letter];
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
                      is_unique);
  }
  gen->tiles_played--;
  gen->current_right_col = original_current_right_col;
}

void shadow_play_left(Generator *gen, int main_played_through_score,
                      int perpendicular_additional_score, int word_multiplier,
                      int is_unique) {
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
  uint64_t cross_set = get_cross_set(gen->board, gen->current_row_index,
                                     gen->current_left_col, !gen->vertical);
  // Allowed if
  // there is a letter on the rack in the cross set or,
  // there is anything in the cross set and the rack has a blank.
  if ((cross_set & gen->rack_cross_set) != 0 ||
      ((gen->rack_cross_set & 1) && cross_set)) {
    // Play tile and update scoring parameters

    uint8_t bonus_square = get_bonus_square(gen->board, gen->current_row_index,
                                            gen->current_left_col);
    int cross_score = get_cross_score(gen->board, gen->current_row_index,
                                      gen->current_left_col, !gen->vertical);
    int this_word_multiplier = bonus_square >> 4;
    perpendicular_additional_score += cross_score * this_word_multiplier;
    word_multiplier *= this_word_multiplier;

    if (cross_set == TRIVIAL_CROSS_SET) {
      // See equivalent in shadow_play_right for the reasoning here.
      is_unique = 1;
    }

    if (gen->tiles_played + is_unique >= 2) {
      shadow_record(gen, gen->current_left_col, gen->current_right_col,
                    main_played_through_score, perpendicular_additional_score,
                    word_multiplier);
    }
    shadow_play_left(gen, main_played_through_score,
                     perpendicular_additional_score, word_multiplier,
                     is_unique);
  }
  shadow_play_right(gen, main_played_through_score,
                    perpendicular_additional_score, word_multiplier, is_unique);
  gen->current_left_col = original_current_left_col;
  gen->tiles_played--;
}

void shadow_start(Generator *gen) {
  int main_played_through_score = 0;
  int perpendicular_additional_score = 0;
  int word_multiplier = 1;
  uint8_t current_letter = get_letter_cache(gen, gen->current_left_col);

  if (current_letter == ALPHABET_EMPTY_SQUARE_MARKER) {
    // Only play a letter if a letter from the rack fits in the cross set
    if (shadow_allowed_in_cross_set(gen, gen->current_left_col)) {
      // Play tile and update scoring parameters

      uint8_t bonus_square = get_bonus_square(
          gen->board, gen->current_row_index, gen->current_left_col);
      int cross_score = get_cross_score(gen->board, gen->current_row_index,
                                        gen->current_left_col, !gen->vertical);
      int this_word_multiplier = bonus_square >> 4;
      perpendicular_additional_score += (cross_score * this_word_multiplier);
      word_multiplier = this_word_multiplier;
      gen->tiles_played++;
      if (!gen->vertical) {
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
        main_played_through_score +=
            gen->letter_distribution->scores[current_letter];
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
                   !gen->vertical);
  shadow_play_right(gen, main_played_through_score,
                    perpendicular_additional_score, word_multiplier,
                    !gen->vertical);
}

void shadow_play_for_anchor(Generator *gen, int col, Player *player) {

  // set cols
  gen->current_left_col = col;
  gen->current_right_col = col;

  // Reset shadow score
  gen->highest_shadow_equity = 0;

  // Set the number of letters
  gen->number_of_letters_on_rack = player->rack->number_of_letters;

  // Set the current anchor column
  gen->current_anchor_col = col;

  // Set the recorder type
  gen->move_sorting_type = player->strategy_params->move_sorting;

  // Reset tiles played
  gen->tiles_played = 0;

  // Set rack cross set
  gen->rack_cross_set = 0;
  for (uint32_t i = 0; i < gen->letter_distribution->size; i++) {
    if (player->rack->array[i] > 0) {
      gen->rack_cross_set = gen->rack_cross_set | (1 << i);
    }
  }

  shadow_start(gen);
  insert_anchor(gen->anchor_list, gen->current_row_index, col,
                gen->last_anchor_col, gen->board->transposed, gen->vertical,
                gen->highest_shadow_equity);
}

void shadow_by_orientation(Generator *gen, Player *player, int dir) {
  for (int row = 0; row < BOARD_DIM; row++) {
    gen->current_row_index = row;
    gen->last_anchor_col = INITIAL_LAST_ANCHOR_COL;
    load_row_letter_cache(gen, gen->current_row_index);
    for (int col = 0; col < BOARD_DIM; col++) {
      if (get_anchor(gen->board, row, col, dir)) {
        shadow_play_for_anchor(gen, col, player);
        gen->last_anchor_col = col;
      }
    }
  }
}

void set_descending_tile_scores(Generator *gen, Player *player) {
  int i = 0;
  for (int j = 0; j < (int)gen->letter_distribution->size; j++) {
    for (int k = 0;
         k < player->rack->array[gen->letter_distribution->score_order[j]];
         k++) {
      gen->descending_tile_scores[i] =
          gen->letter_distribution
              ->scores[gen->letter_distribution->score_order[j]];
      i++;
    }
  }
}

void generate_moves(Generator *gen, Player *player, Rack *opp_rack,
                    int add_exchange) {
  // Reset the best leaves
  for (int i = 0; i < (RACK_SIZE); i++) {
    gen->best_leaves[i] = (double)(INITIAL_TOP_MOVE_EQUITY);
  }

  init_leave_map(gen->leave_map, player->rack);
  if (player->rack->number_of_letters < RACK_SIZE) {
    set_current_value(
        gen->leave_map,
        get_leave_value(player->strategy_params->klv, player->rack));
  } else {
    set_current_value(gen->leave_map, INITIAL_TOP_MOVE_EQUITY);
  }

  // Set the best leaves and maybe add exchanges.
  generate_exchange_moves(gen, player, 0, 0, add_exchange);

  reset_anchor_list(gen->anchor_list);
  set_descending_tile_scores(gen, player);

  for (int dir = 0; dir < 2; dir++) {
    gen->vertical = dir % 2 != 0;
    shadow_by_orientation(gen, player, dir);
    transpose(gen->board);
  }

  // Reset the reused generator fields
  gen->tiles_played = 0;

  for (int i = 0; i < gen->anchor_list->count; i++) {
    if (player->strategy_params->play_recorder_type ==
            PLAY_RECORDER_TYPE_TOP_EQUITY &&
        gen->anchor_list->anchors[i]->highest_possible_equity <
            gen->move_list->moves[0]->equity) {
      break;
    }
    gen->current_anchor_col = gen->anchor_list->anchors[i]->col;
    gen->current_row_index = gen->anchor_list->anchors[i]->row;
    gen->last_anchor_col = gen->anchor_list->anchors[i]->last_anchor_col;
    gen->vertical = gen->anchor_list->anchors[i]->vertical;
    set_transpose(gen->board, gen->anchor_list->anchors[i]->transpose_state);
    load_row_letter_cache(gen, gen->current_row_index);
    recursive_gen(gen, gen->current_anchor_col, player, opp_rack,
                  kwg_get_root_node_index(player->strategy_params->kwg),
                  gen->current_anchor_col, gen->current_anchor_col,
                  !gen->vertical);
  }

  reset_transpose(gen->board);

  // Add the pass move
  if (player->strategy_params->play_recorder_type == PLAY_RECORDER_TYPE_ALL ||
      gen->move_list->moves[0]->equity < PASS_MOVE_EQUITY) {
    set_spare_move(gen->move_list, gen->strip, 0, 0, 0, 0, 0, 0, 0,
                   MOVE_TYPE_PASS);
    insert_spare_move(gen->move_list, PASS_MOVE_EQUITY);
  } else if (player->strategy_params->play_recorder_type ==
             PLAY_RECORDER_TYPE_TOP_EQUITY) {
    // The move list count is still 0 at this point, so set it to 1.
    // This is done here to avoid repeatedly checking/updating the move count.
    gen->move_list->count = 1;
  }
}

void reset_generator(Generator *gen) {
  reset_bag(gen->bag, gen->letter_distribution);
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

Generator *create_generator(Config *config) {
  Generator *generator = malloc(sizeof(Generator));
  generator->bag = create_bag(config->letter_distribution);
  generator->board = create_board();
  generator->move_list = create_move_list(config->move_list_capacity);
  generator->anchor_list = create_anchor_list();
  generator->leave_map = create_leave_map(config->letter_distribution->size);
  generator->letter_distribution = config->letter_distribution;
  generator->tiles_played = 0;
  generator->vertical = 0;
  generator->last_anchor_col = 0;
  // On by default
  generator->apply_placement_adjustment = 1;

  generator->exchange_strip =
      (uint8_t *)malloc(config->letter_distribution->size * sizeof(uint8_t));
  // Just load the zero values for now
  load_zero_preendgame_adjustment_values(generator);

  return generator;
}

Generator *copy_generator(Generator *gen, int move_list_size) {
  Generator *new_generator = malloc(sizeof(Generator));
  new_generator->bag = copy_bag(gen->bag);
  new_generator->board = copy_board(gen->board);
  // Move list, anchor list, and leave map can be new
  new_generator->move_list = create_move_list(move_list_size);
  new_generator->anchor_list = create_anchor_list();
  new_generator->leave_map = create_leave_map(gen->letter_distribution->size);
  // KWG and letter distribution are read only and can share pointers
  new_generator->letter_distribution = gen->letter_distribution;
  new_generator->tiles_played = 0;
  new_generator->vertical = 0;
  new_generator->last_anchor_col = 0;

  new_generator->apply_placement_adjustment = gen->apply_placement_adjustment;

  new_generator->exchange_strip =
      (uint8_t *)malloc(gen->letter_distribution->size * sizeof(uint8_t));
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