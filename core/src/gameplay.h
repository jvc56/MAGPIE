#ifndef GAMEPLAY_H
#define GAMEPLAY_H

#include <stdint.h>

#include "game.h"
#include "move.h"

void draw_at_most_to_rack(Bag * bag, Rack * rack, int n);
void play_move(Game *  game, Move * move);

#endif