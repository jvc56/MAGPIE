#ifndef MOVE_PRINT_H
#define MOVE_PRINT_H

#include "../src/move.h"

void write_user_visible_move_to_end_of_buffer(char * buf, Board * b, Move * m, Alphabet * alphabet);
void write_move_list_to_end_of_buffer(char * buf, MoveList * ml, Board * b, Alphabet * alph);

#endif