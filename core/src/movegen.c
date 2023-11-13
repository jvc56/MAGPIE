#include "movegen.h"

#include <assert.h>
#include <ctype.h>
#include <float.h>
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
#include "player.h"
#include "rack.h"
#include "util.h"

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

double bingo_endgame_adjustment(Generator *gen, Rack *opp_rack) {
  // An endgame bingo is always an outplay, and this function is used instead
  // of endgame_adjustment because bingo_gen does not manipulate the player rack
  // in the same way as recursive_gen. This should behave the same as the
  // rack->empty case in endgame_adjustment.
  return 2 * ((double)score_on_rack(gen->letter_distribution, opp_rack));
}

double get_spare_bingo_equity(Generator *gen, Rack *opp_rack) {
  double adjustments = 0;

  if (gen->apply_placement_adjustment && gen->board->tiles_played == 0) {
    adjustments = placement_adjustment(gen, gen->move_list->spare_move);
    // printf("adjustments (placement): %f\n", adjustments);
  }

  if (gen->bag->last_tile_index >= 0) {
    int bag_plus_rack_size = (gen->bag->last_tile_index + 1) -
                             gen->move_list->spare_move->tiles_played +
                             RACK_SIZE;
    if (bag_plus_rack_size < PREENDGAME_ADJUSTMENT_VALUES_LENGTH) {
      adjustments += gen->preendgame_adjustment_values[bag_plus_rack_size];
      // printf("adjustments (preendgame): %f\n", adjustments);
    }
  } else {
    adjustments += bingo_endgame_adjustment(gen, opp_rack);
    // printf("adjustments (endgame): %f\n", adjustments);
  }

  return ((double)gen->move_list->spare_move->score) + adjustments;
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

void record_bingo(Generator *gen, Player *player, Rack *opp_rack, int row,
                  int col, uint8_t *bingo, int score) {
  int tiles_played = RACK_SIZE;

  if (gen->vertical) {
    int temp = row;
    row = col;
    col = temp;
  }

  // Set the move to more easily handle equity calculations
  set_spare_move(gen->move_list, bingo, 0, tiles_played - 1, score, row, col,
                 tiles_played, gen->vertical, GAME_EVENT_TILE_PLACEMENT_MOVE);

  /*
    StringBuilder *sb = create_string_builder();
    string_builder_add_move(gen->board, gen->move_list->spare_move,
                            gen->letter_distribution, sb);
    printf("recording bingo %s\n", string_builder_peek(sb));
    destroy_string_builder(sb);
  */
  if (player->strategy_params->play_recorder_type == MOVE_RECORDER_ALL) {
    double equity;
    if (player->strategy_params->move_sorting == MOVE_SORT_EQUITY) {
      equity = get_spare_bingo_equity(gen, opp_rack);
    } else {
      equity = score;
    }
    insert_spare_move(gen->move_list, equity);
  } else {
    insert_spare_move_top_equity(gen->move_list,
                                 get_spare_bingo_equity(gen, opp_rack));
  }
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
    /*
        StringBuilder *sb = create_string_builder();
        string_builder_add_move(gen->board, gen->move_list->moves[0],
                                gen->letter_distribution, sb);
        printf("recording play %s\n", string_builder_peek(sb));
        destroy_string_builder(sb);
    */
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
        record_play(gen, player, NULL, 0, stripidx, GAME_EVENT_EXCHANGE);
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
  } else if (!player->rack->empty &&
             (gen->tiles_played < gen->max_tiles_to_play) &&
             !((player->strategy_params->play_recorder_type ==
                MOVE_RECORDER_BEST) &&
               better_play_has_been_found(
                   gen,
                   gen->highest_equity_by_length[gen->tiles_played + 1]))) {
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

    const int length = rightstrip - leftstrip + 1;
    const int num_tiles_played_through = length - gen->tiles_played;
    if (accepts && no_letter_directly_left && gen->tiles_played > 0 &&
        (unique_play || gen->tiles_played > 1) &&
        (num_tiles_played_through >= gen->min_num_playthrough) &&
        (num_tiles_played_through <= gen->max_num_playthrough) &&
        (gen->tiles_played >= gen->min_tiles_to_play) &&
        (gen->tiles_played <= gen->max_tiles_to_play)) {
      /*
            printf("leftstrip: %d, rightstrip: %d, length: %d, num_tiles_played:
         %d\n", leftstrip, rightstrip, length, gen->tiles_played);
            printf("record_play: %d %d %d %d %d %d\n", gen->tiles_played,
                   gen->min_tiles_to_play, gen->max_tiles_to_play,
                   num_tiles_played_through, gen->min_num_playthrough,
                   gen->max_num_playthrough);
      */
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

    const int length = rightstrip - leftstrip + 1;
    const int num_tiles_played_through = length - gen->tiles_played;
    if (accepts && no_letter_directly_right && gen->tiles_played > 0 &&
        (unique_play || gen->tiles_played > 1) &&
        (num_tiles_played_through >= gen->min_num_playthrough) &&
        (num_tiles_played_through <= gen->max_num_playthrough) &&
        (gen->tiles_played >= gen->min_tiles_to_play) &&
        (gen->tiles_played <= gen->max_tiles_to_play)) {
      /*
            printf("leftstrip: %d, rightstrip: %d, length: %d, num_tiles_played:
         %d\n", leftstrip, rightstrip, length, gen->tiles_played);
            printf("record_play: %d %d %d %d %d %d\n", gen->tiles_played,
                   gen->min_tiles_to_play, gen->max_tiles_to_play,
                   num_tiles_played_through, gen->min_num_playthrough,
                   gen->max_num_playthrough);
      */
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
                   int main_played_through_score, int num_tiles_played_through,
                   int perpendicular_additional_score, int word_multiplier,
                   Rack *opp_rack) {
  printf("shadow_record(%d, %d, %d, %d, %d, %d...)\n", left_col, right_col,
         main_played_through_score, num_tiles_played_through,
         perpendicular_additional_score, word_multiplier);
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

  printf("tiles_played_score: %d\n", tiles_played_score);
  int score = tiles_played_score +
              (main_played_through_score * word_multiplier) +
              perpendicular_additional_score + bingo_bonus;
  printf("score: %d\n", score);
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

  const int num_played = gen->tiles_played;
  if (equity > gen->highest_shadow_equity) {
    gen->highest_shadow_equity = equity;
  }
  if (equity > gen->highest_equity_by_length[num_played]) {
    gen->highest_equity_by_length[num_played] = equity;
  }
  if (num_tiles_played_through < gen->min_num_playthrough) {
    gen->min_num_playthrough = num_tiles_played_through;
  }
  if (num_tiles_played_through > gen->max_num_playthrough) {
    gen->max_num_playthrough = num_tiles_played_through;
  }
  if (num_played < gen->min_tiles_to_play) {
    gen->min_tiles_to_play = num_played;
  }
  if (num_played > gen->max_tiles_to_play) {
    gen->max_tiles_to_play = num_played;
  }
  const int left_of_anchor = gen->current_anchor_col - left_col;
  if (num_played > gen->max_tiles_starting_left_by[left_of_anchor]) {
    gen->max_tiles_starting_left_by[left_of_anchor] = num_played;
  }
  ShadowLimit *shadow_limit_table_entry =
      &(gen->shadow_limit_table[left_of_anchor][num_played]);
  if (equity > shadow_limit_table_entry->highest_equity) {
    shadow_limit_table_entry->highest_equity = equity;
    shadow_limit_table_entry->num_playthrough = num_tiles_played_through;
  }
}

void shadow_play_right(Generator *gen, int main_played_through_score,
                       int num_tiles_played_through,
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
      num_tiles_played_through++;
      if (!is_blanked(next_letter)) {
        main_played_through_score +=
            gen->letter_distribution->scores[next_letter];
      }
      gen->current_right_col++;
    }

    if (gen->tiles_played + is_unique >= 2) {
      shadow_record(gen, gen->current_left_col, gen->current_right_col,
                    main_played_through_score, num_tiles_played_through,
                    perpendicular_additional_score, word_multiplier, opp_rack);
    }

    shadow_play_right(gen, main_played_through_score, num_tiles_played_through,
                      perpendicular_additional_score, word_multiplier,
                      is_unique, cross_set_index, opp_rack);
  }
  gen->tiles_played--;
  gen->current_right_col = original_current_right_col;
}

void shadow_play_left(Generator *gen, int main_played_through_score,
                      int num_tiles_played_through,
                      int perpendicular_additional_score, int word_multiplier,
                      int is_unique, int cross_set_index, Rack *opp_rack) {
  printf("shadow_play_left\n");
  // Go left until hitting an empty square or the edge of the board.
  if (gen->current_left_col == 0 ||
      gen->current_left_col == gen->last_anchor_col + 1 ||
      gen->tiles_played >= gen->number_of_letters_on_rack) {
    // We have gone all the way left or right.
    return;
  }

  int original_current_left_col = gen->current_left_col;
  gen->current_left_col--;
  printf("gen->current_left_col: %d\n", gen->current_left_col);
  gen->tiles_played++;
  printf("gen->tiles_played: %d\n", gen->tiles_played);
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
                    main_played_through_score, num_tiles_played_through,
                    perpendicular_additional_score, word_multiplier, opp_rack);
    }
    shadow_play_left(gen, main_played_through_score, num_tiles_played_through,
                     perpendicular_additional_score, word_multiplier, is_unique,
                     cross_set_index, opp_rack);
  }
  shadow_play_right(gen, main_played_through_score, num_tiles_played_through,
                    perpendicular_additional_score, word_multiplier, is_unique,
                    cross_set_index, opp_rack);
  gen->current_left_col = original_current_left_col;
  gen->tiles_played--;
}

