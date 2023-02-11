#ifndef UTIL_H
#define UTIL_H

#include "../src/config.h"
#include "../src/game.h"
#include "../src/move.h"
#include "../src/rack.h"

void generate_moves_for_game(Game * game);
double get_leave_value_for_move(Config * config, Move * move, Rack * rack);
int within_epsilon(double a, double b);
void reset_string(char * string);
void write_char_to_end_of_buffer(char * buffer, char c);
void write_double_to_end_of_buffer(char * buffer, double d);
void write_int_to_end_of_buffer(char * buffer, int n);
void write_rack_to_end_of_buffer(char * dest, Alphabet * alphabet, Rack * rack);
void write_spaces_to_end_of_buffer(char * buffer, int n);
void write_string_to_end_of_buffer(char * buffer, char * s);

#endif
