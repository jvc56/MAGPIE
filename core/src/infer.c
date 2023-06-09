#include <stdio.h>
#include <stdlib.h>

#include "bag.h"
#include "game.h"
#include "infer.h"
#include "kwg.h"
#include "klv.h"
#include "leave_rack.h"
#include "move.h"
#include "rack.h"
#include "stats.h"

#include "../test/game_print.h"
#include "../test/move_print.h"
#include "../test/inference_print.h"
#include "../test/test_util.h"

InferenceRecord * create_inference_record(int distribution_size, int draw_and_leave_subtotals_size) {
    InferenceRecord * record = malloc(sizeof(InferenceRecord));
    record->rack_leave = create_rack(distribution_size);
    record->draw_and_leave_subtotals = (int *) malloc(draw_and_leave_subtotals_size*sizeof(int));
    record->equity_values = create_stat();
    record->leave_rack_list = create_leave_rack_list(20, distribution_size);
    return record;
}

void destroy_inference_record(InferenceRecord * record) {
    destroy_rack(record->rack_leave);
    destroy_stat(record->equity_values);
    destroy_leave_rack_list(record->leave_rack_list);
    free(record->draw_and_leave_subtotals);
    free(record);
}

Inference * create_inference(int distribution_size) {
    Inference * inference =  malloc(sizeof(Inference));
    inference->distribution_size = distribution_size;
    inference->draw_and_leave_subtotals_size = distribution_size*(RACK_SIZE)*2;
    inference->bag_as_rack = create_rack(distribution_size);
    inference->leave_record = create_inference_record(distribution_size, inference->draw_and_leave_subtotals_size);
    inference->exchanged_record = create_inference_record(distribution_size, inference->draw_and_leave_subtotals_size);
    inference->rack_record = create_inference_record(distribution_size, inference->draw_and_leave_subtotals_size);
    return inference;
}

void destroy_inference(Inference * inference) {
    destroy_rack(inference->bag_as_rack);
    destroy_inference_record(inference->leave_record);
    destroy_inference_record(inference->exchanged_record);
    destroy_inference_record(inference->rack_record);
    free(inference);
}

// Functions for the inference record

int get_letter_subtotal_index(uint8_t letter, int number_of_letters, int subtotal_index_offset) {
    return (letter * 2 * (RACK_SIZE)) + ((number_of_letters - 1) * 2) + subtotal_index_offset;
}

int get_subtotal(InferenceRecord * record, uint8_t letter, int number_of_letters, int subtotal_index_offset) {
    return record->draw_and_leave_subtotals[get_letter_subtotal_index(letter, number_of_letters, subtotal_index_offset)];
}

void add_to_letter_subtotal(InferenceRecord * record, uint8_t letter, int number_of_letters, int subtotal_index_offset, int delta) {
    record->draw_and_leave_subtotals[get_letter_subtotal_index(letter, number_of_letters, subtotal_index_offset)] += delta;
}

int get_subtotal_sum_with_minimum(InferenceRecord * record, uint8_t letter, int minimum_number_of_letters, int subtotal_index_offset) {
    int sum = 0;
    for (int i = minimum_number_of_letters; i <= (RACK_SIZE); i++) {
        sum += get_subtotal(record, letter, i, subtotal_index_offset);
    }
    return sum;
}

int choose(int n, int k) {
    if (n < k) {
        return 0;
    }
    if (k == 0) {
        return 1;
    }
    return (n * choose(n - 1, k - 1)) / k;
}

int get_number_of_draws_for_rack(Rack * bag_as_rack, Rack * rack) {
    int number_of_ways = 1;
    for (int i = 0; i < rack->array_size; i++) {
        if (rack->array[i] > 0) {
            number_of_ways *= choose(bag_as_rack->array[i] + rack->array[i], rack->array[i]);
        }
    }
    return number_of_ways;
}

