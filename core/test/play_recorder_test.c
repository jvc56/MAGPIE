#include <assert.h>
#include <stdlib.h>
#include <stdio.h>

#include "../src/game.h"
#include "../src/config.h"
#include "../src/gameplay.h"

#include "game_print.h"
#include "test_util.h"

void play_top_versus_all_game(Game * game) {
    int top_move_index;
    float top_move_equity;

    float equity;
    int score;
    int row_start;
    int col_start;
    int tiles_played;
    int tiles_length;
    int vertical;
    int move_type;
    draw_at_most_to_rack(game->gen->bag, game->players[0]->rack, RACK_SIZE);
    draw_at_most_to_rack(game->gen->bag, game->players[1]->rack, RACK_SIZE);
    while (!game->game_end_reason) {
        game->players[game->player_on_turn_index]->strategy_params->play_recorder_type = PLAY_RECORDER_TYPE_TOP_EQUITY;
        generate_moves(game->gen, game->players[game->player_on_turn_index], game->players[1 - game->player_on_turn_index]->rack, game->gen->bag->last_tile_index + 1 >= RACK_SIZE);

        // Record the top move
        equity = game->gen->move_list->moves[0]->equity;
        score = game->gen->move_list->moves[0]->score;
        row_start = game->gen->move_list->moves[0]->row_start;
        col_start = game->gen->move_list->moves[0]->col_start;
        tiles_played = game->gen->move_list->moves[0]->tiles_played;
        tiles_length = game->gen->move_list->moves[0]->tiles_length;
        vertical = game->gen->move_list->moves[0]->vertical;
        move_type = game->gen->move_list->moves[0]->move_type;

        reset_move_list(game->gen->move_list);

        game->players[game->player_on_turn_index]->strategy_params->play_recorder_type = PLAY_RECORDER_TYPE_ALL;
        generate_moves(game->gen, game->players[game->player_on_turn_index], game->players[1 - game->player_on_turn_index]->rack, game->gen->bag->last_tile_index + 1 >= RACK_SIZE);

        // Move list is a min heap, so just iterate through to
        // find the top move instead of popping everything
        top_move_index = 0;
        top_move_equity = game->gen->move_list->moves[0]->equity;
        for (int i = 1; i <  game->gen->move_list->count; i++) {
            if (game->gen->move_list->moves[i]->equity > top_move_equity) {
                top_move_index = i;
                top_move_equity = game->gen->move_list->moves[i]->equity;
            }
        }

        // Ensure that the top move found by gen all matches the top
        // move found by recording the top move only.
        if
        (
            !within_epsilon_double(top_move_equity, game->gen->move_list->moves[top_move_index]->equity) ||
            move_type != game->gen->move_list->moves[top_move_index]->move_type
        ) {
            print_game(game);
            printf("index: %d\n", top_move_index);
            printf("equity: %0.4f, %0.4f\n", equity, top_move_equity);
            printf("scores: %d, %d\n", score, game->gen->move_list->moves[top_move_index]->score);
            printf("row_start: %d, %d\n", row_start, game->gen->move_list->moves[top_move_index]->row_start);
            printf("col_start: %d, %d\n", col_start, game->gen->move_list->moves[top_move_index]->col_start);
            printf("tiles_played: %d, %d\n", tiles_played, game->gen->move_list->moves[top_move_index]->tiles_played);
            printf("tiles_length: %d, %d\n", tiles_length, game->gen->move_list->moves[top_move_index]->tiles_length);
            printf("vertical: %d, %d\n", vertical, game->gen->move_list->moves[top_move_index]->vertical);
            printf("move_type: %d, %d\n", move_type, game->gen->move_list->moves[top_move_index]->move_type);
            abort();
        }

        play_move(game, game->gen->move_list->moves[top_move_index]);
        reset_move_list(game->gen->move_list);
    }
}

void play_top_versus_all_games(Config * config) {
    Game * game = create_game(config);

    for (int i = 0; i < config->number_of_games_or_pairs; i++) {
        play_top_versus_all_game(game);
        reset_game(game);
    }

    destroy_game(game);
}

void test_play_recorder(Config * config) {
    play_top_versus_all_games(config);
}
