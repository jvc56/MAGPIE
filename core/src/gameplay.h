#ifndef GAMEPLAY_H
#define GAMEPLAY_H

#include <stdint.h>

#include "game.h"
#include "move.h"

void draw_at_most_to_rack(Bag * bag, Rack * rack, int n);
void play_move(Game *  game, Move * move);
void play_random_top_equity_game(Game * game);
void play_top_n_equity_move(Game * game, int n);

#endif