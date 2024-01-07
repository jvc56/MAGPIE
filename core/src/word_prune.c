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
    if (row == 0 || compare_board_rows(&board_rows->rows[row],
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

// max consecutive empty spaces not touching a tile
int max_nonplaythrough_spaces_in_row(BoardRow* board_row) {
  int max_empty_spaces = 0;
  int empty_spaces = 0;
  for (int i = 0; i < BOARD_DIM; i++) {
    if (board_row->letters[i] == 0) {
      empty_spaces++;
    } else {
      // the last empty space doesn't count: it's touching a tile
      empty_spaces--;
      if (empty_spaces > max_empty_spaces) {
        max_empty_spaces = empty_spaces;
      }
      // start at -1, the first empty space won't count: it's touching a tile
      empty_spaces = -1;
    }
  }
  // check the final streak of empty spaces. because it's up against the edge
  // of the board we don't need to decrement: that last square isn't touching
  // a tile.
  if (empty_spaces > max_empty_spaces) {
    max_empty_spaces = empty_spaces;
  }
  return max_empty_spaces;
}

void add_word(PossibleWordList* possible_word_list, uint8_t* word,
              int word_length) {
  if (possible_word_list->num_words == possible_word_list->capacity) {
    possible_word_list->capacity *= 2;
    possible_word_list->possible_words =
        realloc_or_die(possible_word_list->possible_words,
                       sizeof(PossibleWord) * possible_word_list->capacity);
  }
  PossibleWord* possible_word =
      &possible_word_list->possible_words[possible_word_list->num_words];
  possible_word_list->num_words++;
  memory_copy(possible_word->word, word, word_length);
  possible_word->word_length = word_length;
}

void add_words_without_playthrough(const KWG* kwg, uint32_t node_index,
                                   Rack* bag_as_rack, int max_nonplaythrough,
                                   uint8_t* word, int tiles_played,
                                   bool accepts,
                                   PossibleWordList* possible_word_list) {
  if (accepts) {
    add_word(possible_word_list, word, tiles_played);
  }
  if (tiles_played == max_nonplaythrough) {
    return;
  }
  if (node_index == 0) {
    return;
  }
  for (int i = node_index;; i++) {
    const int ml = kwg_tile(kwg, i);
    const int new_node_index = kwg_arc_index(kwg, i);
    if ((bag_as_rack->array[ml] != 0) || (bag_as_rack->array[BLANK_MACHINE_LETTER] != 0)) {
      int accepts = kwg_accepts(kwg, i);
      // Manipulating the rack's array directly is a little bit
      // dirty, and doesn't update rack->number_of_letters or
      // rack->empty, but those aren't used here, and the original
      // rack will be restored at the end.
      if (bag_as_rack->array[ml] > 0) {
        bag_as_rack->array[ml]--;
        word[tiles_played] = ml;
        add_words_without_playthrough(kwg, new_node_index, bag_as_rack,
                                      max_nonplaythrough, word, tiles_played + 1,
                                      accepts, possible_word_list);
        bag_as_rack->array[ml]++;
      } else if (bag_as_rack->array[BLANK_MACHINE_LETTER] > 0) {
        bag_as_rack->array[BLANK_MACHINE_LETTER]--;
        word[tiles_played] = ml;
        add_words_without_playthrough(kwg, new_node_index, bag_as_rack,
                                      max_nonplaythrough, word, tiles_played + 1,
                                      accepts, possible_word_list);
        bag_as_rack->array[BLANK_MACHINE_LETTER]++;
      }
    }
    if (kwg_is_end(kwg, i)) {
      break;
    }
  }
}

/*
void add_playthrough_words_from_row(BoardRow* board_row, const KWG* kwg,
                                    Rack* bag_as_rack,
                                    PossibleWordList* possible_word_list) {}
*/

PossibleWordList* create_empty_possible_word_list() {
  PossibleWordList* possible_word_list =
      malloc_or_die(sizeof(PossibleWordList));
  possible_word_list->capacity = POSSIBLE_WORDS_INITIAL_CAPACITY;
  possible_word_list->possible_words =
      malloc_or_die(sizeof(PossibleWord) * possible_word_list->capacity);
  possible_word_list->num_words = 0;
  return possible_word_list;
}

PossibleWordList* create_possible_word_list(const Game* game,
                                            const KWG* override_kwg) {
  const KWG* kwg = override_kwg;
  if (kwg == NULL) {
    const Player* player = game->players[game->player_on_turn_index];
    kwg = player->kwg;
  }
  PossibleWordList* possible_word_list = create_empty_possible_word_list();

  Rack* bag_as_rack = create_rack(game->gen->letter_distribution->size);
  add_bag_to_rack(game->gen->bag, bag_as_rack);

  // actually direction-agnostic: both rows and columns together
  BoardRows* board_rows = create_board_rows(game);

  int max_nonplaythrough_spaces = 0;
  for (int i = 0; i < board_rows->num_rows; i++) {
    int nonplaythrough_spaces =
        max_nonplaythrough_spaces_in_row(&board_rows->rows[i]);
    if (nonplaythrough_spaces > max_nonplaythrough_spaces) {
      max_nonplaythrough_spaces = nonplaythrough_spaces;
    }
  }

  uint8_t word[BOARD_DIM];
  add_words_without_playthrough(kwg, kwg_get_dawg_root_node_index(kwg),
                                bag_as_rack, max_nonplaythrough_spaces, word, 0,
                                false, possible_word_list);

  /*
    for (int i = 0; i < board_rows->num_rows; i++) {
      add_playthrough_words_from_row(&board_rows->rows[i], kwg, bag_as_rack,
                                     possible_word_list);
    }
  */

  destroy_rack(bag_as_rack);
  destroy_board_rows(board_rows);
  return possible_word_list;
}

void destroy_possible_word_list(PossibleWordList* possible_word_list) {
  free(possible_word_list->possible_words);
  free(possible_word_list);
}