void get_stat_for_letter(InferenceRecord * record, Stat * stat, uint8_t letter) {
    reset_stat(stat);
    for (int i = 1; i <= (RACK_SIZE); i++) {
        int number_of_draws_with_exactly_i_of_letter = get_subtotal(record, letter, i, INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW);
        if (number_of_draws_with_exactly_i_of_letter > 0) {
            push(stat, i, number_of_draws_with_exactly_i_of_letter);
        }
    }
    // Add the zero case to the stat
    // We do not have direct stats for when the letter
    // was never drawn so we infer it here
    uint64_t number_of_draws_without_letter = weight(record->equity_values) - weight(stat);
    push(stat, 0, number_of_draws_without_letter);
}

double get_probability_for_random_minimum_draw(Rack * bag_as_rack, Rack * rack, uint8_t this_letter, int minimum, int number_of_actual_tiles_played) {
    int number_of_this_letters_already_on_rack = rack->array[this_letter];
    int minimum_adjusted_for_partial_rack = minimum - number_of_this_letters_already_on_rack;
    // If the partial leave already has the minimum
    // number of letters, the probability is trivially 1.
    if (minimum_adjusted_for_partial_rack <= 0) {
        return 1;
    }
    int total_number_of_letters_in_bag = bag_as_rack->number_of_letters;
    int total_number_of_letters_on_rack = rack->number_of_letters;
    int number_of_this_letter_in_bag = bag_as_rack->array[this_letter];

    // If there are not enough letters to meet the minimum, the probability
    // is trivially 0.
    if (number_of_this_letter_in_bag < minimum_adjusted_for_partial_rack) {
        return 0;
    }

    int total_number_of_letters_to_draw = (RACK_SIZE) - (total_number_of_letters_on_rack + number_of_actual_tiles_played);

    // If the player is emptying the bag and there are the minimum
    // number of leaves remaining, the probability is trivially 1.
    if (total_number_of_letters_in_bag <= total_number_of_letters_to_draw &&
       number_of_this_letter_in_bag >= minimum_adjusted_for_partial_rack) {
        return 1;
    }

    int total_draws = choose(total_number_of_letters_in_bag, total_number_of_letters_to_draw);
    int number_of_other_letters_in_bag = total_number_of_letters_in_bag - number_of_this_letter_in_bag;
    int total_draws_for_this_letter_minimum = 0;
    for (int i = minimum_adjusted_for_partial_rack; i <= total_number_of_letters_to_draw; i++) {
        total_draws_for_this_letter_minimum += choose(number_of_this_letter_in_bag, i) *
        choose(number_of_other_letters_in_bag, total_number_of_letters_to_draw - i);
    }

    return ((double)total_draws_for_this_letter_minimum)/total_draws;
}

void record_valid_leave(InferenceRecord * record, Rack * bag_as_rack, Rack * rack, float current_leave_value, int distribution_size) {
    int number_of_draws_for_leave = get_number_of_draws_for_rack(bag_as_rack, rack);
    push(record->equity_values, (double) current_leave_value, number_of_draws_for_leave);
    insert_leave_rack(record->leave_rack_list, rack, number_of_draws_for_leave, current_leave_value);
    for (int i = 0; i < distribution_size; i++) {
        if (rack->array[i] > 0) {
            add_to_letter_subtotal(record, i, rack->array[i], INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW, number_of_draws_for_leave);
            add_to_letter_subtotal(record, i, rack->array[i], INFERENCE_SUBTOTAL_INDEX_OFFSET_LEAVE, 1);
        }
    }
}

void record_tiles_in_bag_as_player_leave(Inference * inference, Rack * actual_tiles_played) {
    push(inference->leave_record->equity_values, (double) get_leave_value(inference->klv, inference->leave_record->rack_leave), 1);
    for (int i = 0; i < inference->distribution_size; i++) {
        int number_of_remaining_letters = inference->bag_as_rack->array[i] - actual_tiles_played->array[i];
        if (number_of_remaining_letters > 0) {
            add_to_letter_subtotal(inference->leave_record, i, number_of_remaining_letters, INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW, 1);
            add_to_letter_subtotal(inference->leave_record, i, number_of_remaining_letters, INFERENCE_SUBTOTAL_INDEX_OFFSET_LEAVE, 1);
        }
    }
}

