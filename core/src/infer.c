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

InferenceRecord * create_inference_record(int draw_and_leave_subtotals_size) {
    InferenceRecord * record = malloc(sizeof(InferenceRecord));
    record->draw_and_leave_subtotals = (int *) malloc(draw_and_leave_subtotals_size*sizeof(int));
    record->equity_values = create_stat();
    return record;
}

void destroy_inference_record(InferenceRecord * record) {
    destroy_stat(record->equity_values);
    free(record->draw_and_leave_subtotals);
    free(record);
}

Inference * create_inference(int distribution_size) {
    Inference * inference =  malloc(sizeof(Inference));
    inference->distribution_size = distribution_size;
    inference->draw_and_leave_subtotals_size = distribution_size*(RACK_SIZE)*2;
    inference->bag_as_rack = create_rack(distribution_size);
    inference->leave = create_rack(distribution_size);
    inference->exchanged = create_rack(distribution_size);
    inference->leave_record = create_inference_record(inference->draw_and_leave_subtotals_size);
    inference->exchanged_record = create_inference_record(inference->draw_and_leave_subtotals_size);
    inference->rack_record = create_inference_record(inference->draw_and_leave_subtotals_size);
    inference->leave_rack_list = create_leave_rack_list(20, distribution_size);
    return inference;
}

