#ifndef GAMEPLAY_H
#define GAMEPLAY_H

#include "../def/move_defs.h"
#include "../ent/bag.h"
#include "../ent/game.h"
#include "../ent/game_history.h"
#include "../ent/move.h"
#include "../ent/rack.h"
#include "move_gen.h"

void draw_starting_racks(Game *game);
Equity calculate_end_rack_points(const Rack *rack,
                                 const LetterDistribution *ld);
Equity calculate_end_rack_penalty(const Rack *rack,
                                  const LetterDistribution *ld);
void play_move(const Move *move, Game *game, Rack *leave);
void play_move_without_drawing_tiles(const Move *move, Game *game);
void set_random_rack(Game *game, int player_index, const Rack *known_rack);
Move *get_top_equity_move(Game *game, int thread_index, MoveList *move_list);
void generate_moves_for_game_override_record_type(
    const MoveGenArgs *args, move_record_t move_record_type);
void generate_moves_for_game(const MoveGenArgs *args);
void draw_to_full_rack(const Game *game, int player_index);
int draw_rack_string_from_bag(const Game *game, int player_index,
                              const char *rack_string);
bool draw_rack_from_bag(Game *game, int player_index, const Rack *rack_to_draw);
void draw_leave_from_bag(Bag *bag, int player_draw_index, Rack *rack_to_update,
                         const Rack *rack_to_draw);
void get_leave_for_move(const Move *move, const Game *game, Rack *leave);
void return_rack_to_bag(const Game *game, int player_index);
bool rack_is_drawable(const Game *game, int player_index,
                      const Rack *rack_to_draw);
Equity get_leave_value_for_move(const KLV *klv, const Move *move, Rack *rack);
void return_phony_letters(Game *game);

void game_play_n_events(GameHistory *game_history, Game *game, int event_index,
                        bool validate, ErrorStack *error_stack);
bool game_history_contains_end_rack_penalty_event(
    const GameHistory *game_history);

#endif