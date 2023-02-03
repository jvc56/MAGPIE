#include "../src/game.h"
#include "../src/gameplay.h"

void test_play_random_top_equity_game_gen_all_moves(Config * config, int n) {
    config->play_recorder_type = PLAY_RECORDER_TYPE_ALL;
    Game * game = create_game(config);
    for (int i = 0; i < n; i++) {
        play_random_top_equity_game(game);
        reset_game(game);
    }
    destroy_game(game);
}

void test_play_random_games(Config * config, int n) {
    test_play_random_top_equity_game_gen_all_moves(config, n);
}