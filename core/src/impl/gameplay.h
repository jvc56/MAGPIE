#ifndef GAMEPLAY_H
#define GAMEPLAY_H

#include "../def/move_defs.h"

#include "../ent/bag.h"
#include "../ent/game.h"
#include "../ent/move.h"
#include "../ent/rack.h"

void draw_starting_racks(Game *game);
void play_move(const Move *move, Game *game);
void set_random_rack(Game *game, int pidx, Rack *known_rack);
Move *get_top_equity_move(Game *game, int thread_index, MoveList *move_list);
void generate_moves_for_game(Game *game, int thread_index, MoveList *move_list);
void draw_at_most_to_rack(Bag *bag, Rack *rack, int n, int player_draw_index);

#endif