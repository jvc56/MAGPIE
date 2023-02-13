#include <stdlib.h>
#include <stdio.h>

#include "constants.h"
#include "move.h"
#include "board.h"

void reset_move_list(MoveList * ml) {
    ml->count = 0;
    ml->moves[0]->equity = INITIAL_TOP_MOVE_EQUITY;
}

MoveList * create_move_list() {
    MoveList *ml = malloc(sizeof(MoveList));
    ml->moves = malloc((sizeof(Move*)) * (MOVE_LIST_CAPACITY));
    for (int i = 0; i < (MOVE_LIST_CAPACITY); i++) {
        ml->moves[i] = malloc(sizeof(Move));
    }
    reset_move_list(ml);
    return ml;
}

void destroy_move(Move * move) {
    free(move);
}

void destroy_move_list(MoveList * ml) {
    for (int i = 0; i < (MOVE_LIST_CAPACITY); i++) {
        destroy_move(ml->moves[i]);
    }
    free(ml->moves);
    free(ml);
}

Move * new_move(MoveList * ml) {
    if (ml->count == MOVE_LIST_CAPACITY) {
        printf("move list at capacity\n");
        exit(EXIT_FAILURE);
    }
    ml->count++;
    return ml->moves[ml->count - 1];
}

Move * new_top_equity_move(MoveList * ml) {
    return ml->moves[1];
}

void set_move_equity(Move * move, double equity) {
    move->equity = equity;
}

void set_move_without_equity(Move * move, uint8_t strip[], int leftstrip, int rightstrip, int score, int row_start, int col_start, int tiles_played, int vertical, int move_type) {
    move->score = score;
    move->row_start = row_start;
    move->col_start = col_start;
    move->tiles_played = tiles_played;
    move->vertical = vertical;
    move->move_type = move_type;
    move->tiles_length = rightstrip - leftstrip + 1;
    for (int i = 0; i < move->tiles_length; i++) {
        move->tiles[i] = strip[leftstrip + i];
    }
}

void set_move(Move * move, uint8_t strip[], int leftstrip, int rightstrip, int score, double equity, int row_start, int col_start, int tiles_played, int vertical, int move_type) {
    move->score = score;
    move->row_start = row_start;
    move->col_start = col_start;
    move->tiles_played = tiles_played;
    move->vertical = vertical;
    move->move_type = move_type;
    move->tiles_length = rightstrip - leftstrip + 1;
    move->equity = equity;
    for (int i = 0; i < move->tiles_length; i++) {
        move->tiles[i] = strip[leftstrip + i];
    }
}

void set_top_equity_move(MoveList * ml) {
    Move * swap = ml->moves[0];
    ml->moves[0] = ml->moves[1];
    ml->moves[1] = swap;
}

void sort_move_list(MoveList * ml) {
    // An insertion sort is not the most performant,
    // but when the move list has move than one move,
    // performance is not great concern (yet).
    int i = 1;
    int k;
    Move * x;
    while (i < ml->count) {
        x = ml->moves[i];
        k = i - 1;
        while (k >= 0 && x->equity > ml->moves[k]->equity) {
            ml->moves[k+1] = ml->moves[k];
            k--;
        }
        ml->moves[k+1] = x;
        i++;
    }
}
