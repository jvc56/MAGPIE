#ifndef MOVE_H
#define MOVE_H

#include <stdint.h>

#include "board.h"
#include "constants.h"

typedef struct Move {
    uint8_t tiles[BOARD_DIM];
    int score;
    int row_start;
    int col_start;
    int tiles_played;
    int tiles_length;
    float equity;
    int vertical;
    int move_type;
} Move;

typedef struct MoveList {
    int count;
    int capacity;
    Move * spare_move;
    Move ** moves;
} MoveList;

MoveList * create_move_list();
void destroy_move_list(MoveList * ml);
void set_spare_move(MoveList * ml, uint8_t strip[], int leftstrip, int rightstrip, int score, int row_start, int col_start, int tiles_played, int vertical, int move_type);
void insert_spare_move(MoveList * ml, float equity);
void insert_spare_move_top_equity(MoveList * ml, float equity);
Move * pop_move(MoveList * ml);
void reset_move_list(MoveList * ml);
void set_move(Move * move, uint8_t strip[], int leftstrip, int rightstrip, int score, int row_start, int col_start, int tiles_played, int vertical, int move_type);

#endif