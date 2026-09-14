#include "word_prune.h"

#include "../def/board_defs.h"
#include "../def/cross_set_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../ent/bag.h"
#include "../ent/board.h"
#include "../ent/dictionary_word.h"
#include "../ent/game.h"
#include "../ent/kwg.h"
#include "../ent/letter_distribution.h"
#include "../ent/player.h"
#include "../ent/rack.h"
#include "../util/io_util.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int compare_board_rows(const void *a, const void *b) {
  const BoardRow *row_a = (const BoardRow *)a;
  const BoardRow *row_b = (const BoardRow *)b;
  for (int i = 0; i < BOARD_DIM; i++) {
    if (row_a->letters[i] < row_b->letters[i]) {
      return -1;
    }
    if (row_a->letters[i] > row_b->letters[i]) {
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
      MachineLetter letter = board_get_letter(game_get_board(game), row, col);
      MachineLetter unblanked = get_unblanked_machine_letter(letter);
      rows[row].letters[col] = unblanked;
    }
  }
  for (int col = 0; col < BOARD_DIM; col++) {
    for (int row = 0; row < BOARD_DIM; row++) {
      MachineLetter letter = board_get_letter(game_get_board(game), row, col);
      MachineLetter unblanked = get_unblanked_machine_letter(letter);
      rows[BOARD_DIM + col].letters[row] = unblanked;
    }
  }
  container->num_rows = BOARD_DIM * 2;
  qsort(rows, (size_t)(BOARD_DIM * 2), sizeof(BoardRow), compare_board_rows);
  container->num_rows = unique_rows(container);
  return container;
}

void board_rows_destroy(BoardRows *board_rows) { free(board_rows); }

// max consecutive empty spaces not touching a tile
int max_nonplaythrough_spaces_in_row(const BoardRow *board_row) {
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
                          const MachineLetter *strip, int leftstrip,
                          int rightstrip) {
  const int word_length = rightstrip - leftstrip + 1;
  dictionary_word_list_add_word(possible_word_list, strip + leftstrip,
                                word_length);
}

void add_words_without_playthrough(const KWG *kwg, uint32_t node_index,
                                   Rack *rack, int max_nonplaythrough,
                                   MachineLetter *word, int tiles_played,
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
    const MachineLetter ml = kwg_node_tile(node);
    const uint32_t new_node_index = kwg_node_arc_index_prefetch(node, kwg);
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
                             MachineLetter current_letter,
                             uint32_t new_node_index, bool accepts,
                             int leftstrip, int rightstrip, int leftmost_col,
                             int tiles_played, MachineLetter *strip,
                             DictionaryWordList *possible_word_list);

