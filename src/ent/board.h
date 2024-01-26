#ifndef BOARD_H
#define BOARD_H

#include <stdbool.h>
#include <stdint.h>

#include "../def/board_defs.h"

#include "letter_distribution.h"

typedef struct Board Board;

Board *board_create();
Board *board_duplicate(const Board *board);
void board_copy(Board *dst, const Board *src);
void board_destroy(Board *board);

bool board_is_empty(const Board *board, int row, int col);
bool board_are_left_and_right_empty(const Board *board, int row, int col);
bool board_is_position_valid(int row, int col);
int board_is_letter_allowed_in_cross_set(uint64_t cross_set, uint8_t letter);
bool board_is_dir_vertical(int dir);

bool board_get_anchor(const Board *board, int row, int col, int dir);
uint8_t board_get_bonus_square(const Board *board, int row, int col);
int board_get_cross_score(const Board *board, int row, int col, int dir,
                          int cross_set_index);
uint64_t board_get_cross_set(const Board *board, int row, int col, int dir,
                             int cross_set_index);
uint64_t *board_get_cross_set_pointer(Board *board, int row, int col, int dir,
                                      int cross_set_index);
uint8_t board_get_letter(const Board *board, int row, int col);
bool board_get_transposed(const Board *board);
int board_get_tiles_played(const Board *board);

uint32_t board_get_node_index(const Board *board);
bool board_get_path_is_valid(const Board *board);
int board_get_word_edge(const Board *board, int row, int col, int dir);
board_layout_t
board_layout_string_to_board_layout(const char *board_layout_string);
int board_score_move(const Board *board, const LetterDistribution *ld,
                     uint8_t word[], int word_start_index, int word_end_index,
                     int row, int col, int tiles_played, int cross_dir,
                     int cross_set_index);
int board_get_cross_set_index(bool kwgs_are_shared, int player_index);

void board_clear_cross_set(Board *board, int row, int col, int dir,
                           int cross_set_index);
void board_reset(Board *board);
void board_set_all_crosses(Board *board);
void board_set_cross_score(Board *board, int row, int col, int score, int dir,
                           int cross_set_index);
void board_set_cross_set(Board *board, int row, int col, uint64_t letter,
                         int dir, int cross_set_index);
void board_set_cross_set_letter(uint64_t *cross_set, uint8_t letter);
void board_set_letter(Board *board, int row, int col, uint8_t letter);
void board_transpose(Board *board);
void board_set_transposed(Board *board, bool transposed);

int board_get_tindex_dir(const Board *board, int row, int col, int dir);
void board_set_node_index(Board *board, uint32_t value);
void board_set_path_is_valid(Board *board, bool value);
void board_increment_tiles_played(Board *board, int tiles_played);
void board_update_anchors(Board *board, int row, int col, int dir);
void board_update_all_anchors(Board *board);

#endif