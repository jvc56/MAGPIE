#include "word_prune.h"

#include <stdlib.h>

#include "../def/cross_set_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../util/string_util.h"
#include "../util/util.h"

int compare_board_rows(const void *a, const void *b) {
  const BoardRow *row_a = (const BoardRow *)a;
  const BoardRow *row_b = (const BoardRow *)b;
  for (int i = 0; i < BOARD_DIM; i++) {
    if (row_a->letters[i] < row_b->letters[i]) {
      return -1;
    } else if (row_a->letters[i] > row_b->letters[i]) {
      return 1;
    }
  }
  return 0;
}

int unique_rows(BoardRows *board_rows) {
  int unique_rows = 0;
  for (int row = board_rows->num_rows - 1; row >= 0; row--) {
    if (row == 0 || compare_board_rows(&board_rows->rows[row],
                                       &board_rows->rows[row - 1]) != 0) {
      unique_rows++;
    } else {
      // copy rows to replace duplicate
      for (int row_to_move = row + 1; row_to_move < board_rows->num_rows;
           row_to_move++) {
        memcpy(&board_rows->rows[row_to_move - 1],
               &board_rows->rows[row_to_move], sizeof(BoardRow));
      }
    }
  }
  return unique_rows;
}

BoardRows *board_rows_create(const Game *game) {
  BoardRows *container = malloc_or_die(sizeof(BoardRows));
  BoardRow *rows = container->rows;
  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      uint8_t letter = board_get_letter(game_get_board(game), row, col);
      uint8_t unblanked = get_unblanked_machine_letter(letter);
      rows[row].letters[col] = unblanked;
    }
  }
  for (int col = 0; col < BOARD_DIM; col++) {
    for (int row = 0; row < BOARD_DIM; row++) {
      uint8_t letter = board_get_letter(game_get_board(game), row, col);
      uint8_t unblanked = get_unblanked_machine_letter(letter);
      rows[BOARD_DIM + col].letters[row] = unblanked;
    }
  }
  container->num_rows = BOARD_DIM * 2;
  qsort(rows, BOARD_DIM * 2, sizeof(BoardRow), compare_board_rows);
  container->num_rows = unique_rows(container);
  return container;
}

void board_rows_destroy(BoardRows *board_rows) { free(board_rows); }