void playthrough_words_recursive_gen(const BoardRow *board_row, const KWG *kwg,
                                     Rack *rack, int col, int anchor_col,
                                     uint32_t node_index, int leftstrip,
                                     int rightstrip, int leftmost_col,
                                     int tiles_played, MachineLetter *strip,
                                     DictionaryWordList *possible_word_list) {
  const MachineLetter current_letter = board_row->letters[col];
  if (current_letter != ALPHABET_EMPTY_SQUARE_MARKER) {
    const MachineLetter ml = current_letter; // already unblanked
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
      const MachineLetter ml = kwg_node_tile(node);
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
                             MachineLetter current_letter,
                             uint32_t new_node_index, bool accepts,
                             int leftstrip, int rightstrip, int leftmost_col,
                             int tiles_played, MachineLetter *strip,
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
  MachineLetter strip[BOARD_DIM];
  const uint32_t gaddag_root = kwg_get_root_node_index(kwg);
  int leftmost_col = 0;
  for (int col = 0; col < BOARD_DIM; col++) {
    MachineLetter current_letter = board_row->letters[col];
    if (current_letter != ALPHABET_EMPTY_SQUARE_MARKER) {
      while (((col < BOARD_DIM - 1) &&
              (board_row->letters[col + 1] != ALPHABET_EMPTY_SQUARE_MARKER))) {
        col++;
      }
      if (col == BOARD_DIM) {
        col--;
      }
      current_letter = board_row->letters[col];
      const MachineLetter ml = current_letter; // already unblanked
      uint32_t next_node_index = 0;
      bool accepts = false;
      for (uint32_t i = gaddag_root;; i++) {
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

// Enumerate words playable in empty space (non-playthrough) using only the
// GADDAG, so wordprune runs on any KWG including gaddag-only pruned ones (no
// DAWG required). For each first letter, follow its arc then the separator to
// reach the forward (DAWG-equivalent) node, then reuse the existing forward
// walk for the suffix. gaddag_root -> first_letter -> SEPARATOR encodes the
// same forward suffix language as the DAWG node after that first letter.
void add_words_without_playthrough_gaddag(
    const KWG *kwg, Rack *rack, int max_nonplaythrough, MachineLetter *word,
    DictionaryWordList *possible_word_list) {
  if (max_nonplaythrough <= 0) {
    return;
  }
  const uint32_t gaddag_root = kwg_get_root_node_index(kwg);
  for (uint32_t node_idx = gaddag_root;; node_idx++) {
    const uint32_t node = kwg_node(kwg, node_idx);
    const MachineLetter ml = kwg_node_tile(node);
    const bool have_natural = rack_get_letter(rack, ml) > 0;
    const bool have_blank = rack_get_letter(rack, BLANK_MACHINE_LETTER) > 0;
    if (ml != SEPARATION_MACHINE_LETTER && (have_natural || have_blank)) {
      // Cross the separator from the single-letter reversed prefix [ml] to the
      // forward node (and its acceptance: whether ml alone is a word).
      uint32_t fwd_node = 0;
      bool fwd_accepts = false;
      for (uint32_t sep_idx = kwg_node_arc_index_prefetch(node, kwg);;
           sep_idx++) {
        const uint32_t sep = kwg_node(kwg, sep_idx);
        if (kwg_node_tile(sep) == SEPARATION_MACHINE_LETTER) {
          fwd_node = kwg_node_arc_index_prefetch(sep, kwg);
          fwd_accepts = kwg_node_accepts(sep);
          break;
        }
        if (kwg_node_is_end(sep)) {
          break;
        }
      }
      if (fwd_node != 0 || fwd_accepts) {
        word[0] = ml; // blanks are recorded as their natural letter
        const MachineLetter consumed = have_natural ? ml : BLANK_MACHINE_LETTER;
        rack_take_letter(rack, consumed);
        add_words_without_playthrough(kwg, fwd_node, rack, max_nonplaythrough,
                                      word, 1, fwd_accepts, possible_word_list);
        rack_add_letter(rack, consumed);
      }
    }
    if (kwg_node_is_end(node)) {
      break;
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
  Rack unplayed_as_rack;
  rack_set_dist_size_and_reset(&unplayed_as_rack, ld_size);
  const Bag *bag = game_get_bag(game);
  for (int i = 0; i < ld_size; i++) {
    for (int j = 0; j < bag_get_letter(bag, i); j++) {
      rack_add_letter(&unplayed_as_rack, i);
    }
    // Add tiles from players' racks
    for (int player_index = 0; player_index < 2; player_index++) {
      const Player *player = game_get_player(game, player_index);
      const Rack *rack = player_get_rack(player);
      for (int j = 0; j < rack_get_letter(rack, i); j++) {
        rack_add_letter(&unplayed_as_rack, i);
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

  MachineLetter word[BOARD_DIM];
  // Gaddag-only enumeration (no DAWG dependency) so this works on any KWG.
  add_words_without_playthrough_gaddag(
      kwg, &unplayed_as_rack, max_nonplaythrough_spaces, word, temp_list);
  for (int i = 0; i < board_rows->num_rows; i++) {
    add_playthrough_words_from_row(&board_rows->rows[i], kwg, &unplayed_as_rack,
                                   temp_list);
  }

  board_rows_destroy(board_rows);

  dictionary_word_list_sort(temp_list);
  dictionary_word_list_unique(temp_list, possible_word_list);
  dictionary_word_list_destroy(temp_list);
}
// ---------------------------------------------------------------------------
// Cross-check-aware pruning. See word_prune.h.
// ---------------------------------------------------------------------------

enum { WORD_PRUNE_NUM_LANES = BOARD_DIM * 2 };

// Letters that some accepted playthrough placement put on each empty square,
// per lane. Lanes 0..BOARD_DIM-1 are rows indexed by column; the rest are
// columns indexed by row.
typedef struct LaneLetterSets {
  uint32_t letters[WORD_PRUNE_NUM_LANES][BOARD_DIM];
} LaneLetterSets;

typedef struct CrossCheckContext {
  const BoardRows *lanes;
  int lane;
  // Letter sets recorded by the previous pass, or NULL on the first pass.
  const LaneLetterSets *previous;
  LaneLetterSets *current;
  DictionaryWordList *word_list;
} CrossCheckContext;

static inline int perpendicular_lane(int lane, int pos) {
  return lane < BOARD_DIM ? BOARD_DIM + pos : pos;
}

static inline int perpendicular_pos(int lane) {
  return lane < BOARD_DIM ? lane : lane - BOARD_DIM;
}

// True when the square at (lane, pos) already has a tile directly before or
// after it in the perpendicular lane.
static inline bool has_fixed_perpendicular_neighbor(const BoardRows *lanes,
                                                    int lane, int pos) {
  const BoardRow *perp = &lanes->rows[perpendicular_lane(lane, pos)];
  const int perp_pos = perpendicular_pos(lane);
  return (perp_pos > 0 &&
          perp->letters[perp_pos - 1] != ALPHABET_EMPTY_SQUARE_MARKER) ||
         (perp_pos < BOARD_DIM - 1 &&
          perp->letters[perp_pos + 1] != ALPHABET_EMPTY_SQUARE_MARKER);
}

static inline bool cross_check_allows(const CrossCheckContext *ctx, int pos,
                                      MachineLetter ml) {
  if (ctx->previous == NULL ||
      !has_fixed_perpendicular_neighbor(ctx->lanes, ctx->lane, pos)) {
    return true;
  }
  const uint32_t letters =
      ctx->previous->letters[perpendicular_lane(ctx->lane, pos)]
                            [perpendicular_pos(ctx->lane)];
  return (letters >> ml) & 1;
}

static BoardRows *all_lanes_create(const Game *game) {
  BoardRows *container = malloc_or_die(sizeof(BoardRows));
  const Board *board = game_get_board(game);
  for (int row = 0; row < BOARD_DIM; row++) {
    for (int col = 0; col < BOARD_DIM; col++) {
      const MachineLetter unblanked =
          get_unblanked_machine_letter(board_get_letter(board, row, col));
      container->rows[row].letters[col] = unblanked;
      container->rows[BOARD_DIM + col].letters[row] = unblanked;
    }
  }
  container->num_rows = WORD_PRUNE_NUM_LANES;
  return container;
}

static void cross_check_record_word(CrossCheckContext *ctx,
                                    const MachineLetter *strip, int leftstrip,
                                    int rightstrip) {
  const BoardRow *row = &ctx->lanes->rows[ctx->lane];
  for (int pos = leftstrip; pos <= rightstrip; pos++) {
    if (row->letters[pos] == ALPHABET_EMPTY_SQUARE_MARKER) {
      ctx->current->letters[ctx->lane][pos] |= (uint32_t)1 << strip[pos];
    }
  }
  dictionary_word_list_add_word(ctx->word_list, strip + leftstrip,
                                rightstrip - leftstrip + 1);
}

static void cross_check_words_go_on(CrossCheckContext *ctx, const KWG *kwg,
                                    Rack *rack, int current_col, int anchor_col,
                                    MachineLetter current_letter,
                                    uint32_t new_node_index, bool accepts,
                                    int leftstrip, int rightstrip,
                                    int leftmost_col, int tiles_played,
                                    MachineLetter *strip);

// Mirrors playthrough_words_recursive_gen, with the cross-check on pool
// letters placed on empty squares.
static void cross_check_words_recursive_gen(CrossCheckContext *ctx,
                                            const KWG *kwg, Rack *rack, int col,
                                            int anchor_col, uint32_t node_index,
                                            int leftstrip, int rightstrip,
                                            int leftmost_col, int tiles_played,
                                            MachineLetter *strip) {
  const BoardRow *board_row = &ctx->lanes->rows[ctx->lane];
  const MachineLetter current_letter = board_row->letters[col];
  if (current_letter != ALPHABET_EMPTY_SQUARE_MARKER) {
    uint32_t next_node_index = 0;
    bool accepts = false;
    for (uint32_t node_idx = node_index;; node_idx++) {
      const uint32_t node = kwg_node(kwg, node_idx);
      if (kwg_node_tile(node) == current_letter) {
        next_node_index = kwg_node_arc_index_prefetch(node, kwg);
        accepts = kwg_node_accepts(node);
        break;
      }
      if (kwg_node_is_end(node)) {
        break;
      }
    }
    cross_check_words_go_on(ctx, kwg, rack, col, anchor_col, current_letter,
                            next_node_index, accepts, leftstrip, rightstrip,
                            leftmost_col, tiles_played, strip);
  } else if (!rack_is_empty(rack)) {
    for (uint32_t node_idx = node_index;; node_idx++) {
      const uint32_t node = kwg_node(kwg, node_idx);
      const MachineLetter ml = kwg_node_tile(node);
      if (ml != SEPARATION_MACHINE_LETTER && cross_check_allows(ctx, col, ml)) {
        const uint32_t next_node_index = kwg_node_arc_index_prefetch(node, kwg);
        const bool accepts = kwg_node_accepts(node);
        if (rack_get_letter(rack, ml) > 0) {
          rack_take_letter(rack, ml);
          cross_check_words_go_on(
              ctx, kwg, rack, col, anchor_col, ml, next_node_index, accepts,
              leftstrip, rightstrip, leftmost_col, tiles_played + 1, strip);
          rack_add_letter(rack, ml);
        } else if (rack_get_letter(rack, BLANK_MACHINE_LETTER) > 0) {
          rack_take_letter(rack, BLANK_MACHINE_LETTER);
          cross_check_words_go_on(
              ctx, kwg, rack, col, anchor_col, ml, next_node_index, accepts,
              leftstrip, rightstrip, leftmost_col, tiles_played + 1, strip);
          rack_add_letter(rack, BLANK_MACHINE_LETTER);
        }
      }
      if (kwg_node_is_end(node)) {
        break;
      }
    }
  }
}

// Mirrors playthrough_words_go_on, recording accepted placements.
static void cross_check_words_go_on(CrossCheckContext *ctx, const KWG *kwg,
                                    Rack *rack, int current_col, int anchor_col,
                                    MachineLetter current_letter,
                                    uint32_t new_node_index, bool accepts,
                                    int leftstrip, int rightstrip,
                                    int leftmost_col, int tiles_played,
                                    MachineLetter *strip) {
  const BoardRow *board_row = &ctx->lanes->rows[ctx->lane];
  if (current_col <= anchor_col) {
    strip[current_col] =
        board_row->letters[current_col] != ALPHABET_EMPTY_SQUARE_MARKER
            ? board_row->letters[current_col]
            : current_letter;
    leftstrip = current_col;
    if (accepts && tiles_played > 0) {
      cross_check_record_word(ctx, strip, leftstrip, rightstrip);
    }
    if (new_node_index == 0) {
      return;
    }
    if (current_col > leftmost_col) {
      cross_check_words_recursive_gen(
          ctx, kwg, rack, current_col - 1, anchor_col, new_node_index,
          leftstrip, rightstrip, leftmost_col, tiles_played, strip);
    }
    const bool no_letter_directly_left =
        current_col == 0 ||
        board_row->letters[current_col - 1] == ALPHABET_EMPTY_SQUARE_MARKER;
    const uint32_t separation_node_index =
        kwg_get_next_node_index(kwg, new_node_index, SEPARATION_MACHINE_LETTER);
    if (separation_node_index != 0 && no_letter_directly_left &&
        anchor_col < BOARD_DIM - 1) {
      cross_check_words_recursive_gen(
          ctx, kwg, rack, anchor_col + 1, anchor_col, separation_node_index,
          leftstrip, rightstrip, leftmost_col, tiles_played, strip);
    }
  } else {
    strip[current_col] =
        board_row->letters[current_col] != ALPHABET_EMPTY_SQUARE_MARKER
            ? board_row->letters[current_col]
            : current_letter;
    rightstrip = current_col;
    const bool no_letter_directly_right =
        current_col == BOARD_DIM - 1 ||
        board_row->letters[current_col + 1] == ALPHABET_EMPTY_SQUARE_MARKER;
    if (accepts && no_letter_directly_right && tiles_played > 0) {
      cross_check_record_word(ctx, strip, leftstrip, rightstrip);
    }
    if (new_node_index != 0 && current_col < BOARD_DIM - 1) {
      cross_check_words_recursive_gen(
          ctx, kwg, rack, current_col + 1, anchor_col, new_node_index,
          leftstrip, rightstrip, leftmost_col, tiles_played, strip);
    }
  }
}

// Mirrors add_playthrough_words_from_row for one lane of the context.
static void cross_check_add_playthrough_words_from_lane(CrossCheckContext *ctx,
                                                        const KWG *kwg,
                                                        Rack *pool) {
  const BoardRow *board_row = &ctx->lanes->rows[ctx->lane];
  MachineLetter strip[BOARD_DIM];
  const uint32_t gaddag_root = kwg_get_root_node_index(kwg);
  int leftmost_col = 0;
  for (int col = 0; col < BOARD_DIM; col++) {
    if (board_row->letters[col] == ALPHABET_EMPTY_SQUARE_MARKER) {
      continue;
    }
    while (col < BOARD_DIM - 1 &&
           board_row->letters[col + 1] != ALPHABET_EMPTY_SQUARE_MARKER) {
      col++;
    }
    const MachineLetter current_letter = board_row->letters[col];
    uint32_t next_node_index = 0;
    for (uint32_t node_idx = gaddag_root;; node_idx++) {
      const uint32_t node = kwg_node(kwg, node_idx);
      if (kwg_node_tile(node) == current_letter) {
        next_node_index = kwg_node_arc_index_prefetch(node, kwg);
        break;
      }
      if (kwg_node_is_end(node)) {
        break;
      }
    }
    cross_check_words_go_on(ctx, kwg, pool, col, col, current_letter,
                            next_node_index, false, col, col, leftmost_col, 0,
                            strip);
    // leave an empty-space gap
    leftmost_col = col + 2;
  }
}

void generate_possible_words_with_cross_checks(
    const Game *game, const KWG *override_kwg,
    DictionaryWordList *possible_word_list) {
  const KWG *kwg = override_kwg;
  if (kwg == NULL) {
    kwg = player_get_kwg(
        game_get_player(game, game_get_player_on_turn_index(game)));
  }
  const int ld_size = ld_get_size(game_get_ld(game));
  Rack pool;
  rack_set_dist_size_and_reset(&pool, ld_size);
  const Bag *bag = game_get_bag(game);
  for (int ml = 0; ml < ld_size; ml++) {
    for (int count = 0; count < bag_get_letter(bag, ml); count++) {
      rack_add_letter(&pool, ml);
    }
    for (int player_index = 0; player_index < 2; player_index++) {
      const Rack *rack = player_get_rack(game_get_player(game, player_index));
      for (int count = 0; count < rack_get_letter(rack, ml); count++) {
        rack_add_letter(&pool, ml);
      }
    }
  }
  BoardRows *lanes = all_lanes_create(game);
  int max_nonplaythrough_spaces = 0;
  for (int lane = 0; lane < lanes->num_rows; lane++) {
    const int spaces = max_nonplaythrough_spaces_in_row(&lanes->rows[lane]);
    if (spaces > max_nonplaythrough_spaces) {
      max_nonplaythrough_spaces = spaces;
    }
  }

  DictionaryWordList *temp_list = dictionary_word_list_create();
  MachineLetter word[BOARD_DIM];
  add_words_without_playthrough_gaddag(kwg, &pool, max_nonplaythrough_spaces,
                                       word, temp_list);

  // Pass one: unconstrained, recording which letters each placement puts on
  // each empty square. Nearly all of the reduction comes from applying those
  // sets once; iterating to a fixed point removes about two percent more at
  // more than double the cost, so exactly two passes are made.
  LaneLetterSets *first_pass = malloc_or_die(sizeof(LaneLetterSets));
  memset(first_pass, 0, sizeof(LaneLetterSets));
  DictionaryWordList *discarded = dictionary_word_list_create();
  for (int lane = 0; lane < lanes->num_rows; lane++) {
    CrossCheckContext ctx = {.lanes = lanes,
                             .lane = lane,
                             .previous = NULL,
                             .current = first_pass,
                             .word_list = discarded};
    cross_check_add_playthrough_words_from_lane(&ctx, kwg, &pool);
  }
  dictionary_word_list_destroy(discarded);

  // Pass two: constrained by the first pass's letter sets.
  LaneLetterSets *second_pass = malloc_or_die(sizeof(LaneLetterSets));
  memset(second_pass, 0, sizeof(LaneLetterSets));
  for (int lane = 0; lane < lanes->num_rows; lane++) {
    CrossCheckContext ctx = {.lanes = lanes,
                             .lane = lane,
                             .previous = first_pass,
                             .current = second_pass,
                             .word_list = temp_list};
    cross_check_add_playthrough_words_from_lane(&ctx, kwg, &pool);
  }
  free(second_pass);
  free(first_pass);
  board_rows_destroy(lanes);

  dictionary_word_list_sort(temp_list);
  dictionary_word_list_unique(temp_list, possible_word_list);
  dictionary_word_list_destroy(temp_list);
}
