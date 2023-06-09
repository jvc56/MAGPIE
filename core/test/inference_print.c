#include <stdio.h>
#include <string.h>

#include "../src/infer.h"
#include "../src/rack.h"

#include "inference_print.h"
#include "game_print.h"
#include "test_util.h"

void print_leave_rack(LeaveRack * leave_rack, int index, uint64_t total_draws, LetterDistribution * letter_distribution) {
    char leave_string[(RACK_SIZE)] = "";
    write_rack_to_end_of_buffer(leave_string, letter_distribution, leave_rack->rack);
    printf("%-3d %-7s %-6.2f %-6d %0.2f\n", index + 1, leave_string, ((double)leave_rack->draws/total_draws) * 100, leave_rack->draws, leave_rack->equity);
}

void write_letter_minimum(InferenceRecord * record, Rack * bag_as_rack, char * inference_string, uint8_t letter, int minimum, int number_of_actual_tiles_played) {
    int draw_subtotal = get_subtotal_sum_with_minimum(record, letter, minimum, INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW);
    int leave_subtotal = get_subtotal_sum_with_minimum(record, letter, minimum, INFERENCE_SUBTOTAL_INDEX_OFFSET_LEAVE);
    double inference_probability = ((double)draw_subtotal) / (double)weight(record->equity_values);
    double random_probability = get_probability_for_random_minimum_draw(bag_as_rack, record->rack_leave, letter, minimum, number_of_actual_tiles_played);
    sprintf(inference_string + strlen(inference_string), " | %-7.2f %-7.2f%-5d%-5d",
    inference_probability * 100,
    random_probability * 100,
    draw_subtotal,
    leave_subtotal);
}

void write_letter_line(Game * game, InferenceRecord * record, Rack * bag_as_rack, Stat * letter_stat, char * inference_string, uint8_t letter, int max_duplicate_letter_draw, int number_of_actual_tiles_played) {
    get_stat_for_letter(record, letter_stat, letter);
    sprintf(inference_string + strlen(inference_string), "%c: %4.2f %4.2f",
    machine_letter_to_human_readable_letter(game->gen->letter_distribution, letter),
    mean(letter_stat),
    stdev(letter_stat));

    for (int i = 1; i <= max_duplicate_letter_draw; i++) {
        write_letter_minimum(record, bag_as_rack, inference_string, letter, i, number_of_actual_tiles_played);
    }
    write_string_to_end_of_buffer(inference_string, "\n");
}

void print_inference(Inference * inference, Rack * actual_tiles_played) {
	char inference_string[6000] = "";

    // Create a transient stat to use the stat functions
    Stat * letter_stat = create_stat();


    for (int i = 0; i < inference->player_to_infer_rack->array_size; i++) {
        for (int j = 0; j < actual_tiles_played->array[i]; j++) {
            take_letter_from_rack(inference->player_to_infer_rack, i);
        }
    }
    

    Game * game = inference->game;
    uint64_t total_draws = weight(inference->leave_record->equity_values);
    uint64_t total_leaves = cardinality(inference->leave_record->equity_values);
    write_string_to_end_of_buffer(inference_string, "Played tiles:          ");
    write_rack_to_end_of_buffer(inference_string, inference->game->gen->letter_distribution, actual_tiles_played);
    sprintf(inference_string + strlen(inference_string), "\n");
    sprintf(inference_string + strlen(inference_string), "Score:                 %d\n", inference->actual_score);
    if (inference->player_to_infer_rack->number_of_letters > 0) {
        write_string_to_end_of_buffer(inference_string, "Partial Rack:          ");
        write_rack_to_end_of_buffer(inference_string, inference->game->gen->letter_distribution, inference->player_to_infer_rack);
        sprintf(inference_string + strlen(inference_string), "\n");
    }
    sprintf(inference_string + strlen(inference_string), "Equity margin:         %0.2f\n", inference->equity_margin);
    sprintf(inference_string + strlen(inference_string), "Total possible draws:  %ld\n", total_draws);
    sprintf(inference_string + strlen(inference_string), "Total possible leaves: %ld\n", total_leaves);
    sprintf(inference_string + strlen(inference_string), "Average leave value:   %0.2f\n", mean(inference->leave_record->equity_values));
    sprintf(inference_string + strlen(inference_string), "Stdev leave value:     %0.2f\n\n", stdev(inference->leave_record->equity_values));

    int max_duplicate_letter_draw = 0;
    for (int letter = 0; letter < (int)inference->game->gen->letter_distribution->size; letter++) {
        for (int number_of_letter = 1; number_of_letter <= (RACK_SIZE); number_of_letter++) {
            int draws = get_subtotal_sum_with_minimum(inference->leave_record, letter, number_of_letter, INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW);
            if (draws == 0) {
                break;
            }
            if (number_of_letter > max_duplicate_letter_draw) {
                max_duplicate_letter_draw = number_of_letter;
            }
        }
    }

    sprintf(inference_string + strlen(inference_string), "               ");
    for (int i = 0; i < max_duplicate_letter_draw; i++) {
        sprintf(inference_string + strlen(inference_string), "Has at least %d of           ", i + 1);
    }
    sprintf(inference_string + strlen(inference_string), "\n");
    sprintf(inference_string + strlen(inference_string), "\n");

    sprintf(inference_string + strlen(inference_string), "   Avg  Std ");


    for (int i = 0; i < max_duplicate_letter_draw; i++) {
        sprintf(inference_string + strlen(inference_string), " | %%       Rand   Tot  Unq  ");
    }
    sprintf(inference_string + strlen(inference_string), "\n");
    
    if (total_draws > 0) {
        for (int i = 0; i < (int)game->gen->letter_distribution->size; i++) {
            write_letter_line(game, inference->leave_record, inference->bag_as_rack, letter_stat, inference_string, i, max_duplicate_letter_draw, actual_tiles_played->number_of_letters);
        }
    }

    destroy_stat(letter_stat);

    printf("\n\nInference Report:\n\n");
    print_game(inference->game);
	printf("\n%s\n", inference_string);
    // Get the list of most common leaves
    printf("Most Common Leaves\n\n#   Leave   %%      Draws  Equity\n");
    int number_of_common_leaves = inference->leave_record->leave_rack_list->count;
    sort_leave_racks(inference->leave_record->leave_rack_list);
    for (int common_leave_index = 0; common_leave_index < number_of_common_leaves; common_leave_index++) {
        LeaveRack * leave_rack = inference->leave_record->leave_rack_list->leave_racks[common_leave_index];
        print_leave_rack(leave_rack, common_leave_index, weight(inference->leave_record->equity_values), inference->game->gen->letter_distribution);
    }
}