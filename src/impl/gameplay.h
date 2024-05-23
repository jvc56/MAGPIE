#ifndef GAMEPLAY_H
#define GAMEPLAY_H

#include "../def/gameplay_defs.h"
#include "../def/move_defs.h"

#include "../ent/bag.h"
#include "../ent/game.h"
#include "../ent/game_history.h"
#include "../ent/move.h"
#include "../ent/rack.h"

void draw_starting_racks(Game *game);
play_move_status_t play_move(const Move *move, Game *game,
                             const Rack *rack_to_draw);
void set_random_rack(Game *game, int pidx, Rack *known_rack);
Move *get_top_equity_move(Game *game, int thread_index, MoveList *move_list);
void generate_moves_for_game(Game *game, int thread_index, MoveList *move_list);
void draw_letter_to_rack(Bag *bag, Rack *rack, uint8_t letter,
                         int player_draw_index);
void draw_at_most_to_rack(Bag *bag, Rack *rack, int n, int player_draw_index);
int draw_rack_string_from_bag(const LetterDistribution *ld, Bag *bag,
                              Rack *rack, const char *rack_string,
                              int player_draw_index);
bool draw_rack_from_bag(Bag *bag, Rack *rack, const Rack *rack_to_draw,
                        int player_draw_index);
void return_rack_to_bag(Rack *rack, Bag *bag, int player_draw_index);
double get_leave_value_for_move(const KLV *klv, const Move *move, Rack *rack);
void return_phony_tiles(Game *game);
#endif