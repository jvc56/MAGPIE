#include <assert.h>
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
#include "util.h"

#define INITIAL_LAST_ANCHOR_COL (BOARD_DIM)

void go_on(Generator *gen, int current_col, uint8_t L, Player *player,
           Rack *opp_rack, uint32_t new_node_index, int accepts, int leftstrip,
           int rightstrip, int unique_play);

int get_cross_set_index(Generator *gen, int player_index) {
  return gen->kwgs_are_distinct && player_index;
}

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

double shadow_endgame_adjustment(Generator *gen, int num_tiles_played,
                                 Rack *opp_rack) {
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

double endgame_adjustment(Generator *gen, Rack *rack, Rack *opp_rack) {
  if (!rack->empty) {
    // This play is not going out. We should penalize it by our own score
    // plus some constant.
    return ((-(double)score_on_rack(gen->letter_distribution, rack)) *
            NON_OUTPLAY_LEAVE_SCORE_MULTIPLIER_PENALTY) -
           NON_OUTPLAY_CONSTANT_PENALTY;
  }
  return 2 * ((double)score_on_rack(gen->letter_distribution, opp_rack));
}

double get_spare_move_equity(Generator *gen, Player *player, Rack *opp_rack) {
  double leave_adjustment = 0;
  double other_adjustments = 0;

  if (gen->apply_placement_adjustment && gen->board->tiles_played == 0 &&
      gen->move_list->spare_move->move_type == GAME_EVENT_TILE_PLACEMENT_MOVE) {
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

double get_spare_exchange_equity(Generator *gen) {
  double leave_adjustment = get_current_value(gen->leave_map);
  double other_adjustments = 0;

  if (gen->apply_placement_adjustment && gen->board->tiles_played == 0 &&
      gen->move_list->spare_move->move_type == GAME_EVENT_TILE_PLACEMENT_MOVE) {
    other_adjustments = placement_adjustment(gen, gen->move_list->spare_move);
  }

  return leave_adjustment + other_adjustments;
}

// this function assumes the word is always horizontal. If this isn't the case,
// the board needs to be transposed ahead of time.
int score_move(Board *board, uint8_t word[], int word_start_index,
               int word_end_index, int row, int col, int tiles_played,
               int cross_dir, int cross_set_index,
               LetterDistribution *letter_distribution) {
  int ls;
  int main_word_score = 0;
  int cross_scores = 0;
  int bingo_bonus = 0;
  if (tiles_played == RACK_SIZE) {
    bingo_bonus = BINGO_BONUS;
  }
  int word_multiplier = 1;
  for (int idx = 0; idx < word_end_index - word_start_index + 1; idx++) {
    uint8_t ml = word[idx + word_start_index];
    uint8_t bonus_square = get_bonus_square(board, row, col + idx);
    int letter_multiplier = 1;
    int this_word_multiplier = 1;
    int fresh_tile = 0;
    if (ml == PLAYED_THROUGH_MARKER) {
      ml = get_letter(board, row, col + idx);
    } else {
      fresh_tile = 1;
      this_word_multiplier = bonus_square >> 4;
      letter_multiplier = bonus_square & 0x0F;
      word_multiplier *= this_word_multiplier;
    }
    int cs = get_cross_score(board, row, col + idx, cross_dir, cross_set_index);
    if (is_blanked(ml)) {
      ls = 0;
    } else {
      ls = letter_distribution->scores[ml];
    }

    main_word_score += ls * letter_multiplier;
    int actual_cross_word =
        (row > 0 && !is_empty(board, row - 1, col + idx)) ||
        ((row < BOARD_DIM - 1) && !is_empty(board, row + 1, col + idx));
    if (fresh_tile && actual_cross_word) {
      cross_scores += ls * letter_multiplier * this_word_multiplier +
                      cs * this_word_multiplier;
    }
  }
  return main_word_score * word_multiplier + cross_scores + bingo_bonus;
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

  if (move_type == GAME_EVENT_TILE_PLACEMENT_MOVE) {
    score = score_move(gen->board, gen->strip, leftstrip, rightstrip, start_row,
                       start_col, tiles_played, !gen->vertical,
                       get_cross_set_index(gen, player->index),
                       gen->letter_distribution);
    strip = gen->strip;
  } 

  // Set the move to more easily handle equity calculations
  set_spare_move(gen->move_list, strip, leftstrip, rightstrip, score, row, col,
                 tiles_played, gen->vertical, move_type);

  if (player->strategy_params->play_recorder_type == MOVE_RECORDER_ALL) {
    double equity;
    if (player->strategy_params->move_sorting == MOVE_SORT_EQUITY) {
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

void record_exchange(Generator *gen, Player *player, Rack *exchanged) {
  if ((player->strategy_params->play_recorder_type == MOVE_RECORDER_BEST) &&
      (player->strategy_params->move_sorting == MOVE_SORT_EQUITY)) {
    double leave_value = get_current_value(gen->leave_map);
    if (leave_value <= gen->move_list->moves[0]->equity) {
      return;
    }
  }

  int start_row = gen->current_row_index;
  int tiles_exchanged = 0;
  int start_col = 0;
  int row = start_row;
  int col = start_col;

  int score = 0;
  uint8_t *strip = NULL;

  strip = gen->exchange_strip;
  for (uint8_t ml = 0; ml < gen->letter_distribution->size; ml++) {
    int num_this = exchanged->array[ml];
    for (int i = 0; i < num_this; i++) {
      strip[tiles_exchanged] = ml;
      tiles_exchanged++;
    }
  }
  set_spare_move(gen->move_list, strip, 0, tiles_exchanged, score, row, col,
                 tiles_exchanged, gen->vertical, GAME_EVENT_EXCHANGE);
  if (player->strategy_params->play_recorder_type == MOVE_RECORDER_ALL) {
    double equity;
    if (player->strategy_params->move_sorting == MOVE_SORT_EQUITY) {
      equity = get_spare_exchange_equity(gen);
    } else {
      equity = score;
    }
    insert_spare_move(gen->move_list, equity);
  } else {
    insert_spare_move_top_equity(gen->move_list,
                                 get_spare_exchange_equity(gen));
  }
}

void generate_exchange_moves(Generator *gen, Player *player, Rack *leave,
                             int32_t node_index, int32_t word_index, uint8_t ml,
                             int add_exchange) {
  while (ml < (gen->letter_distribution->size) &&
         player->rack->array[ml] == 0) {
    ml++;
  }
  // printf("ml: %d\n", ml);
  // printf("current_index: %d, player->rack->number_of_letters: %d\n",
  //       gen->leave_map->current_index, player->rack->number_of_letters);
  if (ml == (gen->letter_distribution->size)) {
    /*
        StringBuilder *s1 = create_string_builder();
        string_builder_add_rack(player->rack, gen->letter_distribution, s1);
        StringBuilder *s2 = create_string_builder();
        string_builder_add_rack(leave, gen->letter_distribution, s2);
        printf("exchanged: %s leave: %s\n", string_builder_peek(s1),
               string_builder_peek(s2));
        destroy_string_builder(s1);
        destroy_string_builder(s2);
    */
    if (player->rack->number_of_letters > 0) {
      // printf("  gen->leave_map->current_index: %d\n",
      //        gen->leave_map->current_index);
      double value = 0.0;
      if (word_index > 0) {
        value = player->strategy_params->klv->leave_values[word_index - 1];
      }
      // printf("  incremental value: %f\n", value);
      //   value = get_leave_value(player->strategy_params->klv, player->rack);
      //   printf("value from root: %f\n", value);
      set_current_value(gen->leave_map, value);
      if (value > gen->best_leaves[leave->number_of_letters]) {
        gen->best_leaves[leave->number_of_letters] = value;
      }
      if (add_exchange) {
        record_exchange(gen, player, player->rack);
      }
    }
  } else {
    generate_exchange_moves(gen, player, leave, node_index, word_index, ml + 1,
                            add_exchange);
    int num_this = player->rack->array[ml];
    for (int i = 0; i < num_this; i++) {
      add_letter_to_rack(leave, ml);
      // add_letter_and_update_current_index(gen->leave_map, player->rack, ml);
      take_letter_and_update_complement_index(gen->leave_map, player->rack,
                                              leave, ml);
      int32_t next_word_index;
      int32_t next_node_index =
          increment_node_to_ml(player->strategy_params->klv, node_index,
                               word_index, &next_word_index, ml);
      // printf("next_word_index: %d next_node_index: %d\n", next_word_index,
      //        next_node_index);
      node_index = next_node_index;
      word_index = next_word_index;
      {
        int32_t next_word_index;
        int32_t next_node_index =
            follow_arc(player->strategy_params->klv, node_index, word_index,
                       &next_word_index);
        // printf("next_word_index: %d next_node_index: %d\n", next_word_index,
        //        next_node_index);
        node_index = next_node_index;
        word_index = next_word_index;
      }
      generate_exchange_moves(gen, player, leave, node_index, word_index,
                              ml + 1, add_exchange);
    }

    for (int i = 0; i < num_this; i++) {
      take_letter_from_rack(leave, ml);
      // take_letter_and_update_current_index(gen->leave_map, player->rack, ml);
      add_letter_and_update_complement_index(gen->leave_map, player->rack,
                                             leave, ml);
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

int better_play_has_been_found(Generator *gen, double highest_possible_value) {
  const double best_value_found = (gen->move_sorting_type == MOVE_SORT_EQUITY)
                                      ? gen->move_list->moves[0]->equity
                                      : gen->move_list->moves[0]->score;
  return highest_possible_value + COMPARE_MOVES_EPSILON <= best_value_found;
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
      get_cross_set(gen->board, gen->current_row_index, col, cs_direction,
                    get_cross_set_index(gen, player->index));
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
                         BOARD_HORIZONTAL_DIRECTION,
                         get_cross_set_index(gen, player->index)) ==
           TRIVIAL_CROSS_SET)) {
        unique_play = 1;
      }
    }
    leftstrip = current_col;
    int no_letter_directly_left =
        (current_col == 0) || is_empty_cache(gen, current_col - 1);

    if (accepts && no_letter_directly_left && gen->tiles_played > 0 &&
        (unique_play || gen->tiles_played > 1)) {
      record_play(gen, player, opp_rack, leftstrip, rightstrip,
                  GAME_EVENT_TILE_PLACEMENT_MOVE);
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
                         BOARD_HORIZONTAL_DIRECTION,
                         get_cross_set_index(gen, player->index)) ==
           TRIVIAL_CROSS_SET)) {
        unique_play = 1;
      }
    }
    rightstrip = current_col;
    int no_letter_directly_right =
        (current_col == BOARD_DIM - 1) || is_empty_cache(gen, current_col + 1);

    if (accepts && no_letter_directly_right && gen->tiles_played > 0 &&
        (unique_play || gen->tiles_played > 1)) {
      record_play(gen, player, opp_rack, leftstrip, rightstrip,
                  GAME_EVENT_TILE_PLACEMENT_MOVE);
    }

    if (new_node_index != 0 && current_col < BOARD_DIM - 1) {
      recursive_gen(gen, current_col + 1, player, opp_rack, new_node_index,
                    leftstrip, rightstrip, unique_play);
    }
  }
}

int shadow_allowed_in_cross_set(Generator *gen, int col, int cross_set_index) {
  uint64_t cross_set = get_cross_set(gen->board, gen->current_row_index, col,
                                     !gen->vertical, cross_set_index);
  // Allowed if
  // there is a letter on the rack in the cross set or,
  // there is anything in the cross set and the rack has a blank.
  return (cross_set & gen->rack_cross_set) != 0 ||
         ((gen->rack_cross_set & 1) && cross_set);
}

void shadow_record(Generator *gen, int left_col, int right_col,
                   int main_played_through_score,
                   int perpendicular_additional_score, int word_multiplier,
                   Rack *opp_rack) {
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
  if (gen->move_sorting_type == MOVE_SORT_EQUITY) {
    if (gen->bag->last_tile_index >= 0) {
      // Bag is not empty: use leave values
      equity +=
          gen->best_leaves[gen->number_of_letters_on_rack - gen->tiles_played];
      // Apply preendgame heuristic if this play would empty the bag or leave
      // few enough tiles remaining.
      int bag_plus_rack_size =
          (gen->bag->last_tile_index + 1) - gen->tiles_played + RACK_SIZE;
      if (bag_plus_rack_size < PREENDGAME_ADJUSTMENT_VALUES_LENGTH) {
        equity += gen->preendgame_adjustment_values[bag_plus_rack_size];
      }
    } else {
      // Bag is empty: add double opponent's rack if playing out, otherwise
      // deduct a penalty based on the score of our tiles left after this play.
      equity += shadow_endgame_adjustment(gen, gen->tiles_played, opp_rack);
    }
  }
  if (equity > gen->highest_shadow_equity) {
    gen->highest_shadow_equity = equity;
  }
}

void shadow_play_right(Generator *gen, int main_played_through_score,
                       int perpendicular_additional_score, int word_multiplier,
                       int is_unique, int cross_set_index, Rack *opp_rack) {
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
                    !gen->vertical, cross_set_index);
  // Allowed if
  // there is a letter on the rack in the cross set or,
  // there is anything in the cross set and the rack has a blank.
  if ((cross_set & gen->rack_cross_set) != 0 ||
      ((gen->rack_cross_set & 1) && cross_set)) {
    // Play tile and update scoring parameters

    uint8_t bonus_square = get_bonus_square(gen->board, gen->current_row_index,
                                            gen->current_right_col);
    int cross_score = get_cross_score(gen->board, gen->current_row_index,
                                      gen->current_right_col, !gen->vertical,
                                      cross_set_index);
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
                    word_multiplier, opp_rack);
    }

    shadow_play_right(gen, main_played_through_score,
                      perpendicular_additional_score, word_multiplier,
                      is_unique, cross_set_index, opp_rack);
  }
  gen->tiles_played--;
  gen->current_right_col = original_current_right_col;
}

