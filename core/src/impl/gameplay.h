#ifndef GAMEPLAY_H
#define GAMEPLAY_H

#include "../def/move_defs.h"

#include "../ent/bag.h"
#include "../ent/game.h"
#include "../ent/move.h"
#include "../ent/rack.h"

void draw_starting_racks(Game *game);
void play_move(const Move *move, Game *game);
void set_random_rack(Game *game, int pidx, Rack *existing_rack);
Move *get_top_equity_move(Game *game, MoveList **move_list);
void generate_moves_for_game(Game *game, move_record_t move_record_type,
                             move_sort_t move_sort_type, MoveList **move_list);
void generate_moves_for_game_with_player_move_types(Game *game,
                                                    MoveList **move_list);

#endif