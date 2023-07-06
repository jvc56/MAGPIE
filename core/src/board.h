#ifndef BOARD_H
#define BOARD_H

#include <stdint.h>

#include "constants.h"
#include "letter_distribution.h"

typedef struct TraverseBackwardsReturnValues {
  uint32_t node_index;
  int path_is_valid;
} TraverseBackwardsReturnValues;
typedef struct Board {
  uint8_t letters[BOARD_DIM * BOARD_DIM];
  uint8_t bonus_squares[BOARD_DIM * BOARD_DIM];
  uint64_t cross_sets[BOARD_DIM * BOARD_DIM * 2];
  int cross_scores[BOARD_DIM * BOARD_DIM * 2];
  int anchors[BOARD_DIM * BOARD_DIM * 2];
  int transposed;
  int tiles_played;
  TraverseBackwardsReturnValues *traverse_backwards_return_values;
} Board;

void clear_all_crosses(Board *board);
void clear_cross_set(Board *board, int row, int col, int dir);
Board *create_board();
Board *copy_board(Board *board);
void copy_board_into(Board *dst, Board *src);
void destroy_board(Board *board);
int get_anchor(Board *board, int row, int col, int vertical);
uint8_t get_bonus_square(Board *board, int row, int col);
int get_cross_score(Board *board, int row, int col, int dir);
uint64_t get_cross_set(Board *board, int row, int col, int dir);
uint64_t *get_cross_set_pointer(Board *board, int row, int col, int dir);
uint8_t get_letter(Board *board, int row, int col);
uint8_t get_letter_by_index(Board *board, int index);
int is_empty(Board *board, int row, int col);
int left_and_right_empty(Board *board, int row, int col);
int pos_exists(int row, int col);
void reset_board(Board *board);
void set_all_crosses(Board *board);
void set_cross_score(Board *board, int row, int col, int score, int dir);
void set_cross_set(Board *board, int row, int col, uint64_t letter, int dir);
void set_cross_set_letter(uint64_t *cross_set, uint8_t letter);
void set_letter(Board *board, int row, int col, uint8_t letter);
void set_letter_by_index(Board *board, int index, uint8_t letter);
int score_move(Board *board, uint8_t word[], int word_start_index,
               int word_end_index, int row, int col, int tiles_played,
               int cross_dir, LetterDistribution *letter_distribution);
void transpose(Board *board);
void reset_transpose(Board *board);
void set_transpose(Board *board, int transpose);
int traverse_backwards_for_score(Board *board, int row, int col,
                                 LetterDistribution *letter_distribution);
void update_anchors(Board *board, int row, int col, int vertical);
void update_all_anchors(Board *board);
int word_edge(Board *board, int row, int col, int dir);

#endif