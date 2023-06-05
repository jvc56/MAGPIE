#include <stdio.h>
#include <string.h>

#include "../src/infer.h"
#include "../src/rack.h"

#include "inference_print.h"
#include "game_print.h"
#include "test_util.h"

void print_inference(Inference * inference, Rack * actual_tiles_played) {
	char inference_string[2700] = "";

    Game * game = inference->game;
    uint64_t total_draws = weight(inference->leave_values);
    uint64_t total_leaves = cardinality(inference->leave_values);
    write_string_to_end_of_buffer(inference_string, "Played tiles: ");
    write_rack_to_end_of_buffer(inference_string, inference->game->gen->letter_distribution, actual_tiles_played);
    sprintf(inference_string + strlen(inference_string), "\nScore:        %d\n", inference->actual_score);
    sprintf(inference_string + strlen(inference_string), "Total possible draws:  %ld\n", total_draws);
    sprintf(inference_string + strlen(inference_string), "Total possible leaves: %ld\n", total_leaves);
    sprintf(inference_string + strlen(inference_string), "Average leave value:   %0.2f\n", mean(inference->leave_values));
    sprintf(inference_string + strlen(inference_string), "Stdev leave value:     %0.2f\n\n", stdev(inference->leave_values));
    if (total_draws > 0) {
        for (int i = 0; i < (int)game->gen->letter_distribution->size; i++) {
            int draw_subtotal = get_subtotal_sum_with_minimum(inference, i, 1, INFERENCE_SUBTOTAL_INDEX_OFFSET_DRAW);
            sprintf(inference_string + strlen(inference_string), "%c: %6.2f%% %d\n",
            game->gen->letter_distribution->machine_letter_to_human_readable_letter[i],
            ((float)draw_subtotal) / ((float)total_draws) * 100.0,
            draw_subtotal);
        }
    }


    printf("\n\nInference Report:\n\n");
    print_game(inference->game);
	printf("\n%s\n", inference_string);
}