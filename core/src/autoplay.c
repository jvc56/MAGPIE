#include <stdio.h>

#include "config.h"
#include "game.h"
#include "gameplay.h"

void play_game(Game * game) {
    draw_at_most_to_rack(game->gen->bag, game->players[0]->rack, RACK_SIZE);
    draw_at_most_to_rack(game->gen->bag, game->players[1]->rack, RACK_SIZE);
    while (!game->game_end_reason) {
        generate_moves(game->gen, game->players[game->player_on_turn_index], game->players[1 - game->player_on_turn_index]->rack, game->gen->bag->last_tile_index + 1 >= RACK_SIZE);
        play_move(game, game->gen->move_list->moves[0]);
        reset_move_list(game->gen->move_list);
    }
}

void autoplay_without_using_game_pairs(Config * config) {
    Game * game = create_game(config);

    for (int i = 0; i < config->number_of_games_or_pairs; i++) {
        play_game(game);
        reset_game(game);
    }

    destroy_game(game);
}

void autoplay_using_game_pairs(Config * config) {
    printf("implemented: %s\n", config->cgp);
}

void autoplay(Config * config) {
    if (config->game_pairs) {
        autoplay_using_game_pairs(config);
    } else {
        autoplay_without_using_game_pairs(config);
    }
}