void destroy_inference(Inference * inference) {
    destroy_rack(inference->bag_as_rack);
    destroy_rack(inference->leave);
    destroy_rack(inference->exchanged);
    destroy_leave_rack_list(inference->leave_rack_list);
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

void increment_subtotals_for_record(InferenceRecord * record, Rack * rack, int number_of_draws_for_leave) {
    for (int i = 0; i < rack->array_size; i++) {
        if (rack->array[i] > 0) {
            add_to_letter_subtotal(record, i, rack->array[i], INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW, number_of_draws_for_leave);
            add_to_letter_subtotal(record, i, rack->array[i], INFERENCE_SUBTOTAL_INDEX_OFFSET_LEAVE, 1);
        }
    }
}

void record_valid_leave(InferenceRecord * record, Rack * rack, float current_leave_value, int number_of_draws_for_leave) {
    push(record->equity_values, (double) current_leave_value, number_of_draws_for_leave);
    increment_subtotals_for_record(record, rack, number_of_draws_for_leave);
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

void evaluate_possible_leave(Inference * inference, float current_leave_value) {
    Move * top_move = get_top_move(inference);
    int is_within_equity_margin = inference->actual_score + current_leave_value + inference->equity_margin + (INFERENCE_EQUITY_EPSILON) >= top_move->equity;
    int number_exchanged_matches = top_move->move_type == MOVE_TYPE_EXCHANGE && top_move->tiles_played == inference->number_of_tiles_exchanged;
    int recordable = is_within_equity_margin || number_exchanged_matches || inference->bag_as_rack->empty;
    if (recordable) {
        int number_of_draws_for_leave = get_number_of_draws_for_rack(inference->bag_as_rack, inference->leave);
        if (inference->number_of_tiles_exchanged > 0) {
            record_valid_leave(inference->rack_record, inference->leave, current_leave_value, number_of_draws_for_leave);
            // The full rack for the exchange was recorded above,
            // but now we have to record the leave and the exchanged tiles
            for (int exchanged_tile_index = 0; exchanged_tile_index < top_move->tiles_played; exchanged_tile_index++) {
                uint8_t tile_exchanged = top_move->tiles[exchanged_tile_index];
                add_letter_to_rack(inference->exchanged, tile_exchanged);
                take_letter_from_rack(inference->leave, tile_exchanged);
            }
            record_valid_leave(inference->leave_record, inference->leave, get_leave_value(inference->klv, inference->leave), number_of_draws_for_leave);
            record_valid_leave(inference->exchanged_record, inference->exchanged, get_leave_value(inference->klv, inference->exchanged), number_of_draws_for_leave);
            insert_leave_rack(inference->leave_rack_list, inference->leave, inference->exchanged, number_of_draws_for_leave, current_leave_value);
            reset_rack(inference->exchanged);
            for (int exchanged_tile_index = 0; exchanged_tile_index < top_move->tiles_played; exchanged_tile_index++) {
                uint8_t tile_exchanged = top_move->tiles[exchanged_tile_index];
                add_letter_to_rack(inference->leave, tile_exchanged);
            }
        } else {
            record_valid_leave(inference->leave_record, inference->leave, current_leave_value, number_of_draws_for_leave);
            insert_leave_rack(inference->leave_rack_list, inference->leave, NULL, number_of_draws_for_leave, current_leave_value);
        }
    }
}

void increment_letter_for_inference(Inference * inference, uint8_t letter) {
    take_letter_from_rack(inference->bag_as_rack, letter);
    add_letter_to_rack(inference->player_to_infer_rack, letter);
    add_letter_to_rack(inference->leave, letter);
}

void decrement_letter_for_inference(Inference * inference, uint8_t letter) {
    add_letter_to_rack(inference->bag_as_rack, letter);
    take_letter_from_rack(inference->player_to_infer_rack, letter);
    take_letter_from_rack(inference->leave, letter);
}

void count_all_possible_leaves(Rack * bag_as_rack, int tiles_to_infer, int start_letter, uint64_t * count) {
    if (tiles_to_infer == 0) {
        *count += 1;
        return;
    }
	for (int letter = start_letter; letter < bag_as_rack->array_size; letter++) {
        if (bag_as_rack->array[letter] > 0) {
            take_letter_from_rack(bag_as_rack, letter);
            count_all_possible_leaves(bag_as_rack, tiles_to_infer - 1, letter, count);
            add_letter_to_rack(bag_as_rack, letter);
        }
    }
}

void iterate_through_all_possible_leaves(Inference * inference, int tiles_to_infer, int start_letter) {
    if (tiles_to_infer == 0) {
        if (inference->lower_inclusive_bound <= inference->current_rack_index &&
            inference->upper_inclusive_bound >= inference->current_rack_index) {
                double current_leave_value = 0;
                if (inference->number_of_tiles_exchanged == 0) {
                    current_leave_value = get_leave_value(inference->klv, inference->leave);
                }
                evaluate_possible_leave(inference, current_leave_value);
            }
        inference->current_rack_index++;
        return;
    }
	for (int letter = start_letter; letter < inference->distribution_size; letter++) {
        if (inference->bag_as_rack->array[letter] > 0) {
            increment_letter_for_inference(inference, letter);
            iterate_through_all_possible_leaves(inference, tiles_to_infer - 1, letter);
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
}

void set_bounds_for_worker(Inference * inference, int thread_index, int number_of_threads, uint64_t racks_to_iterate_through) {
    // This assumes that racks_to_iterate_through >= number_of_threads
    // Set the lower and upper bounds for the thread
    if (racks_to_iterate_through == (uint64_t)number_of_threads) {
        // Handle the trivial case
        inference->lower_inclusive_bound = thread_index;
        inference->upper_inclusive_bound = thread_index;
    } else {
        int number_of_evaluations_for_thread = racks_to_iterate_through / number_of_threads;
        int remainder_evaluations = racks_to_iterate_through % number_of_threads;
        int lower_remainder_adjustment = 0;
        int upper_remainder_adjustment = 0;
        // There is a remainder after the modulus, so
        // spread out the remaining evaluations for a remainder of R
        // among the first R threads. Since this will assign bounds,
        // adjustments need to be propagated.
        if (remainder_evaluations > 0) {
            int remainder_adjustment = thread_index;
            if (thread_index >= remainder_evaluations) {
                remainder_adjustment = remainder_evaluations;
            }
            lower_remainder_adjustment = remainder_adjustment;
            upper_remainder_adjustment = remainder_adjustment + 1;
        }
        inference->lower_inclusive_bound = thread_index * number_of_evaluations_for_thread + lower_remainder_adjustment;
        inference->upper_inclusive_bound = (thread_index + 1) * number_of_evaluations_for_thread + upper_remainder_adjustment;
    }
    inference->current_rack_index = 0;
}

void initialize_inference_for_evaluation(Inference * inference, Game * game, Rack * actual_tiles_played, int player_to_infer_index, int actual_score, int number_of_tiles_exchanged, float equity_margin) {    
    initialize_inference_record_for_evaluation(inference->leave_record, inference->draw_and_leave_subtotals_size);
    initialize_inference_record_for_evaluation(inference->exchanged_record, inference->draw_and_leave_subtotals_size);
    initialize_inference_record_for_evaluation(inference->rack_record, inference->draw_and_leave_subtotals_size);
    reset_leave_rack_list(inference->leave_rack_list);

    inference->game = game;
    inference->actual_score = actual_score;
    inference->number_of_tiles_exchanged = number_of_tiles_exchanged;
    inference->equity_margin = equity_margin;

    inference->player_to_infer_index = player_to_infer_index;
    inference->klv = game->players[player_to_infer_index]->strategy_params->klv;
    inference->player_to_infer_rack = game->players[player_to_infer_index]->rack;

    reset_rack(inference->bag_as_rack);
    reset_rack(inference->leave);

    // Create the bag as a rack
    for (int i = 0; i <= game->gen->bag->last_tile_index; i++) {
        add_letter_to_rack(inference->bag_as_rack, game->gen->bag->tiles[i]);
    }

    // Add any existing tiles on the player's rack
    // to the player's leave for partial inferences
    for (int i = 0; i < inference->player_to_infer_rack->array_size; i++) {
        for (int j = 0; j < inference->player_to_infer_rack->array[i]; j++) {
            add_letter_to_rack(inference->leave, i);
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

Inference * copy_inference(Inference * inference) {
    return NULL;
}

void infer_worker(Inference * inference, int tiles_to_infer) {
    iterate_through_all_possible_leaves(inference, tiles_to_infer, BLANK_MACHINE_LETTER);
}

void infer_manager(Inference * inference, int number_of_threads, int racks_to_iterate_through, int tiles_to_infer) {
    if (number_of_threads == 1) {
        set_bounds_for_worker(inference, 0, 1, racks_to_iterate_through);
        infer_worker(inference, tiles_to_infer);
        return;
    }

    printf("unimplemented\n");
    abort();

    Inference ** inference_workers = malloc((sizeof(Inference*)) * (number_of_threads));
    for (int thread_index = 0; thread_index < number_of_threads; thread_index++) {
        inference_workers[thread_index] = copy_inference(inference);
        set_bounds_for_worker(inference_workers[thread_index], thread_index, number_of_threads, racks_to_iterate_through);
        // Spin off infer_worker on a separate thread
        // infer_worker(inference_workers->[thread_index]);
    }

    // Combine and free
    for (int thread_index = 0; thread_index < number_of_threads; thread_index++) {
        // inference_workers->[thread_index].join;
        // add_to_inference(inference, inference_workers->[thread_index]);
        destroy_inference(inference_workers[thread_index]);
    }
    free(inference_workers);
}

int infer(Inference * inference, Game * game, Rack * actual_tiles_played, int player_to_infer_index, int actual_score, int number_of_tiles_exchanged, float equity_margin, int number_of_threads) {
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

    if (number_of_threads < 1) {
        return INFERENCE_STATUS_INVALID_NUMBER_OF_THREADS;
    }

    int tiles_to_infer = (RACK_SIZE) - inference->player_to_infer_rack->number_of_letters;
    uint64_t racks_to_iterate_through = 0;
    count_all_possible_leaves(inference->bag_as_rack, tiles_to_infer, BLANK_MACHINE_LETTER, &racks_to_iterate_through);
    if (racks_to_iterate_through < (uint64_t)number_of_threads) {
        number_of_threads = racks_to_iterate_through;
    }
    infer_manager(inference, number_of_threads, racks_to_iterate_through, tiles_to_infer);

    // Return the player to infer rack to it's original
    // state since the inference does not own that struct
    for (int i = 0; i < actual_tiles_played->array_size; i++) {
        for (int j = 0; j < actual_tiles_played->array[i]; j++) {
            take_letter_from_rack(inference->player_to_infer_rack, i);
        }
    }

    return INFERENCE_STATUS_SUCCESS;
}