void shadow_play_left(Generator *gen, int main_played_through_score,
                      int perpendicular_additional_score, int word_multiplier,
                      int is_unique, int cross_set_index, Rack *opp_rack) {
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
                    !gen->vertical, cross_set_index);
  // Allowed if
  // there is a letter on the rack in the cross set or,
  // there is anything in the cross set and the rack has a blank.
  if ((cross_set & gen->rack_cross_set) != 0 ||
      ((gen->rack_cross_set & 1) && cross_set)) {
    // Play tile and update scoring parameters

    uint8_t bonus_square = get_bonus_square(gen->board, gen->current_row_index,
                                            gen->current_left_col);
    int cross_score =
        get_cross_score(gen->board, gen->current_row_index,
                        gen->current_left_col, !gen->vertical, cross_set_index);
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
                    word_multiplier, opp_rack);
    }
    shadow_play_left(gen, main_played_through_score,
                     perpendicular_additional_score, word_multiplier, is_unique,
                     cross_set_index, opp_rack);
  }
  shadow_play_right(gen, main_played_through_score,
                    perpendicular_additional_score, word_multiplier, is_unique,
                    cross_set_index, opp_rack);
  gen->current_left_col = original_current_left_col;
  gen->tiles_played--;
}

void shadow_start(Generator *gen, int cross_set_index, Rack *opp_rack) {
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
      int cross_score = get_cross_score(gen->board, gen->current_row_index,
                                        gen->current_left_col, !gen->vertical,
                                        cross_set_index);
      int this_word_multiplier = bonus_square >> 4;
      perpendicular_additional_score += (cross_score * this_word_multiplier);
      word_multiplier = this_word_multiplier;
      gen->tiles_played++;
      if (!gen->vertical) {
        // word_multiplier is always hard-coded as 0 since we are recording a
        // single tile
        shadow_record(gen, gen->current_left_col, gen->current_right_col,
                      main_played_through_score, perpendicular_additional_score,
                      0, opp_rack);
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
                   !gen->vertical, cross_set_index, opp_rack);
  shadow_play_right(gen, main_played_through_score,
                    perpendicular_additional_score, word_multiplier,
                    !gen->vertical, cross_set_index, opp_rack);
}

