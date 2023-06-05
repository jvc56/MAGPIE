#include <stdio.h>
#include <stdlib.h>

#include "bag.h"
#include "game.h"
#include "infer.h"
#include "kwg.h"
#include "klv.h"
#include "move.h"
#include "rack.h"
#include "stats.h"

#include "../test/game_print.h"
#include "../test/inference_print.h"
#include "../test/test_util.h"

Inference * create_inference(int distribution_size) {
    Inference * inference =  malloc(sizeof(Inference));
    inference->distribution_size = distribution_size;
    inference->draw_and_leave_subtotals_size = distribution_size*(RACK_SIZE)*2;
    inference->draw_and_leave_subtotals = (int *) malloc(inference->draw_and_leave_subtotals_size*sizeof(int));
    inference->player_leave = create_rack(distribution_size);
    inference->bag_as_rack = create_rack(distribution_size);
    inference->leave_values = create_stat();
    return inference;
}

void destroy_inference(Inference * inference) {
    destroy_rack(inference->player_leave);
    destroy_rack(inference->bag_as_rack);
    destroy_stat(inference->leave_values);
    free(inference->draw_and_leave_subtotals);
    free(inference);
}

// Functions for the inference record

int get_letter_subtotal_index(uint8_t letter, int number_of_letters, int subtotal_index_offset) {
    return (letter * 2 * (RACK_SIZE)) + ((number_of_letters - 1) * 2) + subtotal_index_offset;
}

int get_subtotal(Inference * inference, uint8_t letter, int number_of_letters, int subtotal_index_offset) {
    return inference->draw_and_leave_subtotals[get_letter_subtotal_index(letter, number_of_letters, subtotal_index_offset)];
}

void add_to_letter_subtotal(Inference * inference, uint8_t letter, int number_of_letters, int subtotal_index_offset, int delta) {
    inference->draw_and_leave_subtotals[get_letter_subtotal_index(letter, number_of_letters, subtotal_index_offset)] += delta;
}

int get_subtotal_sum_with_minimum(Inference * inference, uint8_t letter, int minimum_number_of_letters, int subtotal_index_offset) {
    int sum = 0;
    for (int i = minimum_number_of_letters; i <= (RACK_SIZE); i++) {
        sum += get_subtotal(inference, letter, i, subtotal_index_offset);
    }
    return sum;
}

int choose(int n, int k) {
    if (k == 0) {
        return 1;
    }
    return (n * choose(n - 1, k - 1)) / k;
}

int get_number_of_draws_for_leave(Inference * inference) {
    int number_of_ways = 1;
    for (int i = 0; i < inference->player_leave->array_size; i++) {
        if (inference->player_leave->array[i] > 0) {
            number_of_ways *= choose(inference->bag_as_rack->array[i] + inference->player_leave->array[i], inference->player_leave->array[i]);
        }
    }
    return number_of_ways;
}

void get_stat_for_letter(Inference * inference, Stat * stat, uint8_t letter) {
    reset_stat(stat);
    for (int i = 1; i <= (RACK_SIZE); i++) {
        int number_of_draws_with_exactly_i_of_letter = get_subtotal(inference, letter, i, INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW);
        if (number_of_draws_with_exactly_i_of_letter > 0) {
            push(stat, i, number_of_draws_with_exactly_i_of_letter);
        }
    }
    // Add the zero case to the stat
    // We do not have direct stats for when the letter
    // was never drawn so we infer it here
    uint64_t number_of_draws_without_letter = weight(inference->leave_values) - weight(stat);
    push(stat, 0, number_of_draws_without_letter);
}

double get_probability_for_random_minimum_draw(Inference * inference, uint8_t letter, int minimum) {
    return 0.0;
}

void record_valid_leave(Inference * inference, float current_leave_value) {
    int number_of_draws_for_leave = get_number_of_draws_for_leave(inference);
    push(inference->leave_values, (double) current_leave_value, number_of_draws_for_leave);
    for (int i = 0; i < inference->distribution_size; i++) {
        if (inference->player_leave->array[i] > 0) {
            add_to_letter_subtotal(inference, i, inference->player_leave->array[i], INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW, number_of_draws_for_leave);
            add_to_letter_subtotal(inference, i, inference->player_leave->array[i], INFERENCE_SUBTOTAL_INDEX_OFFSET_LEAVE, 1);
        }
    }
}

