#include <stdio.h>
#include <stdlib.h>

#include "bag.h"
#include "game.h"
#include "infer.h"
#include "kwg.h"
#include "move.h"
#include "rack.h"

Inference * create_inference(int distribution_size) {
    Inference * inference =  malloc(sizeof(Inference));

    inference->status = INFERENCE_STATUS_INITIALIZED;
    inference->total_possible_leaves = 0;
    inference->leaves_including_letter = (int *) malloc(distribution_size*sizeof(int));
	for (int i = 0; i < distribution_size; i++) {
		inference->leaves_including_letter[i] = 0;
	}

    return inference;
}

void destroy_inference(Inference * inference) {
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
    generate_moves(game->gen, game->players[p_onturn], game->players[1 - p_onturn]->rack, game->gen->bag->last_tile_index + 1 >= RACK_SIZE);
    game->players[p_onturn]->strategy_params->play_recorder_type = original_recorder_type;
    return game->gen->move_list->moves[0];
}

int within_equity_margin(Inference * inference) {
    Move * top_move = get_top_move(inference);

    float current_leave_value = inference->game->players[inference->game->player_on_turn_index]->strategy_params->klv->leave_values[inference->current_node_index];
    float actual_play_equity_for_leave = inference->actual_score + current_leave_value;

    return actual_play_equity_for_leave + inference->equity_margin + (INFERENCE_EQUITY_EPSILON) >= top_move->equity;
}

void record_valid_leave(Inference * inference) {
    int number_of_ways_to_draw_leave = compute_number_of_ways_to_draw_leave(inference);
    inference->total_possible_leaves = number_of_ways_to_draw_leave;
    for (int i = 0; i < inference->player_on_turn_rack->array_size; i++) {
        if (inference->player_on_turn_rack->array[i] > 0) {
            inference->leaves_including_letter[i] += number_of_ways_to_draw_leave;
        }
    }
}

void evaluate_possible_leave(Inference * inference) {
    if (!within_equity_margin(inference)) {
        return;
    }
    record_valid_leave(inference);
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

void iterate_through_all_possible_leaves(Inference * inference) {
    if (inference->leave_tiles_remaining == 0) {
        evaluate_possible_leave(inference);
        return;
    }
	for (int i = inference->current_node_index; ; i++) {
		int leave_letter = kwg_tile(inference->game->gen->kwg, i);
        if (inference->bag_as_rack->array[leave_letter] > 0) {
            increment_letter_for_inference(inference, leave_letter);
            iterate_through_all_possible_leaves(inference);
            decrement_letter_for_inference(inference, leave_letter);
        }
        if (kwg_is_end(inference->game->gen->kwg, i)) {
            break;
        }
    }
}

Rack * create_rack_from_bag(Game * game) {
    Rack * rack = create_rack(game->gen->letter_distribution->size);
    for (int i = 0; i <= game->gen->bag->last_tile_index; i++) {
        add_letter_to_rack(rack, game->gen->bag->tiles[i]);
    }
    return rack;
}

void initialize_inference_for_evaluation(Inference * inference, Game * game, Rack * player_on_turn_rack, Rack * bag_as_rack, int actual_score, int current_node_index, int leave_tiles_remaining, float equity_margin) {
    inference->game = game;
    inference->player_on_turn_rack = player_on_turn_rack;
    inference->bag_as_rack = bag_as_rack;
    inference->player_leave = create_rack(player_on_turn_rack->array_size);
    inference->actual_score = actual_score;
    inference->current_node_index = current_node_index;
    inference->leave_tiles_remaining = leave_tiles_remaining;
    inference->equity_margin = equity_margin;
}

Inference * infer(Game * game, Rack * actual_tiles_played, int actual_score, float equity_margin) {
    Inference * inference = create_inference(game->gen->letter_distribution->size);

    if (!game->players[0]->rack->empty || !game->players[1]->rack->empty) {
        inference->status = INFERENCE_STATUS_RACKS_NOT_EMPTY;
        return inference;
    }

    if ((game->gen->bag->last_tile_index + 1) <= RACK_SIZE) {
        set_inference_to_remaining_tiles(inference, game);
        return inference;
    }

    // Remove the tiles played in the move from the game bag
    // and add them to the player's rack
    Rack * player_on_turn_rack = game->players[game->player_on_turn_index]->rack;
    for (int i = 0; i < actual_tiles_played->array_size; i++) {
        for (int j = 0; j < actual_tiles_played->array[i]; j++) {
            draw_letter(game->gen->bag, i);
            add_letter_to_rack(player_on_turn_rack, i);
        }
    }

    int number_of_tiles_remaining_on_rack = (RACK_SIZE) - actual_tiles_played->number_of_letters;
    Rack * bag_as_rack = create_rack_from_bag(game);
    initialize_inference_for_evaluation(inference, game, player_on_turn_rack, bag_as_rack, actual_score, kwg_get_root_node_index(game->gen->kwg), number_of_tiles_remaining_on_rack, equity_margin);
    iterate_through_all_possible_leaves(inference);

    inference->status = INFERENCE_STATUS_SUCCESS;

    destroy_rack(bag_as_rack);
    return inference;
}