void shadow_start(Generator *gen, int cross_set_index, Rack *opp_rack) {
  printf("shadow_start\n");
  int main_played_through_score = 0;
  int num_tiles_played_through = 0;
  int perpendicular_additional_score = 0;
  int word_multiplier = 1;
  uint8_t current_letter = get_letter_cache(gen, gen->current_left_col);

  if (current_letter == ALPHABET_EMPTY_SQUARE_MARKER) {
    printf("empty square\n");
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
                      main_played_through_score, num_tiles_played_through,
                      perpendicular_additional_score, 0, opp_rack);
      }
    } else {
      // Nothing hooks here, return
      return;
    }
  } else {
    // Traverse the full length of the tiles on the board until hitting an empty
    // square
    while (1) {
      num_tiles_played_through++;
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
  printf("gen->current_left_col: %d\n", gen->current_left_col);
  shadow_play_left(gen, main_played_through_score, num_tiles_played_through,
                   perpendicular_additional_score, word_multiplier,
                   !gen->vertical, cross_set_index, opp_rack);
  shadow_play_right(gen, main_played_through_score, num_tiles_played_through,
                    perpendicular_additional_score, word_multiplier,
                    !gen->vertical, cross_set_index, opp_rack);
}

void shadow_play_for_anchor(Generator *gen, int col, Player *player,
                            Rack *opp_rack) {
  // set cols
  gen->current_left_col = col;
  gen->current_right_col = col;

  // Reset shadow equities
  gen->highest_shadow_equity = 0;
  memset(gen->highest_equity_by_length, 0,
         sizeof(gen->highest_equity_by_length));
  const struct ShadowLimit initial_shadow_limit = {0, -DBL_MAX};
  for (int i = 0; i <= RACK_SIZE; i++) {
    for (int j = 0; j <= RACK_SIZE; j++) {
      gen->shadow_limit_table[i][j] = initial_shadow_limit;
    }
  }

  // Set the number of letters
  gen->number_of_letters_on_rack = player->rack->number_of_letters;

  // Set the current anchor column
  gen->current_anchor_col = col;

  // Set the recorder type
  gen->move_sorting_type = player->strategy_params->move_sorting;

  // Reset tiles played
  gen->tiles_played = 0;

  gen->min_num_playthrough = BOARD_DIM - 1;
  gen->max_num_playthrough = 0;
  gen->min_tiles_to_play = 1;
  gen->max_tiles_to_play = 1;
  // Set rack cross set
  gen->rack_cross_set = 0;
  for (uint32_t i = 0; i < gen->letter_distribution->size; i++) {
    if (player->rack->array[i] > 0) {
      gen->rack_cross_set = gen->rack_cross_set | (1 << i);
    }
  }

  shadow_start(gen, get_cross_set_index(gen, player->index), opp_rack);
  add_anchor(gen->anchor_list, gen->current_row_index, col,
             gen->last_anchor_col, gen->board->transposed, gen->vertical,
             gen->min_num_playthrough, gen->max_num_playthrough,
             gen->min_tiles_to_play, gen->max_tiles_to_play,
             gen->max_tiles_starting_left_by, gen->highest_shadow_equity,
             gen->highest_equity_by_length, gen->shadow_limit_table);
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

void add_bingos(Generator *gen, Player *player, uint32_t node_index,
                int accepts) {
  /*
StringBuilder *sb = create_string_builder();
string_builder_add_rack(player->rack, gen->letter_distribution, sb);
printf(
"add_bingos %s tiles_played: %i node_index: accepts: %i\n",
string_builder_peek(sb), gen->tiles_played, node_index, accepts);
destroy_string_builder(sb);
*/
  if (gen->tiles_played == RACK_SIZE) {
    if (accepts) {
      memcpy(gen->rack_bingos[gen->number_of_bingos], gen->strip,
             sizeof(gen->strip));
      gen->number_of_bingos++;
    }
    return;
  }
  if (node_index == 0) {
    return;
  }
  for (int i = node_index;; i++) {
    const int ml = kwg_tile(player->strategy_params->kwg, i);
    const int new_node_index = kwg_arc_index(player->strategy_params->kwg, i);
    if ((player->rack->array[ml] != 0 || player->rack->array[0] != 0)) {
      int accepts = kwg_accepts(player->strategy_params->kwg, i);
      // Manipulating the array in the rack directly is a little bit dirty, and
      // doesn't update rack->number_of_letters or rack->empty, but those aren't
      // used here, and the original rack will be restored at the end.
      if (player->rack->array[ml] > 0) {
        player->rack->array[ml]--;
        gen->strip[gen->tiles_played] = ml;
        gen->tiles_played++;
        add_bingos(gen, player, new_node_index, accepts);
        gen->tiles_played--;
        player->rack->array[ml]++;
      } else if (player->rack->array[BLANK_MACHINE_LETTER] > 0) {
        player->rack->array[BLANK_MACHINE_LETTER]--;
        gen->strip[gen->tiles_played] = ml;
        gen->tiles_played++;
        add_bingos(gen, player, new_node_index, accepts);
        gen->tiles_played--;
        player->rack->array[BLANK_MACHINE_LETTER]++;
      }
    }
    if (kwg_is_end(player->strategy_params->kwg, i)) {
      break;
    }
  }
}

void look_up_bingos(Generator *gen, Player *player) {
  gen->tiles_played = 0;
  add_bingos(gen, player,
             kwg_get_dawg_root_node_index(player->strategy_params->kwg), false);
}

void split_anchors_for_bingos(AnchorList *anchor_list, int make_bingo_anchors) {
  printf("test_split_anchor_for_bingos\n");
  const int original_count = anchor_list->count;
  // For each anchor: if it
  //   1. can bingo (has max_tiles_to_play == RACKSIZE)
  //   2. never has playthrough tiles (max_num_playthrough == 0)
  // Change the existing anchor to be only for non-bingo plays,
  // and add a new anchor for bingo plays.
  //
  // Update comment, there's more going on now.

  for (int i = 0; i < original_count; i++) {
    Anchor *anchor = anchor_list->anchors[i];
    if ((anchor->min_num_playthrough == 0) &&
        (anchor->max_tiles_to_play == RACK_SIZE)) {
      anchor->max_tiles_to_play--;
      double best_nonbingo = 0;
      for (int len = 1; len < RACK_SIZE; len++) {
        if (anchor->highest_equity_by_length[len] > best_nonbingo) {
          best_nonbingo = anchor->highest_equity_by_length[len];
        }
      }

      anchor->highest_possible_equity = best_nonbingo;
      if (make_bingo_anchors) {
        double best_bingo = 0;
        int max_tiles_starting_left_by[(BOARD_DIM)];
        memset(max_tiles_starting_left_by, 0,
               sizeof(max_tiles_starting_left_by));
        for (int j = 0; j < (BOARD_DIM); j++) {
          printf("anchor->max_tiles_starting_left_by[%i]: %d\n", j,
                 anchor->max_tiles_starting_left_by[j]);
          printf("anchor->shadow_limit_table[%i][7].num_playthrough: %i\n", j,
                 anchor->shadow_limit_table[j][7].num_playthrough);
          if ((anchor->max_tiles_starting_left_by[j] == RACK_SIZE) &&
              (anchor->shadow_limit_table[j][7].num_playthrough == 0)) {
            const double best_equity =
                anchor->shadow_limit_table[j][7].highest_equity;
            printf("best_equity: %f\n", best_equity);
            if (best_equity > best_bingo) {
              best_bingo = best_equity;
            }
            max_tiles_starting_left_by[j] = RACK_SIZE;
          } else {
            max_tiles_starting_left_by[j] = 0;
          }
        }
        printf("best_bingo: %f\n", best_bingo);
        add_anchor(
            anchor_list, anchor->row, anchor->col, anchor->last_anchor_col,
            anchor->transpose_state, anchor->vertical, 0, 0, RACK_SIZE,
            RACK_SIZE, max_tiles_starting_left_by, best_bingo,
            anchor->highest_equity_by_length, anchor->shadow_limit_table);
        best_bingo = 0;
        memset(max_tiles_starting_left_by, 0,
               sizeof(max_tiles_starting_left_by));
        for (int j = 0; j < (BOARD_DIM); j++) {
          if ((anchor->max_tiles_starting_left_by[j] == RACK_SIZE) &&
              (anchor->shadow_limit_table[j][7].num_playthrough > 0)) {
            const double best_equity =
                anchor->shadow_limit_table[j][7].highest_equity;
            if (best_equity > best_bingo) {
              best_bingo = best_equity;
            }
            max_tiles_starting_left_by[j] = RACK_SIZE;
          } else {
            max_tiles_starting_left_by[j] = 0;
          }
        }
        if (anchor->max_num_playthrough > 0) {
          add_anchor(anchor_list, anchor->row, anchor->col,
                     anchor->last_anchor_col, anchor->transpose_state,
                     anchor->vertical, 1, anchor->max_num_playthrough,
                     RACK_SIZE, RACK_SIZE, max_tiles_starting_left_by,
                     best_bingo, anchor->highest_equity_by_length,
                     anchor->shadow_limit_table);
        }
      }
      for (int left_by = 0; left_by < (BOARD_DIM); left_by++) {
        if (anchor->max_tiles_starting_left_by[left_by] == RACK_SIZE) {
          anchor->max_tiles_starting_left_by[left_by]--;
        }
      }
    }
  }
}

void bingo_gen(Generator *gen, Player *player, Rack *opp_rack) {
  // printf("bingo_gen\n");
  const int row = gen->current_row_index;
  // const int dir = gen->vertical;
  const Board *board = gen->board;
  int leftmost_start_col = gen->current_anchor_col - (RACK_SIZE - 1);
  if (leftmost_start_col < 0) {
    leftmost_start_col = 0;
  }

  if (gen->last_anchor_col != INITIAL_LAST_ANCHOR_COL) {
    if ((leftmost_start_col <= gen->last_anchor_col + 1) &&
        (gen->last_anchor_col) >= 0 &&
        !is_empty(board, row, gen->last_anchor_col)) {
      // If the previous anchor included occupied tiles, we must leave a gap in
      // order to start a new word.
      leftmost_start_col = gen->last_anchor_col + 2;
    } else if (leftmost_start_col <= gen->last_anchor_col) {
      // If the previous anchor was only hooking rather than playing through
      // tiles, we can start on the next square.
      leftmost_start_col = gen->last_anchor_col + 1;
    }
  }

  const int leftmost_end_col = leftmost_start_col + (RACK_SIZE - 1);
  int rightmost_start_col = gen->current_anchor_col;
  int rightmost_end_col = rightmost_start_col + (RACK_SIZE - 1);
  if (rightmost_end_col >= BOARD_DIM) {
    const int overflow = rightmost_end_col - (BOARD_DIM - 1);
    rightmost_start_col -= overflow;
    rightmost_end_col -= overflow;
  }
  while (rightmost_start_col >= leftmost_start_col &&
         !is_empty(board, row, rightmost_end_col)) {
    rightmost_start_col--;
    rightmost_end_col--;
  }
  /*
  printf("bingo_gen: row=%d leftmost_start_col=%d leftmost_end_col=%d "
         "rightmost_start_col=%d rightmost_end_col=%d\n",
         row, leftmost_start_col, leftmost_end_col, rightmost_start_col,
         rightmost_end_col);
         */
  if (leftmost_start_col > rightmost_start_col) {
    return;
  }

  // Add up the total perpendicular score for each starting column.
  // Calculate it for the leftmost position, then adjust the rolling sum
  // for each subsequent position.
  int hook_totals[BOARD_DIM];

  // printf(
  //     "bingo_gen: row=%d dir=%d leftmost_start_col=%d
  //     rightmost_start_col=%d\n", row, dir, leftmost_start_col,
  //     rightmost_start_col);

  const int csi = get_cross_set_index(gen, player->index);
  int csd;
  if (gen->vertical) {
    csd = BOARD_HORIZONTAL_DIRECTION;
  } else {
    csd = BOARD_VERTICAL_DIRECTION;
  }
  hook_totals[leftmost_start_col] = 0;
  for (int col = leftmost_start_col; col <= leftmost_end_col; col++) {
    const uint8_t bonus_square = get_bonus_square(board, row, col);
    const int this_word_multiplier = bonus_square >> 4;
    const int cross_score = get_cross_score(board, row, col, csd, csi);
    hook_totals[leftmost_start_col] += cross_score * this_word_multiplier;
  }
  for (int col = leftmost_start_col + 1; col <= rightmost_start_col; col++) {
    const uint8_t old_bonus_square = get_bonus_square(board, row, col - 1);
    const int old_word_multiplier = old_bonus_square >> 4;
    const int old_cross_score = get_cross_score(board, row, col - 1, csd, csi);
    const int old_score = old_cross_score * old_word_multiplier;

    uint8_t new_bonus_square =
        get_bonus_square(board, row, col + RACK_SIZE - 1);
    const int new_word_multiplier = new_bonus_square >> 4;
    const int new_cross_score =
        get_cross_score(board, row, col + RACK_SIZE - 1, csd, csi);
    const int new_score = new_cross_score * new_word_multiplier;

    hook_totals[col] = hook_totals[col - 1] - old_score + new_score;
  }

  uint64_t cross_sets[BOARD_DIM];
  for (int col = leftmost_start_col; col <= rightmost_end_col; col++) {
    cross_sets[col] = get_cross_set(gen->board, row, col, csd, csi);
  }
  uint64_t letter_bits[RACK_SIZE];
  for (int bingo_idx = 0; bingo_idx < gen->number_of_bingos; bingo_idx++) {
    // StringBuilder *sb = create_string_builder();
    // for (int j = 0; j < RACK_SIZE; j++) {
    //   uint8_t print_tile = gen->rack_bingos[bingo_idx][j];
    //   string_builder_add_user_visible_letter(gen->letter_distribution,
    //                                          print_tile, 0, sb);
    // }
    // printf("trying to place bingo %s\n", string_builder_peek(sb));
    // destroy_string_builder(sb);
    for (int tile_idx = 0; tile_idx < RACK_SIZE; tile_idx++) {
      const uint8_t tile = gen->rack_bingos[bingo_idx][tile_idx];
      assert(!is_blanked(tile));
      letter_bits[tile_idx] = 1 << tile;
    }

    for (int start_col = leftmost_start_col; start_col <= rightmost_start_col;
         start_col++) {
      // printf("  start_col=%d hook_total=%d\n", start_col,
      //         hook_totals[start_col]);
      int end_col = start_col + (RACK_SIZE - 1);
      int fits_with_crosses = true;
      for (int col = start_col; col <= end_col; col++) {
        const int tile_idx = col - start_col;
        if ((cross_sets[col] & letter_bits[tile_idx]) == 0) {
          // printf("  tile %d doesn't fit in cross set %d\n", tile_idx, col);
          fits_with_crosses = false;
          break;
        }
      }
      if (!fits_with_crosses) {
        continue;
      }
      int word_multiplier = 1;
      int main_word_score = 0;
      int total_tile_crossing_score = 0;
      int tile_crossing_scores[RACK_SIZE];
      int tile_main_word_scores[RACK_SIZE];
      for (int col = start_col; col <= end_col; col++) {
        // printf("    col=%d\n", col);
        const int tile_idx = col - start_col;
        const uint8_t tile = gen->rack_bingos[bingo_idx][tile_idx];
        const uint8_t bonus_square = get_bonus_square(board, row, col);
        int this_word_multiplier = bonus_square >> 4;
        word_multiplier *= this_word_multiplier;
        const int tile_score = gen->letter_distribution->scores[tile];
        int actual_cross_word =
            (row > 0 && !is_empty(board, row - 1, col)) ||
            ((row < BOARD_DIM - 1) && !is_empty(board, row + 1, col));
        int letter_multiplier = bonus_square & 0x0F;
        int tile_crossing_score = 0;
        if (actual_cross_word) {
          tile_crossing_score =
              tile_score * this_word_multiplier * letter_multiplier;
          total_tile_crossing_score += tile_crossing_score;
        }
        const int tile_main_word_score = tile_score * letter_multiplier;
        tile_crossing_scores[tile_idx] = tile_crossing_score;
        // printf(
        //     "    tilescore=%d letter_multiplier=%d
        //     this_word_multiplier=%d\n",
        //     gen->letter_distribution->scores[tile], letter_multiplier,
        //     this_word_multiplier);
        tile_main_word_scores[tile_idx] = tile_main_word_score;
        main_word_score += tile_main_word_score;
        // printf("    main_word_score=%d\n", main_word_score);
      }
      const int play_score = main_word_score * word_multiplier +
                             total_tile_crossing_score +
                             hook_totals[start_col] + BINGO_BONUS;
      /*
      printf("  play_score=%d\n", play_score);
      for (int i = 0; i < RACK_SIZE; i++) {
        printf("tile_crossing_scores[%d]: %d, tile_main_word_scores[%d]: %d\n",
               i, tile_crossing_scores[i], i, tile_main_word_scores[i]);
      }
      */
      if (player->rack->array[BLANK_MACHINE_LETTER] == 0) {
        record_bingo(gen, player, opp_rack, row, start_col,
                     gen->rack_bingos[bingo_idx], play_score);
      } else {
        int bingo_letter_counts[MAX_ALPHABET_SIZE];
        memset(bingo_letter_counts, 0, sizeof(bingo_letter_counts));
        for (int tile_idx = 0; tile_idx < RACK_SIZE; tile_idx++) {
          const uint8_t tile = gen->rack_bingos[bingo_idx][tile_idx];
          bingo_letter_counts[tile]++;
        }
        if (player->rack->array[BLANK_MACHINE_LETTER] == 1) {
          uint8_t letter_needing_blank = 0;
          for (int tile_idx = 0; tile_idx < RACK_SIZE; tile_idx++) {
            const uint8_t tile = gen->rack_bingos[bingo_idx][tile_idx];
            if (bingo_letter_counts[tile] > player->rack->array[tile]) {
              letter_needing_blank = tile;
              break;
            }
          }
          assert(letter_needing_blank != 0);
          // printf("one blank: %d\n", letter_needing_blank);
          for (int tile_idx = 0; tile_idx < RACK_SIZE; tile_idx++) {
            const uint8_t tile = gen->rack_bingos[bingo_idx][tile_idx];
            if (tile == letter_needing_blank) {
              gen->rack_bingos[bingo_idx][tile_idx] =
                  get_blanked_machine_letter(tile);
              const int blanked_score =
                  play_score - tile_crossing_scores[tile_idx] -
                  tile_main_word_scores[tile_idx] * word_multiplier;
              record_bingo(gen, player, opp_rack, row, start_col,
                           gen->rack_bingos[bingo_idx], blanked_score);
              gen->rack_bingos[bingo_idx][tile_idx] = letter_needing_blank;
            }
          }
        } else {
          assert(player->rack->array[BLANK_MACHINE_LETTER] == 2);
          uint8_t letter_needing_blank = 0;
          uint8_t second_letter_needing_blank = 0;
          for (int tile_idx = 0; tile_idx < RACK_SIZE; tile_idx++) {
            const uint8_t tile = gen->rack_bingos[bingo_idx][tile_idx];
            const int deficit =
                bingo_letter_counts[tile] - player->rack->array[tile];
            assert(deficit <= 2);
            if (deficit == 2) {
              letter_needing_blank = tile;
              break;
            } else if (deficit == 1) {
              if (letter_needing_blank == 0) {
                letter_needing_blank = tile;
              } else if (letter_needing_blank != tile) {
                second_letter_needing_blank = tile;
                break;
              }
            }
          }
          if (second_letter_needing_blank == 0) {
            int blank_indices[RACK_SIZE];
            memset(blank_indices, 0, sizeof(blank_indices));
            int num_duplicates = 0;
            for (int tile_idx = 0; tile_idx < RACK_SIZE; tile_idx++) {
              const uint8_t tile = gen->rack_bingos[bingo_idx][tile_idx];
              if (tile == letter_needing_blank) {
                blank_indices[num_duplicates] = tile_idx;
                num_duplicates++;
              }
            }
            assert(num_duplicates >= 2);
            for (int dup_idx1 = 0; dup_idx1 < num_duplicates; dup_idx1++) {
              const int tile_idx1 = blank_indices[dup_idx1];
              gen->rack_bingos[bingo_idx][tile_idx1] =
                  get_blanked_machine_letter(letter_needing_blank);
              const int blanked1 =
                  play_score - tile_crossing_scores[tile_idx1] -
                  tile_main_word_scores[tile_idx1] * word_multiplier;
              for (int dup_idx2 = dup_idx1 + 1; dup_idx2 < num_duplicates;
                   dup_idx2++) {
                const int tile_idx2 = blank_indices[dup_idx2];
                gen->rack_bingos[bingo_idx][tile_idx2] =
                    get_blanked_machine_letter(letter_needing_blank);
                const int blanked2 =
                    blanked1 - tile_crossing_scores[tile_idx2] -
                    tile_main_word_scores[tile_idx2] * word_multiplier;
                record_bingo(gen, player, opp_rack, row, start_col,
                             gen->rack_bingos[bingo_idx], blanked2);
                gen->rack_bingos[bingo_idx][tile_idx2] = letter_needing_blank;
              }
              gen->rack_bingos[bingo_idx][tile_idx1] = letter_needing_blank;
            }
          } else {
            int blank1_indices[RACK_SIZE];
            int blank1_dups = 0;
            int blank2_indices[RACK_SIZE];
            int blank2_dups = 0;
            for (int tile_idx = 0; tile_idx < RACK_SIZE; tile_idx++) {
              const uint8_t tile = gen->rack_bingos[bingo_idx][tile_idx];
              if (tile == letter_needing_blank) {
                blank1_indices[blank1_dups] = tile_idx;
                blank1_dups++;
              } else if (tile == second_letter_needing_blank) {
                blank2_indices[blank2_dups] = tile_idx;
                blank2_dups++;
              }
            }
            /*
            printf("blank1_dups: %d\n", blank1_dups);
            for (int i = 0; i < blank1_dups; i++) {
              printf("  %d\n", blank1_indices[i]);
            }
            printf("blank2_dups: %d\n", blank2_dups);
            for (int i = 0; i < blank2_dups; i++) {
              printf("  %d\n", blank2_indices[i]);
            }
            */
            for (int blank1_dup_idx = 0; blank1_dup_idx < blank1_dups;
                 blank1_dup_idx++) {
              const int tile_idx1 = blank1_indices[blank1_dup_idx];
              gen->rack_bingos[bingo_idx][tile_idx1] =
                  get_blanked_machine_letter(letter_needing_blank);
              const int blanked1 =
                  play_score - tile_crossing_scores[tile_idx1] -
                  tile_main_word_scores[tile_idx1] * word_multiplier;
              for (int blank2_dup_idx = 0; blank2_dup_idx < blank2_dups;
                   blank2_dup_idx++) {
                const int tile_idx2 = blank2_indices[blank2_dup_idx];
                gen->rack_bingos[bingo_idx][tile_idx2] =
                    get_blanked_machine_letter(second_letter_needing_blank);
                const int blanked2 =
                    blanked1 - tile_crossing_scores[tile_idx2] -
                    tile_main_word_scores[tile_idx2] * word_multiplier;
                record_bingo(gen, player, opp_rack, row, start_col,
                             gen->rack_bingos[bingo_idx], blanked2);
                gen->rack_bingos[bingo_idx][tile_idx2] =
                    second_letter_needing_blank;
              }
              gen->rack_bingos[bingo_idx][tile_idx1] = letter_needing_blank;
            }
          }
        }
      }
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
    shadow_by_orientation(gen, player, dir, opp_rack);
    transpose(gen->board);
  }

  // int do_bingo_gen = player->index == 1;  // DO NOT SUBMIT
  int do_bingo_gen = true;  // player->index == 1;

  /*
    StringBuilder *sb = create_string_builder();
    string_builder_add_rack(player->rack, gen->letter_distribution, sb);
  */
  // uint64_t start_time;
  // uint64_t end_time;

  gen->number_of_bingos = 0;
  if (do_bingo_gen) {
    // start_time = __rdtsc();
    look_up_bingos(gen, player);
    // end_time = __rdtsc();
    /*
        printf("rack: %s, number_of_bingos: %d, look_up_bingos time: %lluns\n",
               string_builder_peek(sb), gen->number_of_bingos,
               end_time - start_time);
    */
  }

  /*
   for (int i = 0; i < gen->number_of_bingos; i++) {
     StringBuilder *sb = create_string_builder();
     for (int j = 0; j < RACK_SIZE; j++) {
       uint8_t print_tile = gen->rack_bingos[i][j];
       string_builder_add_user_visible_letter(gen->letter_distribution,
                                              print_tile, 0, sb);
     }
     printf("bingo: %s\n", string_builder_peek(sb));
     destroy_string_builder(sb);
   }
   */
  // Reset the reused generator fields
  gen->tiles_played = 0;

  if (do_bingo_gen) {
    int make_bingo_anchors = gen->number_of_bingos > 0;
    split_anchors_for_bingos(gen->anchor_list, make_bingo_anchors);
  }
  sort_anchor_list(gen->anchor_list);

  // uint64_t total_anchor_search_time = 0;
  // int anchors_searched = 0;
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
    gen->min_num_playthrough =
        gen->anchor_list->anchors[i]->min_num_playthrough;
    gen->max_num_playthrough =
        gen->anchor_list->anchors[i]->max_num_playthrough;
    gen->min_tiles_to_play = gen->anchor_list->anchors[i]->min_tiles_to_play;
    gen->max_tiles_to_play = gen->anchor_list->anchors[i]->max_tiles_to_play;
    gen->highest_shadow_equity =
        gen->anchor_list->anchors[i]->highest_possible_equity;
    memcpy(gen->highest_equity_by_length,
           gen->anchor_list->anchors[i]->highest_equity_by_length,
           sizeof(gen->highest_equity_by_length));
    for (int len = gen->max_tiles_to_play; len > 0; len--) {
      for (int shorter_len = len - 1; shorter_len >= 0; shorter_len--) {
        if (gen->highest_equity_by_length[shorter_len] <
            gen->highest_equity_by_length[len]) {
          gen->highest_equity_by_length[shorter_len] =
              gen->highest_equity_by_length[len];
        }
      }
    }
    set_transpose(gen->board, gen->anchor_list->anchors[i]->transpose_state);
    load_row_letter_cache(gen, gen->current_row_index);
    if (do_bingo_gen && (gen->min_tiles_to_play == RACK_SIZE) &&
        (gen->anchor_list->anchors[i]->max_num_playthrough == 0)) {
      // start_time = __rdtsc();
      bingo_gen(gen, player, opp_rack);
      // end_time = __rdtsc();
    } else {
      // start_time = __rdtsc();
      recursive_gen(gen, gen->current_anchor_col, player, opp_rack,
                    kwg_get_root_node_index(player->strategy_params->kwg),
                    gen->current_anchor_col, gen->current_anchor_col,
                    !gen->vertical);
      // end_time = __rdtsc();
    }
    // const uint64_t elapsed_time = end_time - start_time;
    // total_anchor_search_time += elapsed_time;
    /*
        printf(
            "player: %i, rack: %s, i: %i row: %i, col: %i, vert: %i, time: "
            "%lluns, "
            "moves[0].equity: %f, "
            "highest_equity: %f, "
            "max_playthrough: %i, "
            "min_tiles: %i, "
            "max_tiles: %i\n",
            player->index, string_builder_peek(sb), i, gen->current_row_index,
            gen->current_anchor_col, gen->vertical, elapsed_time,
            gen->move_list->moves[0]->equity, gen->highest_shadow_equity,
            gen->max_num_playthrough, gen->min_tiles_to_play,
            gen->max_tiles_to_play);
    */
    // anchors_searched++;
    if (player->strategy_params->play_recorder_type == MOVE_RECORDER_BEST) {
      // If a better play has been found than should have been possible for this
      // anchor, highest_possible_equity was invalid.
      assert(!better_play_has_been_found(
          gen, gen->anchor_list->anchors[i]->highest_possible_equity));
    }
  }
  /*
  printf("player: %i rack: %s time: %lluns anchors searched: %d\n",
         player->index, string_builder_peek(sb), total_anchor_search_time,
         anchors_searched);
  destroy_string_builder(sb);
  */
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