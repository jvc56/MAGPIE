#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>

#include "alphabet.h"
#include "board.h"
#include "constants.h"
#include "cross_set.h"
#include "gaddag.h"

int allowed(uint64_t cross_set, uint8_t letter) {
	return (cross_set & (1 << letter)) != 0;
}

void traverse_backwards(Board * board, int row, int col, uint32_t node_index, int check_letter_set, int left_most_col, Gaddag * gaddag) {
	while (pos_exists(row, col)) {
		uint8_t ml = get_letter(board, row, col);
		if (ml == ALPHABET_EMPTY_SQUARE_MARKER) {
			break;
		}

		if (check_letter_set && col == left_most_col) {
			if (in_letter_set(gaddag, ml, node_index)) {
				board->traverse_backwards_return_values->node_index = node_index;
				board->traverse_backwards_return_values->path_is_valid = 1;
				return;
			}
			board->traverse_backwards_return_values->node_index = node_index;
			board->traverse_backwards_return_values->path_is_valid = 0;
			return;
		}

		node_index = get_next_node_index(gaddag, node_index, get_unblanked_machine_letter(ml));
		if (node_index == 0) {
			board->traverse_backwards_return_values->node_index = node_index;
			board->traverse_backwards_return_values->path_is_valid = 0;
			return;
		}

		col--;
	}
	board->traverse_backwards_return_values->node_index = node_index;
	board->traverse_backwards_return_values->path_is_valid = 1;
}

void gen_cross_set(Board * board, int row, int col, int dir, Gaddag * gaddag, LetterDistribution * letter_distribution) {
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
		traverse_backwards(board, row, col-1, 0, 0, 0, gaddag);
		uint32_t lnode_index = board->traverse_backwards_return_values->node_index;
		int lpath_is_valid = board->traverse_backwards_return_values->path_is_valid;
		int score = traverse_backwards_for_score(board, row, col-1, letter_distribution);
		set_cross_score(board, row, col, score, dir);

		if (!lpath_is_valid) {
			set_cross_set(board, row, col, 0, dir);
			return;
		}
		uint32_t s_index = get_next_node_index(gaddag, lnode_index, SEPARATION_MACHINE_LETTER);
		uint64_t letter_set = get_letter_set(gaddag, s_index);
		set_cross_set(board, row, col, letter_set, dir);
	} else {
		int left_col = word_edge(board, row, col-1, WORD_DIRECTION_LEFT);
		traverse_backwards(board, row, right_col, 0, 0, 0, gaddag);
		uint32_t lnode_index = board->traverse_backwards_return_values->node_index;
		int lpath_is_valid = board->traverse_backwards_return_values->path_is_valid;
		int score_r = traverse_backwards_for_score(board, row, right_col, letter_distribution);
		int score_l = traverse_backwards_for_score(board, row, col-1, letter_distribution);
		set_cross_score(board, row, col, score_r+score_l, dir);
		if (!lpath_is_valid) {
			set_cross_set(board, row, col, 0, dir);
			return;
		}
		if (left_col == col) {
			uint64_t letter_set = get_letter_set(gaddag, lnode_index);
			set_cross_set(board, row, col, letter_set, dir);
		} else {
			uint8_t number_of_arcs = get_number_of_arcs(gaddag, lnode_index);
			uint64_t * cross_set = get_cross_set_pointer(board, row, col, dir);
			*cross_set = 0;
			for (uint32_t i = lnode_index + 1; i <= number_of_arcs + lnode_index; i++) {
				uint8_t ml = (gaddag->nodes[i] >> GADDAG_LETTER_BIT_LOC);
				if (ml == SEPARATION_MACHINE_LETTER) {
					continue;
				}
				uint32_t next_node_index = gaddag->nodes[i] & (GADDAG_NODE_IDX_BIT_MASK);
				traverse_backwards(board, row, col-1, next_node_index, 1, left_col, gaddag);
				lpath_is_valid = board->traverse_backwards_return_values->path_is_valid;
				if (lpath_is_valid) {
					set_cross_set_letter(cross_set, ml);
				}
			}
		}
	}
}

void generate_all_cross_sets(Board * board, Gaddag * gaddag, LetterDistribution * letter_distribution) {
	for (int i = 0; i < BOARD_DIM; i++) {
		for (int j = 0; j < BOARD_DIM; j++) {
			gen_cross_set(board, i, j, 0, gaddag, letter_distribution);
		}
	}
	transpose(board);
	for (int i = 0; i < BOARD_DIM; i++) {
		for (int j = 0; j < BOARD_DIM; j++) {
			gen_cross_set(board, i, j, 1, gaddag, letter_distribution);
		}
	}
	transpose(board);
}