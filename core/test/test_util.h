#ifndef UTIL_H
#define UTIL_H

#include "../src/config.h"
#include "../src/game.h"
#include "../src/leaves.h"
#include "../src/move.h"
#include "../src/rack.h"

void generate_moves_for_game(Game * game);
double get_leave_value_for_move(Laddag * laddag, Move * move, Rack * rack);
void play_top_n_equity_move(Game * game, int n);
int within_epsilon(double a, double b);
void reset_string(char * string);
void write_char_to_end_of_buffer(char * buffer, char c);
void write_double_to_end_of_buffer(char * buffer, double d);
void write_int_to_end_of_buffer(char * buffer, int n);
void write_rack_to_end_of_buffer(char * dest, Alphabet * alphabet, Rack * rack);
void write_spaces_to_end_of_buffer(char * buffer, int n);
void write_string_to_end_of_buffer(char * buffer, char * s);

#endif
