#ifndef GAME_H
#define GAME_H

#include <stdint.h>

#include "bag.h"
#include "config.h"
#include "movegen.h"
#include "player.h"
#include "rack.h"

typedef struct Game {
    Generator * gen;
    Player * players[2];
    int player_on_turn_index;
    int consecutive_scoreless_turns;
    int game_end_reason;
} Game;

void reset_game(Game *  game);
Game * create_game(Config * config);
Game * copy_game(Game * game);
void destroy_game(Game * game);
void load_cgp(Game * game, const char* cgp);
void draw_letter_to_rack(Bag * bag, Rack * rack, uint8_t letter);

#endif