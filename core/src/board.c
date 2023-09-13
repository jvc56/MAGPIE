#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "constants.h"
#include "cross_set.h"

board_layout_t
board_layout_string_to_board_layout(const char *board_layout_string) {
  if (!strcmp(board_layout_string, "CrosswordGame")) {
    return BOARD_LAYOUT_CROSSWORD_GAME;
  }
  if (!strcmp(board_layout_string, "SuperCrosswordGame")) {
    return BOARD_LAYOUT_SUPER_CROSSWORD_GAME;
  }
  return BOARD_LAYOUT_UNKNOWN;
}

// Current index
// depends on tranposition of the board

int get_tindex(Board *board, int row, int col) {
  return (row * (BOARD_DIM * (1 - board->transposed) + board->transposed)) +
         (col * (BOARD_DIM * board->transposed + (1 - board->transposed)));
}

int get_tindex_dir(Board *board, int row, int col, int dir) {
  return get_tindex(board, row, col) * 2 + dir;
}

// Letters

int is_empty(Board *board, int row, int col) {
  return get_letter(board, row, col) == ALPHABET_EMPTY_SQUARE_MARKER;
}

void set_letter_by_index(Board *board, int index, uint8_t letter) {
  board->letters[index] = letter;
}

uint8_t get_letter_by_index(Board *board, int index) {
  return board->letters[index];
}

void set_letter(Board *board, int row, int col, uint8_t letter) {
  board->letters[get_tindex(board, row, col)] = letter;
}

uint8_t get_letter(Board *board, int row, int col) {
  return board->letters[get_tindex(board, row, col)];
}

// Anchors

int get_anchor(Board *board, int row, int col, int vertical) {
  return board->anchors[get_tindex_dir(board, row, col, vertical)];
}

void set_anchor(Board *board, int row, int col, int vertical) {
  board->anchors[get_tindex_dir(board, row, col, vertical)] = 1;
}

void reset_anchors(Board *board, int row, int col) {
  board->anchors[get_tindex_dir(board, row, col, 0)] = 0;
  board->anchors[get_tindex_dir(board, row, col, 1)] = 0;
}

// Cross sets and scores

uint64_t *get_cross_set_pointer(Board *board, int row, int col, int dir) {
  return &board->cross_sets[get_tindex_dir(board, row, col, dir)];
}

uint64_t get_cross_set(Board *board, int row, int col, int dir) {
  return board->cross_sets[get_tindex_dir(board, row, col, dir)];
}

void set_cross_score(Board *board, int row, int col, int score, int dir) {
  board->cross_scores[get_tindex_dir(board, row, col, dir)] = score;
}

int get_cross_score(Board *board, int row, int col, int dir) {
  return board->cross_scores[get_tindex_dir(board, row, col, dir)];
}

uint8_t get_bonus_square(Board *board, int row, int col) {
  return board->bonus_squares[get_tindex(board, row, col)];
}

void set_cross_set_letter(uint64_t *cross_set, uint8_t letter) {
  *cross_set = *cross_set | (1 << letter);
}

void set_cross_set(Board *board, int row, int col, uint64_t letter, int dir) {
  board->cross_sets[get_tindex_dir(board, row, col, dir)] = letter;
}

void clear_cross_set(Board *board, int row, int col, int dir) {
  board->cross_sets[get_tindex_dir(board, row, col, dir)] = 0;
}

void set_all_crosses(Board *board) {
  for (int i = 0; i < BOARD_DIM; i++) {
    for (int j = 0; j < BOARD_DIM; j++) {
      set_cross_set(board, i, j, TRIVIAL_CROSS_SET, BOARD_HORIZONTAL_DIRECTION);
      set_cross_set(board, i, j, TRIVIAL_CROSS_SET, BOARD_VERTICAL_DIRECTION);
    }
  }
}

void clear_all_crosses(Board *board) {
  for (size_t i = 0; i < BOARD_DIM * BOARD_DIM * 2; i++) {
    board->cross_sets[i] = 0;
  }
}

void reset_all_cross_scores(Board *board) {
  for (size_t i = 0; i < (BOARD_DIM * BOARD_DIM * 2); i++) {
    board->cross_scores[i] = 0;
  }
}

