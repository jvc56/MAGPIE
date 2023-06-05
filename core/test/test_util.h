#ifndef UTIL_H
#define UTIL_H

#include "../src/board.h"
#include "../src/config.h"
#include "../src/game.h"
#include "../src/movegen.h"
#include "../src/klv.h"
#include "../src/letter_distribution.h"
#include "../src/move.h"
#include "../src/rack.h"

typedef struct SortedMoveList {
    int count;
    Move ** moves;
} SortedMoveList;

void generate_moves_for_game(Game * game);
float get_leave_value_for_move(KLV * klv, Move * move, Rack * rack);
float get_leave_value_for_rack(KLV * klv, Rack * rack);
void play_top_n_equity_move(Game * game, int n);
SortedMoveList * create_sorted_move_list(MoveList * ml);
void destroy_sorted_move_list(SortedMoveList * sorted_move_list);
void print_anchor_list(Generator * gen);
void print_move_list(Board * board, LetterDistribution * letter_distribution, SortedMoveList * sml, int move_list_length);
void sort_and_print_move_list(Board * board, LetterDistribution * letter_distribution, MoveList * ml);
int within_epsilon_float(float a, float b);
int within_epsilon_double(double a, double b);
void reset_string(char * string);
void write_char_to_end_of_buffer(char * buffer, char c);
void write_double_to_end_of_buffer(char * buffer, float d);
void write_int_to_end_of_buffer(char * buffer, int n);
void write_rack_to_end_of_buffer(char * dest, LetterDistribution * letter_distribution, Rack * rack);
void write_spaces_to_end_of_buffer(char * buffer, int n);
void write_string_to_end_of_buffer(char * buffer, char * s);

#endif
