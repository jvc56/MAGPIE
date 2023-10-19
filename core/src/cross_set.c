#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "board.h"
#include "constants.h"
#include "cross_set.h"
#include "kwg.h"

int allowed(uint64_t cross_set, uint8_t letter) {
  return (cross_set & (1 << letter)) != 0;
}

void traverse_backwards(Board *board, int row, int col, uint32_t node_index,
                        int check_letter_set, int left_most_col, KWG *kwg) {
  while (pos_exists(row, col)) {
    uint8_t ml = get_letter(board, row, col);
    if (ml == ALPHABET_EMPTY_SQUARE_MARKER) {
      break;
    }

    if (check_letter_set && col == left_most_col) {
      if (kwg_in_letter_set(kwg, ml, node_index)) {
        board->traverse_backwards_return_values->node_index = node_index;
        board->traverse_backwards_return_values->path_is_valid = 1;
        return;
      }
      board->traverse_backwards_return_values->node_index = node_index;
      board->traverse_backwards_return_values->path_is_valid = 0;
      return;
    }

    node_index = kwg_get_next_node_index(kwg, node_index,
                                         get_unblanked_machine_letter(ml));
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

void gen_cross_set(Board *board, int row, int col, int dir, int cross_set_index,
                   KWG *kwg, LetterDistribution *letter_distribution) {
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
    traverse_backwards(board, row, col - 1, kwg_get_root_node_index(kwg), 0, 0,
                       kwg);
    uint32_t lnode_index = board->traverse_backwards_return_values->node_index;
    int lpath_is_valid = board->traverse_backwards_return_values->path_is_valid;
    int score =
        traverse_backwards_for_score(board, row, col - 1, letter_distribution);
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
    traverse_backwards(board, row, right_col, kwg_get_root_node_index(kwg), 0,
                       0, kwg);
    uint32_t lnode_index = board->traverse_backwards_return_values->node_index;
    int lpath_is_valid = board->traverse_backwards_return_values->path_is_valid;
    int score_r = traverse_backwards_for_score(board, row, right_col,
                                               letter_distribution);
    int score_l =
        traverse_backwards_for_score(board, row, col - 1, letter_distribution);
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
          traverse_backwards(board, row, col - 1, next_node_index, 1, left_col,
                             kwg);
          if (board->traverse_backwards_return_values->path_is_valid) {
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
void generate_all_cross_sets(Board *board, KWG *kwg_1, KWG *kwg_2,
                             LetterDistribution *letter_distribution,
                             bool kwgs_are_distinct) {
  for (int i = 0; i < BOARD_DIM; i++) {
    for (int j = 0; j < BOARD_DIM; j++) {
      gen_cross_set(board, i, j, 0, 0, kwg_1, letter_distribution);
      if (kwgs_are_distinct) {
        gen_cross_set(board, i, j, 0, 1, kwg_2, letter_distribution);
      }
    }
  }
  transpose(board);
  for (int i = 0; i < BOARD_DIM; i++) {
    for (int j = 0; j < BOARD_DIM; j++) {
      gen_cross_set(board, i, j, 1, 0, kwg_1, letter_distribution);
      if (kwgs_are_distinct) {
        gen_cross_set(board, i, j, 1, 1, kwg_2, letter_distribution);
      }
    }
  }
  transpose(board);
}