int pos_exists(int row, int col) {
  return row >= 0 && row < BOARD_DIM && col >= 0 && col < BOARD_DIM;
}

int left_and_right_empty(Board *board, int row, int col) {
  return !((pos_exists(row, col - 1) && !is_empty(board, row, col - 1)) ||
           (pos_exists(row, col + 1) && !is_empty(board, row, col + 1)));
}

int word_edge(Board *board, int row, int col, int dir) {
  while (pos_exists(row, col) && !is_empty(board, row, col)) {
    col += dir;
  }
  return col - dir;
}

int traverse_backwards_for_score(Board *board, int row, int col,
                                 LetterDistribution *letter_distribution) {
  int score = 0;
  while (pos_exists(row, col)) {
    uint8_t ml = get_letter(board, row, col);
    if (ml == ALPHABET_EMPTY_SQUARE_MARKER) {
      break;
    }
    if (is_blanked(ml)) {
      score += letter_distribution->scores[BLANK_MACHINE_LETTER];
    } else {
      score += letter_distribution->scores[ml];
    }
    col--;
  }
  return score;
}

void update_anchors(Board *board, int row, int col, int vertical) {
  if (vertical) {
    int temp = row;
    row = col;
    col = temp;
  }

  reset_anchors(board, row, col);
  int tile_above = 0;
  int tile_below = 0;
  int tile_left = 0;
  int tile_right = 0;
  int tile_here = 0;

  if (row > 0) {
    tile_above = !is_empty(board, row - 1, col);
  }
  if (col > 0) {
    tile_left = !is_empty(board, row, col - 1);
  }
  if (row < BOARD_DIM - 1) {
    tile_below = !is_empty(board, row + 1, col);
  }
  if (col < BOARD_DIM - 1) {
    tile_right = !is_empty(board, row, col + 1);
  }
  tile_here = !is_empty(board, row, col);
  if (tile_here) {
    if (!tile_right) {
      set_anchor(board, row, col, 0);
    }
    if (!tile_below) {
      set_anchor(board, row, col, 1);
    }
  } else {
    if (!tile_left && !tile_right && (tile_above || tile_below)) {
      set_anchor(board, row, col, 0);
    }
    if (!tile_above && !tile_below && (tile_left || tile_right)) {
      set_anchor(board, row, col, 1);
    }
  }
}

void update_all_anchors(Board *board) {
  if (board->tiles_played > 0) {
    for (int i = 0; i < BOARD_DIM; i++) {
      for (int j = 0; j < BOARD_DIM; j++) {
        update_anchors(board, i, j, 0);
      }
    }
  } else {
    for (int i = 0; i < BOARD_DIM; i++) {
      for (int j = 0; j < BOARD_DIM; j++) {
        reset_anchors(board, i, j);
      }
    }
    int rc = BOARD_DIM / 2;
    board->anchors[get_tindex_dir(board, rc, rc, 0)] = 1;
  }
}

void reset_board(Board *board) {
  // The transposed field must be set to 0 here because
  // it is used to calculate the index for set_letter.
  board->tiles_played = 0;
  board->transposed = 0;

  for (int i = 0; i < BOARD_DIM; i++) {
    for (int j = 0; j < BOARD_DIM; j++) {
      set_letter(board, i, j, ALPHABET_EMPTY_SQUARE_MARKER);
    }
  }

  set_all_crosses(board);
  reset_all_cross_scores(board);
  update_all_anchors(board);
}

void set_bonus_squares(Board *board) {
  for (int i = 0; i < BOARD_DIM * BOARD_DIM; i++) {
    uint8_t bonus_value;
    char bonus_square = CROSSWORD_GAME_BOARD[i];
    if (bonus_square == BONUS_TRIPLE_WORD_SCORE) {
      bonus_value = 3;
      bonus_value = bonus_value << 4;
      bonus_value += 1;
    } else if (bonus_square == BONUS_DOUBLE_WORD_SCORE) {
      bonus_value = 2;
      bonus_value = bonus_value << 4;
      bonus_value += 1;
    } else if (bonus_square == BONUS_DOUBLE_LETTER_SCORE) {
      bonus_value = 1;
      bonus_value = bonus_value << 4;
      bonus_value += 2;
    } else if (bonus_square == BONUS_TRIPLE_LETTER_SCORE) {
      bonus_value = 1;
      bonus_value = bonus_value << 4;
      bonus_value += 3;
    } else {
      bonus_value = 1;
      bonus_value = bonus_value << 4;
      bonus_value += 1;
    }
    board->bonus_squares[i] = bonus_value;
  }
}