Move * get_top_move(Inference * inference) {
    Game * game = inference->game;
    int original_recorder_type = game->players[inference->player_to_infer_index]->strategy_params->play_recorder_type;
    int original_apply_placement_adjustment = game->gen->apply_placement_adjustment;
    game->gen->apply_placement_adjustment = 0;
    game->players[inference->player_to_infer_index]->strategy_params->play_recorder_type = PLAY_RECORDER_TYPE_TOP_EQUITY;
    reset_move_list(game->gen->move_list);
    generate_moves(game->gen, game->players[inference->player_to_infer_index], game->players[1 - inference->player_to_infer_index]->rack, game->gen->bag->last_tile_index + 1 >= RACK_SIZE);
    game->players[inference->player_to_infer_index]->strategy_params->play_recorder_type = original_recorder_type;
    game->gen->apply_placement_adjustment = original_apply_placement_adjustment;
    return game->gen->move_list->moves[0];
}

int within_equity_margin_or_matching_exchange(Inference * inference, float current_leave_value) {
    Move * top_move = get_top_move(inference);
    int is_within_equity_margin = inference->actual_score + current_leave_value + inference->equity_margin + (INFERENCE_EQUITY_EPSILON) >= top_move->equity;
    int number_exchanged_matches = top_move->move_type == MOVE_TYPE_EXCHANGE && top_move->tiles_played == inference->number_of_tiles_exchanged;
    return is_within_equity_margin || number_exchanged_matches;
}

void evaluate_possible_leave(Inference * inference, float current_leave_value) {
    if (within_equity_margin_or_matching_exchange(inference, current_leave_value) || inference->bag_as_rack->empty) {
        record_valid_leave(inference->leave_record, inference->bag_as_rack, inference->leave_record->rack_leave, current_leave_value, inference->distribution_size);
    }
}

void increment_letter_for_inference(Inference * inference, uint8_t letter) {
    take_letter_from_rack(inference->bag_as_rack, letter);
    add_letter_to_rack(inference->player_to_infer_rack, letter);
    add_letter_to_rack(inference->leave_record->rack_leave, letter);
}

void decrement_letter_for_inference(Inference * inference, uint8_t letter) {
    add_letter_to_rack(inference->bag_as_rack, letter);
    take_letter_from_rack(inference->player_to_infer_rack, letter);
    take_letter_from_rack(inference->leave_record->rack_leave, letter);
}

void iterate_through_all_possible_leaves(Inference * inference, int leave_tiles_remaining, int start_letter) {
    if (leave_tiles_remaining == 0) {
        double current_leave_value = 0;
        if (inference->number_of_tiles_exchanged == 0) {
            current_leave_value = get_leave_value(inference->klv, inference->leave_record->rack_leave);
        }
        evaluate_possible_leave(inference, current_leave_value);
        return;
    }
	for (int letter = start_letter; letter < inference->distribution_size; letter++) {
        if (inference->bag_as_rack->array[letter] > 0) {
            increment_letter_for_inference(inference, letter);
            iterate_through_all_possible_leaves(inference, leave_tiles_remaining - 1, letter);
            decrement_letter_for_inference(inference, letter);
        }
    }
    reset_move_list(inference->game->gen->move_list);
}

void initialize_inference_record_for_evaluation(InferenceRecord * record, int draw_and_leave_subtotals_size) {
    // Reset record
    reset_stat(record->equity_values);
    for (int i = 0; i < draw_and_leave_subtotals_size; i++) {
        record->draw_and_leave_subtotals[i] = 0;
    }
    reset_leave_rack_list(record->leave_rack_list);
}

