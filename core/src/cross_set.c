#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "board.h"
#include "constants.h"
#include "cross_set.h"
#include "kwg.h"

int allowed(uint64_t cross_set, uint8_t letter) {
  return (cross_set & ((uint64_t)1 << letter)) != 0;
}

void traverse_backwards(const KWG *kwg, Board *board, int row, int col,
                        uint32_t node_index, bool check_letter_set,
                        int left_most_col) {
  while (pos_exists(row, col)) {
    uint8_t ml = get_letter(board, row, col);
    if (ml == ALPHABET_EMPTY_SQUARE_MARKER) {
      break;
    }

    if (check_letter_set && col == left_most_col) {
      if (kwg_in_letter_set(kwg, ml, node_index)) {
        set_board_node_index(board, node_index);
        set_board_path_is_valid(board, true);
        return;
      }

      set_board_node_index(board, node_index);
      set_board_path_is_valid(board, false);
      return;
    }

    node_index = kwg_get_next_node_index(kwg, node_index,
                                         get_unblanked_machine_letter(ml));
    if (node_index == 0) {
      set_board_node_index(board, node_index);
      set_board_path_is_valid(board, false);
      return;
    }

    col--;
  }

  set_board_node_index(board, node_index);
  set_board_path_is_valid(board, true);
}

void gen_cross_set(const KWG *kwg,
                   const LetterDistribution *letter_distribution, Board *board,
                   int row, int col, int dir, int cross_set_index) {
  if (!pos_exists(row, col)) {
    return;
  }

  if (!is_empty(board, row, col)) {
    set_cross_set(board, row, col, 0, dir, cross_set_index);
    set_cross_score(board, row, col, 0, dir, cross_set_index);
    return;
  }
  if (left_and_right_empty(board, row, col)) {
    set_cross_set(board, row, col, TRIVIAL_CROSS_SET, dir, cross_set_index);
    set_cross_score(board, row, col, 0, dir, cross_set_index);
    return;
  }

  int right_col = word_edge(board, row, col + 1, WORD_DIRECTION_RIGHT);
  if (right_col == col) {
    traverse_backwards(kwg, board, row, col - 1, kwg_get_root_node_index(kwg),
                       false, 0);
    uint32_t lnode_index = get_board_node_index(board);
    int lpath_is_valid = get_board_path_is_valid(board);
    int score =
        traverse_backwards_for_score(board, letter_distribution, row, col - 1);
    set_cross_score(board, row, col, score, dir, cross_set_index);

    if (!lpath_is_valid) {
      set_cross_set(board, row, col, 0, dir, cross_set_index);
      return;
    }
    uint32_t s_index =
        kwg_get_next_node_index(kwg, lnode_index, SEPARATION_MACHINE_LETTER);
    uint64_t letter_set = kwg_get_letter_set(kwg, s_index);
    set_cross_set(board, row, col, letter_set, dir, cross_set_index);
  } else {
    int left_col = word_edge(board, row, col - 1, WORD_DIRECTION_LEFT);
    traverse_backwards(kwg, board, row, right_col, kwg_get_root_node_index(kwg),
                       false, 0);
    uint32_t lnode_index = get_board_node_index(board);
    int lpath_is_valid = get_board_path_is_valid(board);
    int score_r = traverse_backwards_for_score(board, letter_distribution, row,
                                               right_col);
    int score_l =
        traverse_backwards_for_score(board, letter_distribution, row, col - 1);
    set_cross_score(board, row, col, score_r + score_l, dir, cross_set_index);
    if (!lpath_is_valid) {
      set_cross_set(board, row, col, 0, dir, cross_set_index);
      return;
    }
    if (left_col == col) {
      uint64_t letter_set = kwg_get_letter_set(kwg, lnode_index);
      set_cross_set(board, row, col, letter_set, dir, cross_set_index);
    } else {
      uint64_t *cross_set =
          get_cross_set_pointer(board, row, col, dir, cross_set_index);
      *cross_set = 0;
      for (int i = lnode_index;; i++) {
        int t = kwg_tile(kwg, i);
        if (t != 0) {
          int next_node_index = kwg_arc_index(kwg, i);
          traverse_backwards(kwg, board, row, col - 1, next_node_index, true,
                             left_col);
          if (get_board_path_is_valid(board)) {
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

// FIXME: this might belong in game.c
void generate_all_cross_sets(const KWG *kwg_1, const KWG *kwg_2,
                             const LetterDistribution *letter_distribution,
                             Board *board, bool kwgs_are_distinct) {
  for (int i = 0; i < BOARD_DIM; i++) {
    for (int j = 0; j < BOARD_DIM; j++) {
      gen_cross_set(kwg_1, letter_distribution, board, i, j, 0, 0);
      if (kwgs_are_distinct) {
        gen_cross_set(kwg_2, letter_distribution, board, i, j, 0, 1);
      }
    }
  }
  transpose(board);
  for (int i = 0; i < BOARD_DIM; i++) {
    for (int j = 0; j < BOARD_DIM; j++) {
      gen_cross_set(kwg_1, letter_distribution, board, i, j, 1, 0);
      if (kwgs_are_distinct) {
        gen_cross_set(kwg_2, letter_distribution, board, i, j, 1, 1);
      }
    }
  }
  transpose(board);
}