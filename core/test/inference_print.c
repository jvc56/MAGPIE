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
    int total_possible_leaves = inference->total_possible_leaves;
    write_string_to_end_of_buffer(inference_string, "Played tiles: ");
    write_rack_to_end_of_buffer(inference_string, inference->game->gen->letter_distribution, actual_tiles_played);
    sprintf(inference_string + strlen(inference_string), "\nScore:        %d\n\nTotal possible leaves: %d\n\n", inference->actual_score, total_possible_leaves);

    if (total_possible_leaves > 0) {
        for (int i = 0; i < (int)game->gen->letter_distribution->size; i++) {
            int leaves_including_letter = inference->leaves_including_letter[i];
            sprintf(inference_string + strlen(inference_string), "%c: %6.2f%% %d\n",
            game->gen->letter_distribution->machine_letter_to_human_readable_letter[i],
            ((float)leaves_including_letter) / ((float)total_possible_leaves) * 100.0,
            leaves_including_letter);
        }
    }

    printf("\n\nInference Report:\n\n");
    print_game(inference->game);
	printf("\n%s\n", inference_string);
}