void initialize_inference_for_evaluation(Inference * inference, Game * game, Rack * actual_tiles_played, int player_to_infer_index, int actual_score, int number_of_tiles_exchanged, float equity_margin) {    
    initialize_inference_record_for_evaluation(inference->leave_record, inference->draw_and_leave_subtotals_size);
    initialize_inference_record_for_evaluation(inference->exchanged_record, inference->draw_and_leave_subtotals_size);
    initialize_inference_record_for_evaluation(inference->rack_record, inference->draw_and_leave_subtotals_size);

    inference->game = game;
    inference->actual_score = actual_score;
    inference->number_of_tiles_exchanged = number_of_tiles_exchanged;
    inference->equity_margin = equity_margin;

    inference->player_to_infer_index = player_to_infer_index;
    inference->klv = game->players[player_to_infer_index]->strategy_params->klv;
    inference->player_to_infer_rack = game->players[player_to_infer_index]->rack;

    reset_rack(inference->bag_as_rack);
    reset_rack(inference->leave_record->rack_leave);

    // Create the bag as a rack
    for (int i = 0; i <= game->gen->bag->last_tile_index; i++) {
        add_letter_to_rack(inference->bag_as_rack, game->gen->bag->tiles[i]);
    }

    // Add any existing tiles on the player's rack
    // to the player's leave for partial inferences
    for (int i = 0; i < inference->player_to_infer_rack->array_size; i++) {
        for (int j = 0; j < inference->player_to_infer_rack->array[i]; j++) {
            add_letter_to_rack(inference->leave_record->rack_leave, i);
        }
    }

    // Remove the tiles played in the move from the game bag
    // and add them to the player's rack
    for (int i = 0; i < actual_tiles_played->array_size; i++) {
        for (int j = 0; j < actual_tiles_played->array[i]; j++) {
            take_letter_from_rack(inference->bag_as_rack, i);
            add_letter_to_rack(inference->player_to_infer_rack, i);
        }
    }
}

int infer(Inference * inference, Game * game, Rack * actual_tiles_played, int player_to_infer_index, int actual_score, int number_of_tiles_exchanged, float equity_margin) {
    initialize_inference_for_evaluation(inference, game, actual_tiles_played, player_to_infer_index, actual_score, number_of_tiles_exchanged, equity_margin);

    for (int i = 0; i < inference->distribution_size; i++) {
        if (inference->bag_as_rack->array[i] < 0) {
            return INFERENCE_STATUS_TILES_PLAYED_NOT_IN_BAG;
        }
    }

    if (actual_tiles_played->number_of_letters == 0 && number_of_tiles_exchanged == 0) {
        return INFERENCE_STATUS_NO_TILES_PLAYED;
    }

    if (actual_tiles_played->number_of_letters != 0 && number_of_tiles_exchanged != 0) {
        return INFERENCE_STATUS_BOTH_PLAY_AND_EXCHANGE;
    }

    if (number_of_tiles_exchanged != 0 && inference->bag_as_rack->number_of_letters < (RACK_SIZE) * 2) {
        return INFERENCE_STATUS_EXCHANGE_NOT_ALLOWED;
    }

    if (number_of_tiles_exchanged != 0 && actual_score != 0) {
        return INFERENCE_STATUS_EXCHANGE_SCORE_NOT_ZERO;
    }

    if (game->players[player_to_infer_index]->rack->number_of_letters > (RACK_SIZE)) {
        return INFERENCE_STATUS_RACK_OVERFLOW;
    }

    iterate_through_all_possible_leaves(inference, (RACK_SIZE) - inference->player_to_infer_rack->number_of_letters, BLANK_MACHINE_LETTER);

    print_inference(inference, actual_tiles_played);
    reset_rack(inference->player_to_infer_rack);
    return INFERENCE_STATUS_SUCCESS;
}