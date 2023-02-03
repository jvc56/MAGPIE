#ifndef MOVE_H
#define MOVE_H

#include <stdint.h>

#include "alphabet.h"
#include "board.h"
#include "constants.h"

typedef struct Move {
    uint8_t tiles[BOARD_DIM];
    int score;
    int row_start;
    int col_start;
    int tiles_played;
    int tiles_length;
    double equity;
    int vertical;
    int move_type;
} Move;

typedef struct MoveList {
    int count;
    Move ** moves;
} MoveList;

MoveList * create_move_list();
void destroy_move_list(MoveList * ml);
Move * new_move(MoveList * ml);
Move * new_top_equity_move(MoveList * ml);
void reset_move_list(MoveList * ml);
void set_move_without_equity(Move * move, uint8_t strip[], int leftstrip, int rightstrip, int score, int row_start, int col_start, int tiles_played, int vertical, int move_type);
void set_move_equity(Move * move, double equity);
void set_move(Move * move, uint8_t strip[], int leftstrip, int rightstrip, int score, double equity, int row_start, int col_start, int tiles_played, int vertical, int move_type);
void set_top_equity_move(MoveList * ml);
void sort_move_list(MoveList * ml);

#endif