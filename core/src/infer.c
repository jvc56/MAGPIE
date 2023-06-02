#include <stdio.h>
#include <stdlib.h>

#include "bag.h"
#include "game.h"
#include "infer.h"
#include "kwg.h"
#include "klv.h"
#include "move.h"
#include "rack.h"

#include "../test/game_print.h"

Inference * create_inference(int distribution_size) {
    Inference * inference =  malloc(sizeof(Inference));
    inference->distribution_size = distribution_size;
    inference->leaves_including_letter = (int *) malloc(distribution_size*sizeof(int));
    inference->player_leave = create_rack(distribution_size);
    inference->bag_as_rack = create_rack(distribution_size);
    return inference;
}

void destroy_inference(Inference * inference) {
    destroy_rack(inference->player_leave);
    destroy_rack(inference->bag_as_rack);
    free(inference->leaves_including_letter);
    free(inference);
}

void set_inference_to_remaining_tiles(Inference * inference, Game * game) {
    inference->total_possible_leaves = 1;
    for (int i = 0; i <= game->gen->bag->last_tile_index; i++) {
        inference->leaves_including_letter[game->gen->bag->tiles[i]] = 1;
    }
    inference->status = INFERENCE_STATUS_BAG_EMPTY;
}

int choose(int n, int k) {
    if (k == 0) {
        return 1;
    }
    return (n * choose(n - 1, k - 1)) / k;
}

int compute_number_of_ways_to_draw_leave(Inference * inference) {
    int number_of_ways = 1;
    for (int i = 0; i < inference->player_leave->array_size; i++) {
        if (inference->player_leave->array[i] > 0) {
            number_of_ways *= choose(inference->bag_as_rack->array[i] + inference->player_leave->array[i], inference->player_leave->array[i]);
        }
    }
    return number_of_ways;
}

Move * get_top_move(Inference * inference) {
    Game * game = inference->game;
    int p_onturn = game->player_on_turn_index;
    int original_recorder_type = game->players[p_onturn]->strategy_params->play_recorder_type;
    game->players[p_onturn]->strategy_params->play_recorder_type = PLAY_RECORDER_TYPE_TOP_EQUITY;
    reset_move_list(game->gen->move_list);
    generate_moves(game->gen, game->players[p_onturn], game->players[1 - p_onturn]->rack, game->gen->bag->last_tile_index + 1 >= RACK_SIZE);
    print_game(game);
    game->players[p_onturn]->strategy_params->play_recorder_type = original_recorder_type;
    return game->gen->move_list->moves[0];
}

int within_equity_margin(Inference * inference, float current_leave_value) {
    Move * top_move = get_top_move(inference);
    printf("%d + %0.2f >= %0.2f\n", inference->actual_score, current_leave_value, top_move->equity);
    return inference->actual_score + current_leave_value + inference->equity_margin + (INFERENCE_EQUITY_EPSILON) >= top_move->equity;
}

void record_valid_leave(Inference * inference) {
    int number_of_ways_to_draw_leave = compute_number_of_ways_to_draw_leave(inference);
    inference->total_possible_leaves += number_of_ways_to_draw_leave;
    for (int i = 0; i < inference->player_leave->array_size; i++) {
        if (inference->player_leave->array[i] > 0) {
            inference->leaves_including_letter[i] += number_of_ways_to_draw_leave;
        }
    }
}

void evaluate_possible_leave(Inference * inference, float current_leave_value) {
    if (within_equity_margin(inference, current_leave_value)) {
        record_valid_leave(inference);
    }
}

void increment_letter_for_inference(Inference * inference, uint8_t letter) {
    take_letter_from_rack(inference->bag_as_rack, letter);
    add_letter_to_rack(inference->player_on_turn_rack, letter);
    add_letter_to_rack(inference->player_leave, letter);
}

void decrement_letter_for_inference(Inference * inference, uint8_t letter) {
    add_letter_to_rack(inference->bag_as_rack, letter);
    take_letter_from_rack(inference->player_on_turn_rack, letter);
    take_letter_from_rack(inference->player_leave, letter);
}

void iterate_through_all_possible_leaves(Inference * inference, int leave_tiles_remaining, int start_letter) {
    if (leave_tiles_remaining == 0) {
        evaluate_possible_leave(inference, leave_value(inference->klv, inference->player_leave));
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
    inference->status = INFERENCE_STATUS_SUCCESS;
}

void initialize_inference_for_evaluation(Inference * inference, Game * game, Rack * actual_tiles_played, int actual_score, float equity_margin) {
    inference->game = game;
    inference->actual_score = actual_score;
    inference->equity_margin = equity_margin;

    // Reset record
    inference->total_possible_leaves = 0;
    inference->status = INFERENCE_STATUS_INITIALIZED;
    for (int i = 0; i < inference->distribution_size; i++) {
        inference->leaves_including_letter[i] = 0;
    }

    inference->klv = game->players[inference->game->player_on_turn_index]->strategy_params->klv;
    inference->player_on_turn_rack = game->players[game->player_on_turn_index]->rack;
    reset_rack(inference->player_on_turn_rack);
    reset_rack(inference->bag_as_rack);
    reset_rack(inference->player_leave);

    // Create the bag as a rack
    for (int i = 0; i <= game->gen->bag->last_tile_index; i++) {
        add_letter_to_rack(inference->bag_as_rack, game->gen->bag->tiles[i]);
    }

    // Remove the tiles played in the move from the game bag
    // and add them to the player's rack
    for (int i = 0; i < actual_tiles_played->array_size; i++) {
        for (int j = 0; j < actual_tiles_played->array[i]; j++) {
            draw_letter(game->gen->bag, i);
            take_letter_from_rack(inference->bag_as_rack, i);
            add_letter_to_rack(inference->player_on_turn_rack, i);
        }
    }
}

Inference * infer(Game * game, Rack * actual_tiles_played, int actual_score, float equity_margin) {
    Inference * inference = create_inference(game->gen->letter_distribution->size);

    if (actual_tiles_played->number_of_letters == 0) {
        inference->status = INFERENCE_STATUS_NOTHING_TO_INFER;
        return inference;
    }

    int number_of_tiles_remaining = (RACK_SIZE) - actual_tiles_played->number_of_letters;

    if (number_of_tiles_remaining < 0) {
        inference->status = INFERENCE_STATUS_NEGATIVE_TILES_REMAINING;
        return inference;
    }

    if (!game->players[0]->rack->empty || !game->players[1]->rack->empty) {
        inference->status = INFERENCE_STATUS_RACKS_NOT_EMPTY;
        return inference;
    }

    if ((game->gen->bag->last_tile_index + 1) <= RACK_SIZE) {
        set_inference_to_remaining_tiles(inference, game);
        return inference;
    }

    initialize_inference_for_evaluation(inference, game, actual_tiles_played, actual_score, equity_margin);
    iterate_through_all_possible_leaves(inference, number_of_tiles_remaining, BLANK_MACHINE_LETTER);
    reset_rack(inference->player_on_turn_rack);
    return inference;
}