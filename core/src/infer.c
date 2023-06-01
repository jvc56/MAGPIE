#include <stdio.h>
#include <stdlib.h>

#include "bag.h"
#include "game.h"
#include "infer.h"
#include "kwg.h"
#include "move.h"
#include "rack.h"

// #define INFERENCE_STATUS_INITIALIZED 0
// #define INFERENCE_STATUS_SUCCESS 1
// #define INFERENCE_STATUS_UNSUPPORTED_MOVE_TYPE 2
// #define INFERENCE_STATUS_BAG_EMPTY 3

Inference * create_inference(int distribution_size) {
    Inference * inference =  malloc(sizeof(Inference));

    inference->status = INFERENCE_STATUS_INITIALIZED;
    inference->total_possible_leaves = 0;
    inference->total_possible_distinct_leaves = 0;
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
    inference->total_possible_distinct_leaves = 1;
    for (int i = 0; i <= game->gen->bag->last_tile_index; i++) {
        inference->leaves_including_letter[game->gen->bag->tiles[i]] = 1;
    }
    inference->status = INFERENCE_STATUS_BAG_EMPTY;
}

int compute_number_of_ways_to_draw_leave(Bag * bag, Rack * rack) {

}

void evaluate_possible_leave(Inference * inference, Game * game, Rack * actual_tiles_played, int actual_score) {
    int original_recorder_type = game->players[game->player_on_turn_index]->strategy_params->play_recorder_type;
    game->players[game->player_on_turn_index]->strategy_params->play_recorder_type = PLAY_RECORDER_TYPE_TOP_EQUITY;
    generate_moves(game->gen, game->players[game->player_on_turn_index], game->players[1 - game->player_on_turn_index]->rack, game->gen->bag->last_tile_index + 1 >= RACK_SIZE);
    game->players[game->player_on_turn_index]->strategy_params->play_recorder_type = original_recorder_type;
    Move * top_move = game->gen->move_list->moves[0];

    // Check if the actual move and the top move for the given
    // rack score the same and use the same tiles.
    if (top_move->score != actual_score || top_move->tiles_played != actual_tiles_played->number_of_letters) {
        return;
    }

    Rack * player_on_turn_rack = game->players[game->player_on_turn_index]->rack;
    for (int i = 0; i < top_move->tiles_length; i++) {
        uint8_t tile = top_move->tiles[i];
        if (tile != PLAYED_THROUGH_MARKER) {
            if (actual_tiles_played->array[tile] > 0) {
                take_letter_from_rack(actual_tiles_played, tile);
                take_letter_from_rack(game->players[game->player_on_turn_index]->rack), tile);
            } else {
                return;
            }
        }
    }
    
    if (!actual_tiles_played->empty) {
        // This should be impossible
        abort();
    }

    // At this point, the plays match
    // increment the inference stats

    inference->total_possible_distinct_leaves++;

    int number_of_ways_to_draw_leave = compute_number_of_ways_to_draw_leave(game->gen->bag, player_on_turn_rack);
    inference->total_possible_leaves = number_of_ways_to_draw_leave;
    for (int i = 0; i < player_on_turn_rack->array_size; i++) {
        if (player_on_turn_rack->array[i] > 0) {
            inference->leaves_including_letter[i] += number_of_ways_to_draw_leave;
        }
    }

    // Restore the actual rack
    for (int i = 0; i < top_move->tiles_length; i++) {
        uint8_t tile = top_move->tiles[i];
        if (tile != PLAYED_THROUGH_MARKER) {
            add_letter_to_rack(actual_tiles_played, tile);
            add_letter_to_rack(player_on_turn_rack, tile);
        }
    }
}

void iterate_through_all_possible_leaves(Inference * inference, Game * game, Rack * player_on_turn_rack, Rack * bag_as_rack, int current_node_index, int leave_tiles_remaining, Rack * actual_tiles_played, int actual_score) {
    if (leave_tiles_remaining == 0) {
        evaluate_possible_leave(inference, game, actual_tiles_played, actual_score);
        return;
    }
	for (int i = current_node_index; ; i++) {
		int leave_letter = kwg_tile(gen->kwg, i);
        if (bag_as_rack->array[leave_letter] > 0) {
            take_letter_from_rack(bag_as_rack, leave_letter);
            add_letter_to_rack(player_on_turn_rack, leave_letter);
            iterate_through_all_possible_leaves(inference, game, player_on_turn_rack, bag_as_rack, kwg_arc_index(gen->kwg, i), leave_tiles_remaining - 1, actual_tiles_played, actual_score);
            add_letter_to_rack(bag_as_rack, leave_letter);
            take_letter_from_rack(player_on_turn_rack, leave_letter);
        }
        if (kwg_is_end(gen->kwg, i)) {
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

Inference * infer(Game * game, Rack * actual_tiles_played, int actual_score) {
    Inference * inference = create_inference(game->gen->letter_distribution->size);

    if (move->move_type == MOVE_TYPE_EXCHANGE || move->move_type == MOVE_TYPE_PASS) {
        inference->status = INFERENCE_STATUS_UNSUPPORTED_MOVE_TYPE;
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

    // Remove the tiles played in the move from the game bag
    Rack * player_on_turn_rack = game->players[game->player_on_turn_index]->rack;
    for (int i = 0; i < actual_tiles_played->array_size; i++) {
        for (int j = 0; j < actual_tiles_played->array[i]; j++)
        {
            draw_letter(game->gen->bag, i);
            add_letter_to_rack(player_on_turn_rack, i);
        }
    }

    int number_of_tiles_remaining_on_rack = (RACK_SIZE) - actual_tiles_played->number_of_letters;
    Rack * bag_as_rack = create_rack_from_bag(game);
    iterate_through_all_possible_leaves(inference, game, player_on_turn_rack, bag_as_rack, kwg_get_root_node_index(gen->kwg), number_of_tiles_remaining_on_rack, actual_tiles_played, actual_score);

    inference->status = INFERENCE_STATUS_SUCCESS;

    destroy_rack(bag_as_rack);
    return inference;
}