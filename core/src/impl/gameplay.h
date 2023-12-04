#ifndef GAMEPLAY_H
#define GAMEPLAY_H

#include "../ent/bag.h"
#include "../ent/game.h"
#include "../ent/move.h"
#include "../ent/rack.h"

void draw_at_most_to_rack(Bag *bag, Rack *rack, int n, int player_draw_index);
void draw_starting_racks(Game *game);
void play_move(const Move *move, Game *game);
void set_random_rack(Game *game, int pidx, Rack *existing_rack);
Move *get_top_equity_move(Game *game);

#endif