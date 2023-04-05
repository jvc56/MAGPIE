#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>

#include "alphabet.h"
#include "board.h"
#include "constants.h"
#include "cross_set.h"
#include "kwg.h"

int allowed(uint64_t cross_set, uint8_t letter) {
	return (cross_set & (1 << letter)) != 0;
}

void traverse_backwards(Board * board, int row, int col, uint32_t node_index, int check_letter_set, int left_most_col, KWG * kwg) {
	while (pos_exists(row, col)) {
		uint8_t ml = get_letter(board, row, col);
		if (ml == ALPHABET_EMPTY_SQUARE_MARKER) {
			break;
		}

		if (check_letter_set && col == left_most_col) {
			if (kwg_in_letter_set(kwg, ml, node_index)) {
				board->node_index = node_index;
				board->path_is_valid = 1;
				return;
			}
			board->node_index = node_index;
			board->path_is_valid = 0;
			return;
		}

		node_index = kwg_get_next_node_index(kwg, node_index, get_unblanked_machine_letter(ml));
		if (node_index == 0) {
			board->node_index = node_index;
			board->path_is_valid = 0;
			return;
		}

		col--;
	}
	board->node_index = node_index;
	board->path_is_valid = 1;
}

void gen_cross_set(Board * board, int row, int col, int dir, KWG * kwg, LetterDistribution * letter_distribution) {
	if (!pos_exists(row, col)) {
		return;
	}

	if (!is_empty(board, row, col)) {
		set_cross_set(board, row, col, 0, dir);
		set_cross_score(board, row, col, 0, dir);
		return;
	}
	if (left_and_right_empty(board, row, col)) {
		set_cross_set(board, row, col, TRIVIAL_CROSS_SET, dir);
		set_cross_score(board, row, col, 0, dir);
		return;
	}

	int right_col = word_edge(board, row, col+1, WORD_DIRECTION_RIGHT);
	if (right_col == col) {
		traverse_backwards(board, row, col-1, kwg_get_root_node_index(kwg), 0, 0, kwg);
		int score = traverse_backwards_for_score(board, row, col-1, letter_distribution);
		set_cross_score(board, row, col, score, dir);

		if (!board->path_is_valid) {
			set_cross_set(board, row, col, 0, dir);
			return;
		}
		uint32_t s_index = kwg_get_next_node_index(kwg, board->node_index, SEPARATION_MACHINE_LETTER);
		uint64_t letter_set = kwg_get_letter_set(kwg, s_index);
		set_cross_set(board, row, col, letter_set, dir);
	} else {
		int left_col = word_edge(board, row, col-1, WORD_DIRECTION_LEFT);
		traverse_backwards(board, row, right_col, kwg_get_root_node_index(kwg), 0, 0, kwg);
		int score_r = traverse_backwards_for_score(board, row, right_col, letter_distribution);
		int score_l = traverse_backwards_for_score(board, row, col-1, letter_distribution);
		set_cross_score(board, row, col, score_r+score_l, dir);
		if (!board->path_is_valid) {
			set_cross_set(board, row, col, 0, dir);
			return;
		}
		if (left_col == col) {
			uint64_t letter_set = kwg_get_letter_set(kwg, board->node_index);
			set_cross_set(board, row, col, letter_set, dir);
		} else {
			uint64_t * cross_set = get_cross_set_pointer(board, row, col, dir);
			*cross_set = 0;
			for(int i = board->node_index; ;i++) {
				int t = kwg_tile(kwg, i);
				if (t != 0) {
					int next_node_index = kwg_arc_index(kwg, i);
					traverse_backwards(board, row, col-1, next_node_index, 1, left_col, kwg);
					if (board->path_is_valid) {
						set_cross_set_letter(cross_set, t);
					}
				}
				if (kwg_is_end(kwg, i)) {
					break;
				}
			}
		}
	}
}

void generate_all_cross_sets(Board * board, KWG * kwg, LetterDistribution * letter_distribution) {
	for (int i = 0; i < BOARD_DIM; i++) {
		for (int j = 0; j < BOARD_DIM; j++) {
			gen_cross_set(board, i, j, 0, kwg, letter_distribution);
		}
	}
	transpose(board);
	for (int i = 0; i < BOARD_DIM; i++) {
		for (int j = 0; j < BOARD_DIM; j++) {
			gen_cross_set(board, i, j, 1, kwg, letter_distribution);
		}
	}
	transpose(board);
}