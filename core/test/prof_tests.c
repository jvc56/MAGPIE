#include <stdio.h>
#include <time.h>

#include "prof_tests.h"
#include "test_constants.h"
#include "test_util.h"

#include "../src/config.h"
#include "../src/movegen.h"
#include "../src/game.h"

void many_moves(Config * config) {
    Game * game = create_game(config);
    load_cgp(game, MANY_MOVES);
    clock_t begin = clock();
    generate_moves_for_game(game);
    clock_t end = clock();
    printf("many_moves took %0.6f seconds\n", (double)(end - begin) / CLOCKS_PER_SEC);
    destroy_game(game);
}

void prof_tests(Config * config) {
    many_moves(config);
}