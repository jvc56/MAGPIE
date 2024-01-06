#include "word_prune.h"

#include <stdlib.h>

#include "util.h"

#define POSSIBLE_WORDS_INITIAL_CAPACITY 1000

int compare_board_rows(const void* a, const void* b) {
  const BoardRow* row_a = (const BoardRow*)a;
  const BoardRow* row_b = (const BoardRow*)b;
  for (int i = 0; i < BOARD_DIM; i++) {
    if (row_a->letters[i] < row_b->letters[i]) {
      return -1;
    } else if (row_a->letters[i] > row_b->letters[i]) {
      return 1;
    }
  }
  return 0;
}

int unique_rows(BoardRows* board_rows) {
  int unique_rows = 0;
  for (int row = board_rows->num_rows - 1; row >= 0; row--) {
    if (row == 0 ||
        compare_board_rows(&board_rows->rows[row],
                           &board_rows->rows[row - 1]) != 0) {
      unique_rows++;
    } else {
      // copy rows to replace duplicate
      for (int row_to_move = row + 1; row_to_move < board_rows->num_rows;
           row_to_move++) {
        memory_copy(&board_rows->rows[row_to_move - 1],
                    &board_rows->rows[row_to_move], sizeof(BoardRow));
      }
    }
  }
  return unique_rows;
}

BoardRows* create_board_rows(const Game* game) {
  BoardRows* container = malloc_or_die(sizeof(BoardRows));
  BoardRow* rows = container->rows;
  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      rows[row].letters[col] = get_letter(game->gen->board, row, col);
    }
  }
  for (int col = 0; col < BOARD_DIM; col++) {
    for (int row = 0; row < BOARD_DIM; row++) {
      rows[BOARD_DIM + col].letters[row] =
          get_letter(game->gen->board, row, col);
    }
  }
  container->num_rows = BOARD_DIM * 2;
  qsort(rows, BOARD_DIM * 2, sizeof(BoardRow), compare_board_rows);
  container->num_rows = unique_rows(container);
  return container;
}

void destroy_board_rows(BoardRows* board_rows) { free(board_rows); }

PossibleWordList* create_possible_word_list(const Game* game, const KWG* kwg) {
  PossibleWordList* possible_word_list =
      malloc_or_die(sizeof(PossibleWordList));
  possible_word_list->capacity = POSSIBLE_WORDS_INITIAL_CAPACITY;
  possible_word_list->possible_words =
      malloc_or_die(sizeof(PossibleWord) * possible_word_list->capacity);
  possible_word_list->num_words = 0;

  // actually direction-agnostic: both rows and columns together
  BoardRows* board_rows = create_board_rows(game);

  destroy_board_rows(board_rows);
  return possible_word_list;
}

void destroy_possible_word_list(PossibleWordList* possible_word_list) {
  free(possible_word_list->possible_words);
  free(possible_word_list);
}
