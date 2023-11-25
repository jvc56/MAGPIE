#ifndef BOARD_H
#define BOARD_H

#include <stdbool.h>
#include <stdint.h>

#include "constants.h"
#include "letter_distribution.h"

#define BOARD_DIM 15
// Use 2 * 2 for
// vert and horizontal sets and
// player 1 and player 2 sets, when using different lexica
#define NUMBER_OF_CROSSES BOARD_DIM *BOARD_DIM * 2 * 2
#define BOARD_HORIZONTAL_DIRECTION 0
#define BOARD_VERTICAL_DIRECTION 1

// TODO: read this from file to make it easier to configure custom boards
#define CROSSWORD_GAME_BOARD                                                   \
  "=  '   =   '  ="                                                            \
  " -   \"   \"   - "                                                          \
  "  -   ' '   -  "                                                            \
  "'  -   '   -  '"                                                            \
  "    -     -    "                                                            \
  " \"   \"   \"   \" "                                                        \
  "  '   ' '   '  "                                                            \
  "=  '   -   '  ="                                                            \
  "  '   ' '   '  "                                                            \
  " \"   \"   \"   \" "                                                        \
  "    -     -    "                                                            \
  "'  -   '   -  '"                                                            \
  "  -   ' '   -  "                                                            \
  " -   \"   \"   - "                                                          \
  "=  '   =   '  ="

typedef struct TraverseBackwardsReturnValues {
  uint32_t node_index;
  bool path_is_valid;
} TraverseBackwardsReturnValues;

typedef struct Board {
  uint8_t letters[BOARD_DIM * BOARD_DIM];
  uint8_t bonus_squares[BOARD_DIM * BOARD_DIM];

  uint64_t cross_sets[NUMBER_OF_CROSSES];
  int cross_scores[NUMBER_OF_CROSSES];
  int anchors[BOARD_DIM * BOARD_DIM * 2];
  bool transposed;
  int tiles_played;
  TraverseBackwardsReturnValues *traverse_backwards_return_values;
} Board;

bool dir_is_vertical(int dir);
board_layout_t
board_layout_string_to_board_layout(const char *board_layout_string);
void clear_all_crosses(Board *board);
void clear_cross_set(Board *board, int row, int col, int dir,
                     int cross_set_index);
Board *create_board();
Board *board_duplicate(const Board *board);
void board_copy(Board *dst, const Board *src);
void destroy_board(Board *board);
int get_anchor(const Board *board, int row, int col, int dir);
uint8_t get_bonus_square(const Board *board, int row, int col);
int get_cross_score(const Board *board, int row, int col, int dir,
                    int cross_set_index);
uint64_t get_cross_set(const Board *board, int row, int col, int dir,
                       int cross_set_index);
uint64_t *get_cross_set_pointer(Board *board, int row, int col, int dir,
                                int cross_set_index);
uint8_t get_letter(const Board *board, int row, int col);
uint8_t get_letter_by_index(const Board *board, int index);
bool is_empty(const Board *board, int row, int col);
bool left_and_right_empty(const Board *board, int row, int col);
bool pos_exists(int row, int col);
void reset_board(Board *board);
void set_all_crosses(Board *board);
void set_cross_score(Board *board, int row, int col, int score, int dir,
                     int cross_set_index);
void set_cross_set(Board *board, int row, int col, uint64_t letter, int dir,
                   int cross_set_index);
void set_cross_set_letter(uint64_t *cross_set, uint8_t letter);
void set_letter(Board *board, int row, int col, uint8_t letter);
void set_letter_by_index(Board *board, int index, uint8_t letter);
void transpose(Board *board);
void reset_transpose(Board *board);
void set_transpose(Board *board, bool transposed);
int traverse_backwards_for_score(const Board *board,
                                 const LetterDistribution *letter_distribution,
                                 int row, int col);
void update_anchors(Board *board, int row, int col, int dir);
void update_all_anchors(Board *board);
int word_edge(const Board *board, int row, int col, int dir);

#endif