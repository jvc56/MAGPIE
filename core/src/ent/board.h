#ifndef BOARD_H
#define BOARD_H

#include <stdbool.h>
#include <stdint.h>

#include "../def/board_defs.h"

#include "letter_distribution.h"

struct Board;
typedef struct Board Board;

int allowed(uint64_t cross_set, uint8_t letter);

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
bool get_anchor(const Board *board, int row, int col, int dir);
uint8_t get_bonus_square(const Board *board, int row, int col);
int get_cross_score(const Board *board, int row, int col, int dir,
                    int cross_set_index);
uint64_t get_cross_set(const Board *board, int row, int col, int dir,
                       int cross_set_index);
uint64_t *get_cross_set_pointer(Board *board, int row, int col, int dir,
                                int cross_set_index);
uint8_t get_letter(const Board *board, int row, int col);
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
bool get_transpose(const Board *board);
void transpose(Board *board);
void reset_transpose(Board *board);
void set_transpose(Board *board, bool transposed);
int get_tiles_played(const Board *board);
uint32_t get_board_node_index(const Board *board);
bool get_board_path_is_valid(const Board *board);
void set_board_node_index(Board *board, uint32_t value);
void set_board_path_is_valid(Board *board, bool value);
void incrememt_tiles_played(Board *board, int tiles_played);
void update_anchors(Board *board, int row, int col, int dir);
void update_all_anchors(Board *board);
int word_edge(const Board *board, int row, int col, int dir);
int score_move(const Board *board,
               const LetterDistribution *letter_distribution, uint8_t word[],
               int word_start_index, int word_end_index, int row, int col,
               int tiles_played, int cross_dir, int cross_set_index);
int get_cross_set_index(bool kwgs_are_shared, int player_index);

#endif