// this fn assumes the word is always horizontal. If this isn't the case,
// the board needs to be transposed ahead of time.
int score_move(Board *board, uint8_t word[], int word_start_index,
               int word_end_index, int row, int col, int tiles_played,
               int cross_dir, LetterDistribution *letter_distribution) {
  int ls;
  int main_word_score = 0;
  int cross_scores = 0;
  int bingo_bonus = 0;
  if (tiles_played == RACK_SIZE) {
    bingo_bonus = BINGO_BONUS;
  }
  int word_multiplier = 1;
  for (int idx = 0; idx < word_end_index - word_start_index + 1; idx++) {
    uint8_t ml = word[idx + word_start_index];
    uint8_t bonus_square = get_bonus_square(board, row, col + idx);
    int letter_multiplier = 1;
    int this_word_multiplier = 1;
    int fresh_tile = 0;
    if (ml == PLAYED_THROUGH_MARKER) {
      ml = get_letter(board, row, col + idx);
    } else {
      fresh_tile = 1;
      this_word_multiplier = bonus_square >> 4;
      letter_multiplier = bonus_square & 0x0F;
      word_multiplier *= this_word_multiplier;
    }
    int cs = get_cross_score(board, row, col + idx, cross_dir);
    if (is_blanked(ml)) {
      ls = 0;
    } else {
      ls = letter_distribution->scores[ml];
    }

    main_word_score += ls * letter_multiplier;
    int actual_cross_word =
        (row > 0 && !is_empty(board, row - 1, col + idx)) ||
        ((row < BOARD_DIM - 1) && !is_empty(board, row + 1, col + idx));
    if (fresh_tile && actual_cross_word) {
      cross_scores += ls * letter_multiplier * this_word_multiplier +
                      cs * this_word_multiplier;
    }
  }
  return main_word_score * word_multiplier + cross_scores + bingo_bonus;
}

void transpose(Board *board) { board->transposed = 1 - board->transposed; }

void set_transpose(Board *board, int transpose) {
  board->transposed = transpose;
}

void reset_transpose(Board *board) { board->transposed = 0; }

Board *create_board() {
  Board *board = malloc(sizeof(Board));
  board->traverse_backwards_return_values =
      malloc(sizeof(TraverseBackwardsReturnValues));
  reset_board(board);
  set_bonus_squares(board);
  return board;
}

Board *copy_board(Board *board) {
  Board *new_board = malloc(sizeof(Board));
  new_board->traverse_backwards_return_values =
      malloc(sizeof(TraverseBackwardsReturnValues));
  copy_board_into(new_board, board);
  return new_board;
}

// copy src into dst; assume dst is already allocated.
void copy_board_into(Board *dst, Board *src) {
  for (int board_index = 0; board_index < (BOARD_DIM * BOARD_DIM);
       board_index++) {
    dst->letters[board_index] = src->letters[board_index];
    dst->bonus_squares[board_index] = src->bonus_squares[board_index];
    int directional_board_index = board_index * 2;
    dst->cross_sets[directional_board_index] =
        src->cross_sets[directional_board_index];
    dst->cross_sets[directional_board_index + 1] =
        src->cross_sets[directional_board_index + 1];
    dst->cross_scores[directional_board_index] =
        src->cross_scores[directional_board_index];
    dst->cross_scores[directional_board_index + 1] =
        src->cross_scores[directional_board_index + 1];
    dst->anchors[directional_board_index] =
        src->anchors[directional_board_index];
    dst->anchors[directional_board_index + 1] =
        src->anchors[directional_board_index + 1];
    if (board_index >= (BOARD_DIM * BOARD_DIM) ||
        directional_board_index + 1 >= (BOARD_DIM * BOARD_DIM) * 2) {
      printf("out of bounds: %d, %d\n", board_index,
             directional_board_index + 1);
      abort();
    }
  }
  dst->transposed = src->transposed;
  dst->tiles_played = src->tiles_played;
}

void destroy_board(Board *board) {
  free(board->traverse_backwards_return_values);
  free(board);
}