// max consecutive empty spaces not touching a tile
int max_nonplaythrough_spaces_in_row(BoardRow *board_row) {
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

void add_playthrough_word(DictionaryWordList *possible_word_list,
                          uint8_t *strip, int leftstrip, int rightstrip) {
  const int word_length = rightstrip - leftstrip + 1;
  dictionary_word_list_add_word(possible_word_list, strip + leftstrip,
                                word_length);
}

void add_words_without_playthrough(const KWG *kwg, uint32_t node_index,
                                   Rack *rack, int max_nonplaythrough,
                                   uint8_t *word, int tiles_played,
                                   bool accepts,
                                   DictionaryWordList *possible_word_list) {
  if (accepts) {
    dictionary_word_list_add_word(possible_word_list, word, tiles_played);
  }
  if (tiles_played == max_nonplaythrough) {
    return;
  }
  if (node_index == 0) {
    return;
  }
  for (uint32_t i = node_index;; i++) {
    const uint32_t node = kwg_node(kwg, i);
    const uint8_t ml = kwg_node_tile(node);
    const int new_node_index = kwg_node_arc_index_prefetch(node, kwg);
    if ((rack_get_letter(rack, ml) > 0) ||
        (rack_get_letter(rack, BLANK_MACHINE_LETTER) > 0)) {
      bool node_accepts = kwg_node_accepts(node);
      if (rack_get_letter(rack, ml) > 0) {
        rack_take_letter(rack, ml);
        word[tiles_played] = ml;
        add_words_without_playthrough(
            kwg, new_node_index, rack, max_nonplaythrough, word,
            tiles_played + 1, node_accepts, possible_word_list);
        rack_add_letter(rack, ml);
      } else if (rack_get_letter(rack, BLANK_MACHINE_LETTER) > 0) {
        rack_take_letter(rack, BLANK_MACHINE_LETTER);
        word[tiles_played] = ml;
        add_words_without_playthrough(
            kwg, new_node_index, rack, max_nonplaythrough, word,
            tiles_played + 1, node_accepts, possible_word_list);
        rack_add_letter(rack, BLANK_MACHINE_LETTER);
      }
    }
    if (kwg_node_is_end(node)) {
      break;
    }
  }
}

void playthrough_words_go_on(const BoardRow *board_row, const KWG *kwg,
                             Rack *rack, int current_col, int anchor_col,
                             uint8_t current_letter, uint32_t new_node_index,
                             bool accepts, int leftstrip, int rightstrip,
                             int leftmost_col, int tiles_played, uint8_t *strip,
                             DictionaryWordList *possible_word_list);

void playthrough_words_recursive_gen(const BoardRow *board_row, const KWG *kwg,
                                     Rack *rack, int col, int anchor_col,
                                     uint32_t node_index, int leftstrip,
                                     int rightstrip, int leftmost_col,
                                     int tiles_played, uint8_t *strip,
                                     DictionaryWordList *possible_word_list) {
  const uint8_t current_letter = board_row->letters[col];
  if (current_letter != ALPHABET_EMPTY_SQUARE_MARKER) {
    const uint8_t ml = current_letter; // already unblanked
    uint32_t next_node_index = 0;
    bool accepts = false;
    for (uint32_t i = node_index;; i++) {
      const uint32_t node = kwg_node(kwg, i);
      if (kwg_node_tile(node) == ml) {
        next_node_index = kwg_node_arc_index_prefetch(node, kwg);
        accepts = kwg_node_accepts(node);
        break;
      }
      if (kwg_node_is_end(node)) {
        break;
      }
    }
    playthrough_words_go_on(board_row, kwg, rack, col, anchor_col,
                            current_letter, next_node_index, accepts, leftstrip,
                            rightstrip, leftmost_col, tiles_played, strip,
                            possible_word_list);
  } else if (!rack_is_empty(rack)) {
    for (uint32_t i = node_index;; i++) {
      const uint32_t node = kwg_node(kwg, i);
      const uint8_t ml = kwg_node_tile(node);
      if (ml != SEPARATION_MACHINE_LETTER) {
        if (rack_get_letter(rack, ml) > 0) {
          const uint32_t next_node_index =
              kwg_node_arc_index_prefetch(node, kwg);
          const bool accepts = kwg_node_accepts(node);
          rack_take_letter(rack, ml);
          playthrough_words_go_on(board_row, kwg, rack, col, anchor_col, ml,
                                  next_node_index, accepts, leftstrip,
                                  rightstrip, leftmost_col, tiles_played + 1,
                                  strip, possible_word_list);
          rack_add_letter(rack, ml);
        } else if (rack_get_letter(rack, BLANK_MACHINE_LETTER) > 0) {
          const uint32_t next_node_index =
              kwg_node_arc_index_prefetch(node, kwg);
          const bool accepts = kwg_node_accepts(node);
          rack_take_letter(rack, BLANK_MACHINE_LETTER);
          playthrough_words_go_on(board_row, kwg, rack, col, anchor_col, ml,
                                  next_node_index, accepts, leftstrip,
                                  rightstrip, leftmost_col, tiles_played + 1,
                                  strip, possible_word_list);
          rack_add_letter(rack, BLANK_MACHINE_LETTER);
        }
      }
      if (kwg_node_is_end(node)) {
        break;
      }
    }
  }
}

void playthrough_words_go_on(const BoardRow *board_row, const KWG *kwg,
                             Rack *rack, int current_col, int anchor_col,
                             uint8_t current_letter, uint32_t new_node_index,
                             bool accepts, int leftstrip, int rightstrip,
                             int leftmost_col, int tiles_played, uint8_t *strip,
                             DictionaryWordList *possible_word_list) {
  if (current_col <= anchor_col) {
    if (board_row->letters[current_col] != ALPHABET_EMPTY_SQUARE_MARKER) {
      strip[current_col] = board_row->letters[current_col];
    } else {
      strip[current_col] = current_letter;
    }
    leftstrip = current_col;

    if (accepts && tiles_played > 0) {
      add_playthrough_word(possible_word_list, strip, leftstrip, rightstrip);
    }
    if (new_node_index == 0) {
      return;
    }

    if (current_col > leftmost_col) {
      playthrough_words_recursive_gen(board_row, kwg, rack, current_col - 1,
                                      anchor_col, new_node_index, leftstrip,
                                      rightstrip, leftmost_col, tiles_played,
                                      strip, possible_word_list);
    }

    const bool no_letter_directly_left =
        current_col == 0 ||
        board_row->letters[current_col - 1] == ALPHABET_EMPTY_SQUARE_MARKER;

    const uint32_t separation_node_index =
        kwg_get_next_node_index(kwg, new_node_index, SEPARATION_MACHINE_LETTER);
    if (separation_node_index != 0 && no_letter_directly_left &&
        anchor_col < BOARD_DIM - 1) {
      playthrough_words_recursive_gen(board_row, kwg, rack, anchor_col + 1,
                                      anchor_col, separation_node_index,
                                      leftstrip, rightstrip, leftmost_col,
                                      tiles_played, strip, possible_word_list);
    }
  } else {
    if (board_row->letters[current_col] != ALPHABET_EMPTY_SQUARE_MARKER) {
      strip[current_col] = board_row->letters[current_col];
    } else {
      strip[current_col] = current_letter;
    }
    rightstrip = current_col;

    bool no_letter_directly_right =
        current_col == BOARD_DIM - 1 ||
        board_row->letters[current_col + 1] == ALPHABET_EMPTY_SQUARE_MARKER;

    if (accepts && no_letter_directly_right && tiles_played > 0) {
      add_playthrough_word(possible_word_list, strip, leftstrip, rightstrip);
    }

    if (new_node_index != 0 && current_col < BOARD_DIM - 1) {
      playthrough_words_recursive_gen(board_row, kwg, rack, current_col + 1,
                                      anchor_col, new_node_index, leftstrip,
                                      rightstrip, leftmost_col, tiles_played,
                                      strip, possible_word_list);
    }
  }
}

void add_playthrough_words_from_row(const BoardRow *board_row, const KWG *kwg,
                                    Rack *bag_as_rack,
                                    DictionaryWordList *possible_word_list) {
  uint8_t strip[BOARD_DIM];
  const int gaddag_root = kwg_get_root_node_index(kwg);
  int leftmost_col = 0;
  for (int col = 0; col < BOARD_DIM; col++) {
    uint8_t current_letter = board_row->letters[col];
    if (current_letter != ALPHABET_EMPTY_SQUARE_MARKER) {
      while (((col < BOARD_DIM - 1) &&
              (board_row->letters[col + 1] != ALPHABET_EMPTY_SQUARE_MARKER))) {
        col++;
      }
      if (col == BOARD_DIM) {
        col--;
      }
      current_letter = board_row->letters[col];
      const uint8_t ml = current_letter; // already unblanked
      uint32_t next_node_index = 0;
      bool accepts = false;
      for (int i = gaddag_root;; i++) {
        const uint32_t node = kwg_node(kwg, i);
        if (kwg_node_tile(node) == ml) {
          next_node_index = kwg_node_arc_index_prefetch(node, kwg);
          break;
        }
        if (kwg_node_is_end(node)) {
          break;
        }
      }
      int tiles_played = 0;
      playthrough_words_go_on(board_row, kwg, bag_as_rack, col, col,
                              current_letter, next_node_index, accepts, col,
                              col, leftmost_col, tiles_played, strip,
                              possible_word_list);
      // leave an empty-space gap
      leftmost_col = col + 2;
    }
  }
}

void generate_possible_words(const Game *game, const KWG *override_kwg,
                             DictionaryWordList *possible_word_list) {
  const KWG *kwg = override_kwg;
  if (kwg == NULL) {
    const Player *player =
        game_get_player(game, game_get_player_on_turn_index(game));
    kwg = player_get_kwg(player);
  }
  // Accumulates words, then gets sorted. After pushing unique words to
  // possible_word_list, this is destroyed.
  DictionaryWordList *temp_list = dictionary_word_list_create();

  const int ld_size = ld_get_size(game_get_ld(game));
  Rack *unplayed_as_rack = rack_create(ld_size);
  Bag *bag = game_get_bag(game);
  for (int i = 0; i < ld_size; i++) {
    for (int j = 0; j < bag_get_letter(bag, i); j++) {
      rack_add_letter(unplayed_as_rack, i);
    }
    // Add tiles from players' racks
    for (int player_index = 0; player_index < 2; player_index++) {
      const Player *player = game_get_player(game, player_index);
      const Rack *rack = player_get_rack(player);
      for (int j = 0; j < rack_get_letter(rack, i); j++) {
        rack_add_letter(unplayed_as_rack, i);
      }
    }
  }

  // actually direction-agnostic: both rows and columns together
  BoardRows *board_rows = board_rows_create(game);

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
                                unplayed_as_rack, max_nonplaythrough_spaces,
                                word, 0, false, temp_list);
  for (int i = 0; i < board_rows->num_rows; i++) {
    add_playthrough_words_from_row(&board_rows->rows[i], kwg, unplayed_as_rack,
                                   temp_list);
  }

  rack_destroy(unplayed_as_rack);
  board_rows_destroy(board_rows);

  dictionary_word_list_sort(temp_list);
  dictionary_word_list_unique(temp_list, possible_word_list);
  dictionary_word_list_destroy(temp_list);
}