void shadow_play_for_anchor(Generator *gen, int col, Player *player,
                            Rack *opp_rack) {
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

  shadow_start(gen, get_cross_set_index(gen, player->index), opp_rack);
  insert_anchor(gen->anchor_list, gen->current_row_index, col,
                gen->last_anchor_col, gen->board->transposed, gen->vertical,
                gen->highest_shadow_equity);
}

void shadow_by_orientation(Generator *gen, Player *player, int dir,
                           Rack *opp_rack) {
  for (int row = 0; row < BOARD_DIM; row++) {
    gen->current_row_index = row;
    gen->last_anchor_col = INITIAL_LAST_ANCHOR_COL;
    load_row_letter_cache(gen, gen->current_row_index);
    for (int col = 0; col < BOARD_DIM; col++) {
      if (get_anchor(gen->board, row, col, dir)) {
        shadow_play_for_anchor(gen, col, player, opp_rack);
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
  if (player->rack->number_of_letters > RACK_SIZE) {
    set_current_value(
        gen->leave_map,
        get_leave_value(player->strategy_params->klv, player->rack));
  } else {
    set_current_value(gen->leave_map, INITIAL_TOP_MOVE_EQUITY);
  }
  gen->leave_map->current_index = 0;

  // Set the best leaves and maybe add exchanges.
  int32_t node_index = kwg_arc_index(player->strategy_params->klv->kwg, 0);
  Rack *leave = create_rack(gen->letter_distribution->size);
  generate_exchange_moves(gen, player, leave, node_index, 0, 0, add_exchange);
  destroy_rack(leave);

  gen->leave_map->current_index = (1 << player->rack->number_of_letters) - 1;
  reset_anchor_list(gen->anchor_list);
  set_descending_tile_scores(gen, player);

  for (int dir = 0; dir < 2; dir++) {
    gen->vertical = dir % 2 != 0;
    shadow_by_orientation(gen, player, dir, opp_rack);
    transpose(gen->board);
  }

  // Reset the reused generator fields
  gen->tiles_played = 0;

  for (int i = 0; i < gen->anchor_list->count; i++) {
    if (player->strategy_params->play_recorder_type == MOVE_RECORDER_BEST &&
        better_play_has_been_found(
            gen, gen->anchor_list->anchors[i]->highest_possible_equity)) {
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

    if (player->strategy_params->play_recorder_type == MOVE_RECORDER_BEST) {
      // If a better play has been found than should have been possible for this
      // anchor, highest_possible_equity was invalid.
      assert(!better_play_has_been_found(
          gen, gen->anchor_list->anchors[i]->highest_possible_equity));
    }
  }

  reset_transpose(gen->board);

  // Add the pass move
  if (player->strategy_params->play_recorder_type == MOVE_RECORDER_ALL ||
      gen->move_list->moves[0]->equity < PASS_MOVE_EQUITY) {
    set_spare_move_as_pass(gen->move_list);
    insert_spare_move(gen->move_list, PASS_MOVE_EQUITY);
  } else if (player->strategy_params->play_recorder_type ==
             MOVE_RECORDER_BEST) {
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
  Generator *generator = malloc_or_die(sizeof(Generator));
  generator->bag = create_bag(config->letter_distribution);
  generator->board = create_board();
  generator->move_list = create_move_list(config->move_list_capacity);
  generator->anchor_list = create_anchor_list();
  generator->leave_map = create_leave_map(config->letter_distribution->size);
  generator->letter_distribution = config->letter_distribution;
  generator->tiles_played = 0;
  generator->vertical = 0;
  generator->last_anchor_col = 0;
  generator->kwgs_are_distinct = !config->kwg_is_shared;

  // On by default
  generator->apply_placement_adjustment = 1;

  generator->exchange_strip = (uint8_t *)malloc_or_die(
      config->letter_distribution->size * sizeof(uint8_t));
  // Just load the zero values for now
  load_zero_preendgame_adjustment_values(generator);

  return generator;
}

Generator *copy_generator(Generator *gen, int move_list_size) {
  Generator *new_generator = malloc_or_die(sizeof(Generator));
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
  new_generator->kwgs_are_distinct = gen->kwgs_are_distinct;

  new_generator->apply_placement_adjustment = gen->apply_placement_adjustment;

  new_generator->exchange_strip = (uint8_t *)malloc_or_die(
      gen->letter_distribution->size * sizeof(uint8_t));
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