void set_inference_to_remaining_tiles(Inference * inference, Rack * actual_tiles_played) {
    push(inference->leave_values, (double) get_leave_value(inference->klv, inference->player_leave), 1);
    for (int i = 0; i < inference->distribution_size; i++) {
        int number_of_remaining_letters = inference->bag_as_rack->array[i] - actual_tiles_played->array[i];
        if (number_of_remaining_letters > 0) {
            add_to_letter_subtotal(inference, i, number_of_remaining_letters, INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW, 1);
            add_to_letter_subtotal(inference, i, number_of_remaining_letters, INFERENCE_SUBTOTAL_INDEX_OFFSET_LEAVE, 1);
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

int within_equity_margin(Inference * inference, float current_leave_value) {
    Move * top_move = get_top_move(inference);
    return inference->actual_score + current_leave_value + inference->equity_margin + (INFERENCE_EQUITY_EPSILON) >= top_move->equity;
}

void evaluate_possible_leave(Inference * inference, float current_leave_value) {
    if (within_equity_margin(inference, current_leave_value) || inference->bag_as_rack->empty) {
        record_valid_leave(inference, current_leave_value);
    }
}

void increment_letter_for_inference(Inference * inference, uint8_t letter) {
    take_letter_from_rack(inference->bag_as_rack, letter);
    add_letter_to_rack(inference->player_to_infer_rack, letter);
    add_letter_to_rack(inference->player_leave, letter);
}

void decrement_letter_for_inference(Inference * inference, uint8_t letter) {
    add_letter_to_rack(inference->bag_as_rack, letter);
    take_letter_from_rack(inference->player_to_infer_rack, letter);
    take_letter_from_rack(inference->player_leave, letter);
}

void iterate_through_all_possible_leaves(Inference * inference, int leave_tiles_remaining, int start_letter) {
    if (leave_tiles_remaining == 0) {
        evaluate_possible_leave(inference, get_leave_value(inference->klv, inference->player_leave));
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

void initialize_inference_for_evaluation(Inference * inference, Game * game, Rack * actual_tiles_played, int player_to_infer_index, int actual_score, float equity_margin) {    
    // Reset record
    reset_stat(inference->leave_values);
    for (int i = 0; i < inference->draw_and_leave_subtotals_size; i++) {
        inference->draw_and_leave_subtotals[i] = 0;
    }

    inference->game = game;
    inference->actual_score = actual_score;
    inference->equity_margin = equity_margin;

    inference->player_to_infer_index = player_to_infer_index;
    inference->klv = game->players[player_to_infer_index]->strategy_params->klv;
    inference->player_to_infer_rack = game->players[player_to_infer_index]->rack;

    reset_rack(inference->bag_as_rack);
    reset_rack(inference->player_leave);

    // Create the bag as a rack
    for (int i = 0; i <= game->gen->bag->last_tile_index; i++) {
        add_letter_to_rack(inference->bag_as_rack, game->gen->bag->tiles[i]);
    }

    // Add any existing tiles on the player's rack
    // to the player's leave for partial inferences
    for (int i = 0; i < inference->player_to_infer_rack->array_size; i++) {
        for (int j = 0; j < inference->player_to_infer_rack->array[i]; j++) {
            add_letter_to_rack(inference->player_leave, i);
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

int infer(Inference * inference, Game * game, Rack * actual_tiles_played, int player_to_infer_index, int actual_score, float equity_margin) {
    initialize_inference_for_evaluation(inference, game, actual_tiles_played, player_to_infer_index, actual_score, equity_margin);

    for (int i = 0; i < inference->distribution_size; i++) {
        if (inference->bag_as_rack->array[i] < 0) {
            return INFERENCE_STATUS_TILES_PLAYED_NOT_IN_BAG;
        }
    }

    if (actual_tiles_played->number_of_letters == 0) {
        return INFERENCE_STATUS_NO_TILES_PLAYED;
    }

    if (game->players[player_to_infer_index]->rack->number_of_letters > (RACK_SIZE)) {
        return INFERENCE_STATUS_RACK_OVERFLOW;
    }

    iterate_through_all_possible_leaves(inference, (RACK_SIZE) - inference->player_to_infer_rack->number_of_letters, BLANK_MACHINE_LETTER);

    reset_rack(inference->player_to_infer_rack);
    print_inference(inference, actual_tiles_played);
    return INFERENCE_STATUS_SUCCESS;
}