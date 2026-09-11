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
// PROTOTYPE: cross-check-aware refinement.
// ---------------------------------------------------------------------------

enum { WP_LANES = BOARD_DIM * 2 };

typedef struct LaneMasks {
  uint32_t allowed[WP_LANES][BOARD_DIM];
} LaneMasks;

// Lanes 0..BOARD_DIM-1 are rows (position = column); lanes BOARD_DIM.. are
// columns (position = row). The square at (lane, pos) is crossed by the
// perpendicular lane perp_lane at position perp_pos.
static inline int wp_perp_lane(int lane, int pos) {
  return lane < BOARD_DIM ? BOARD_DIM + pos : pos;
}
static inline int wp_perp_pos(int lane) {
  return lane < BOARD_DIM ? lane : lane - BOARD_DIM;
}

static BoardRows *wp_lanes_create(const Game *game) {
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
  container->num_rows = WP_LANES;
  return container;
}

// True when the square at (lane, pos) already has a tile directly before or
// after it in the perpendicular lane, so every final board's perpendicular
// word through it contains that tile.
static inline bool wp_has_fixed_perp_neighbor(const BoardRows *lanes, int lane,
                                              int pos) {
  const BoardRow *perp = &lanes->rows[wp_perp_lane(lane, pos)];
  const int p = wp_perp_pos(lane);
  return (p > 0 && perp->letters[p - 1] != ALPHABET_EMPTY_SQUARE_MARKER) ||
         (p < BOARD_DIM - 1 &&
          perp->letters[p + 1] != ALPHABET_EMPTY_SQUARE_MARKER);
}

static inline bool wp_letter_allowed(const BoardRows *lanes,
                                     const LaneMasks *prev, int lane, int pos,
                                     MachineLetter ml) {
  if (prev == NULL || !wp_has_fixed_perp_neighbor(lanes, lane, pos)) {
    return true;
  }
  return (prev->allowed[wp_perp_lane(lane, pos)][wp_perp_pos(lane)] >> ml) & 1;
}

// Single-rack realizability (PROTOTYPE). A placement's pool letters must be
// laid down by a sequence of moves, each drawing from one player's original
// rack (racks only shrink once the bag is empty; blanks are wildcards), each
// filling its own span, and each leaving the run it touches a dictionary word
// when that run has two or more tiles. Runs of one tile are allowed (they may
// be justified perpendicularly). Player alternation, connectivity and
// cross-group depletion are deliberately ignored, so this is a necessary
// condition and the filtered set stays a superset of the reachable words.
enum { WP_RACKDP_MAX_POOL = 10 };

typedef struct WpRacks {
  int counts[2][MACHINE_LETTER_MAX_VALUE + 1];
  int blanks[2];
  bool enabled;
  const KWG *kwg;
  uint32_t dawg_root;
} WpRacks;

typedef struct WpCtx {
  const BoardRows *lanes;
  int lane;
  const LaneMasks *prev;
  LaneMasks *cur;
  DictionaryWordList *list;
  const WpRacks *racks;
  long dp_rejected;
} WpCtx;

static bool wp_run_is_word(const WpRacks *racks, const MachineLetter *letters,
                           int len) {
  uint32_t node_index = racks->dawg_root;
  for (int i = 0; i < len; i++) {
    uint32_t next = 0;
    bool accepts = false;
    for (uint32_t j = node_index;; j++) {
      const uint32_t node = kwg_node(racks->kwg, j);
      if (kwg_node_tile(node) == letters[i]) {
        next = kwg_node_arc_index_prefetch(node, racks->kwg);
        accepts = kwg_node_accepts(node);
        break;
      }
      if (kwg_node_is_end(node)) {
        return false;
      }
    }
    if (i == len - 1) {
      return accepts;
    }
    if (next == 0) {
      return false;
    }
    node_index = next;
  }
  return false;
}

static bool wp_group_fits_rack(const WpRacks *racks, int player,
                               const MachineLetter *letters, int mask, int k) {
  int need[MACHINE_LETTER_MAX_VALUE + 1] = {0};
  int deficit = 0;
  for (int i = 0; i < k; i++) {
    if (mask & (1 << i)) {
      need[letters[i]]++;
    }
  }
  for (int ml = 0; ml <= MACHINE_LETTER_MAX_VALUE; ml++) {
    if (need[ml] > racks->counts[player][ml]) {
      deficit += need[ml] - racks->counts[player][ml];
    }
  }
  return deficit <= racks->blanks[player];
}

// strip holds the placement's letters for [left, right]; fixed[pos] says the
// square already holds a tile. Returns true when some single-rack move
// sequence can lay down every pool letter.
static bool wp_placement_realizable(const WpRacks *racks,
                                    const MachineLetter *strip, int left,
                                    int right, const bool *fixed) {
  int pool_pos[WP_RACKDP_MAX_POOL];
  MachineLetter pool_letters[WP_RACKDP_MAX_POOL];
  int k = 0;
  for (int pos = left; pos <= right; pos++) {
    if (!fixed[pos]) {
      if (k == WP_RACKDP_MAX_POOL) {
        return true; // too many pool tiles to check cheaply: keep it
      }
      pool_pos[k] = pos;
      pool_letters[k] = strip[pos];
      k++;
    }
  }
  if (k == 0) {
    return true;
  }
  const int full = (1 << k) - 1;
  bool fits[2][1 << WP_RACKDP_MAX_POOL];
  for (int mask = 1; mask <= full; mask++) {
    fits[0][mask] = wp_group_fits_rack(racks, 0, pool_letters, mask, k);
    fits[1][mask] = wp_group_fits_rack(racks, 1, pool_letters, mask, k);
  }
  // run validity memo keyed by [start][end] within [left,right]
  signed char run_valid[BOARD_DIM][BOARD_DIM];
  memset(run_valid, -1, sizeof(run_valid));
  bool reachable[1 << WP_RACKDP_MAX_POOL];
  memset(reachable, 0, sizeof(bool) * (size_t)(1 << k));
  reachable[0] = true;
  for (int set = 0; set < full; set++) {
    if (!reachable[set]) {
      continue;
    }
    const int rest = full & ~set;
    for (int group = rest; group > 0; group = (group - 1) & rest) {
      if (!fits[0][group] && !fits[1][group]) {
        continue;
      }
      const int after = set | group;
      if (reachable[after]) {
        continue;
      }
      // occupied after the move: fixed squares plus pool squares in `after`
      bool occ[BOARD_DIM];
      for (int pos = left; pos <= right; pos++) {
        occ[pos] = fixed[pos];
      }
      int gmin = BOARD_DIM;
      int gmax = -1;
      for (int i = 0; i < k; i++) {
        if (after & (1 << i)) {
          occ[pool_pos[i]] = true;
        }
        if (group & (1 << i)) {
          if (pool_pos[i] < gmin) {
            gmin = pool_pos[i];
          }
          if (pool_pos[i] > gmax) {
            gmax = pool_pos[i];
          }
        }
      }
      bool span_full = true;
      for (int pos = gmin; pos <= gmax; pos++) {
        if (!occ[pos]) {
          span_full = false;
          break;
        }
      }
      if (!span_full) {
        continue;
      }
      int rs = gmin;
      int re = gmax;
      while (rs > left && occ[rs - 1]) {
        rs--;
      }
      while (re < right && occ[re + 1]) {
        re++;
      }
      bool ok = true;
      if (re > rs) {
        if (run_valid[rs][re] < 0) {
          run_valid[rs][re] = wp_run_is_word(racks, strip + rs, re - rs + 1);
        }
        ok = run_valid[rs][re] != 0;
      }
      if (ok) {
        reachable[after] = true;
      }
    }
  }
  return reachable[full];
}

static void wp_record_word(WpCtx *ctx, const MachineLetter *strip,
                           int leftstrip, int rightstrip) {
  const BoardRow *row = &ctx->lanes->rows[ctx->lane];
  if (ctx->racks != NULL && ctx->racks->enabled) {
    bool fixed[BOARD_DIM];
    for (int pos = 0; pos < BOARD_DIM; pos++) {
      fixed[pos] = row->letters[pos] != ALPHABET_EMPTY_SQUARE_MARKER;
    }
    if (!wp_placement_realizable(ctx->racks, strip, leftstrip, rightstrip,
                                 fixed)) {
      ctx->dp_rejected++;
      return;
    }
  }
  for (int pos = leftstrip; pos <= rightstrip; pos++) {
    if (row->letters[pos] == ALPHABET_EMPTY_SQUARE_MARKER) {
      ctx->cur->allowed[ctx->lane][pos] |= (uint32_t)1 << strip[pos];
    }
  }
  dictionary_word_list_add_word(ctx->list, strip + leftstrip,
                                rightstrip - leftstrip + 1);
}

static void wp_go_on(WpCtx *ctx, const KWG *kwg, Rack *rack, int current_col,
                     int anchor_col, MachineLetter current_letter,
                     uint32_t new_node_index, bool accepts, int leftstrip,
                     int rightstrip, int leftmost_col, int tiles_played,
                     MachineLetter *strip);

static void wp_recursive_gen(WpCtx *ctx, const KWG *kwg, Rack *rack, int col,
                             int anchor_col, uint32_t node_index,
                             int leftstrip, int rightstrip, int leftmost_col,
                             int tiles_played, MachineLetter *strip) {
  const BoardRow *board_row = &ctx->lanes->rows[ctx->lane];
  const MachineLetter current_letter = board_row->letters[col];
  if (current_letter != ALPHABET_EMPTY_SQUARE_MARKER) {
    uint32_t next_node_index = 0;
    bool accepts = false;
    for (uint32_t i = node_index;; i++) {
      const uint32_t node = kwg_node(kwg, i);
      if (kwg_node_tile(node) == current_letter) {
        next_node_index = kwg_node_arc_index_prefetch(node, kwg);
        accepts = kwg_node_accepts(node);
        break;
      }
      if (kwg_node_is_end(node)) {
        break;
      }
    }
    wp_go_on(ctx, kwg, rack, col, anchor_col, current_letter, next_node_index,
             accepts, leftstrip, rightstrip, leftmost_col, tiles_played, strip);
  } else if (!rack_is_empty(rack)) {
    for (uint32_t i = node_index;; i++) {
      const uint32_t node = kwg_node(kwg, i);
      const MachineLetter ml = kwg_node_tile(node);
      if (ml != SEPARATION_MACHINE_LETTER &&
          wp_letter_allowed(ctx->lanes, ctx->prev, ctx->lane, col, ml)) {
        const uint32_t next_node_index = kwg_node_arc_index_prefetch(node, kwg);
        const bool accepts = kwg_node_accepts(node);
        if (rack_get_letter(rack, ml) > 0) {
          rack_take_letter(rack, ml);
          wp_go_on(ctx, kwg, rack, col, anchor_col, ml, next_node_index,
                   accepts, leftstrip, rightstrip, leftmost_col,
                   tiles_played + 1, strip);
          rack_add_letter(rack, ml);
        } else if (rack_get_letter(rack, BLANK_MACHINE_LETTER) > 0) {
          rack_take_letter(rack, BLANK_MACHINE_LETTER);
          wp_go_on(ctx, kwg, rack, col, anchor_col, ml, next_node_index,
                   accepts, leftstrip, rightstrip, leftmost_col,
                   tiles_played + 1, strip);
          rack_add_letter(rack, BLANK_MACHINE_LETTER);
        }
      }
      if (kwg_node_is_end(node)) {
        break;
      }
    }
  }
}

static void wp_go_on(WpCtx *ctx, const KWG *kwg, Rack *rack, int current_col,
                     int anchor_col, MachineLetter current_letter,
                     uint32_t new_node_index, bool accepts, int leftstrip,
                     int rightstrip, int leftmost_col, int tiles_played,
                     MachineLetter *strip) {
  const BoardRow *board_row = &ctx->lanes->rows[ctx->lane];
  if (current_col <= anchor_col) {
    strip[current_col] =
        board_row->letters[current_col] != ALPHABET_EMPTY_SQUARE_MARKER
            ? board_row->letters[current_col]
            : current_letter;
    leftstrip = current_col;
    if (accepts && tiles_played > 0) {
      wp_record_word(ctx, strip, leftstrip, rightstrip);
    }
    if (new_node_index == 0) {
      return;
    }
    if (current_col > leftmost_col) {
      wp_recursive_gen(ctx, kwg, rack, current_col - 1, anchor_col,
                       new_node_index, leftstrip, rightstrip, leftmost_col,
                       tiles_played, strip);
    }
    const bool no_letter_directly_left =
        current_col == 0 ||
        board_row->letters[current_col - 1] == ALPHABET_EMPTY_SQUARE_MARKER;
    const uint32_t separation_node_index =
        kwg_get_next_node_index(kwg, new_node_index, SEPARATION_MACHINE_LETTER);
    if (separation_node_index != 0 && no_letter_directly_left &&
        anchor_col < BOARD_DIM - 1) {
      wp_recursive_gen(ctx, kwg, rack, anchor_col + 1, anchor_col,
                       separation_node_index, leftstrip, rightstrip,
                       leftmost_col, tiles_played, strip);
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
      wp_record_word(ctx, strip, leftstrip, rightstrip);
    }
    if (new_node_index != 0 && current_col < BOARD_DIM - 1) {
      wp_recursive_gen(ctx, kwg, rack, current_col + 1, anchor_col,
                       new_node_index, leftstrip, rightstrip, leftmost_col,
                       tiles_played, strip);
    }
  }
}


// Non-playthrough words placed inside an empty run of a lane, constrained by
// the perpendicular masks of each square (PROTOTYPE). A non-playthrough
// placement occupies squares with no tile before or after it in its own lane,
// so its squares never feed the masks consumed by the perpendicular lanes; it
// only consumes them. Forward DAWG walk from each start square of each usable
// run, taking pool letters allowed at that square.
static void wp_npt_walk(WpCtx *ctx, const KWG *kwg, Rack *pool,
                        uint32_t node_index, int pos, int last_pos,
                        MachineLetter *word, int len) {
  if (node_index == 0 || pos > last_pos || rack_is_empty(pool)) {
    return;
  }
  for (uint32_t i = node_index;; i++) {
    const uint32_t node = kwg_node(kwg, i);
    const MachineLetter ml = kwg_node_tile(node);
    if (ml != SEPARATION_MACHINE_LETTER &&
        wp_letter_allowed(ctx->lanes, ctx->prev, ctx->lane, pos, ml)) {
      const bool have_natural = rack_get_letter(pool, ml) > 0;
      const bool have_blank = rack_get_letter(pool, BLANK_MACHINE_LETTER) > 0;
      if (have_natural || have_blank) {
        const MachineLetter consumed = have_natural ? ml : BLANK_MACHINE_LETTER;
        rack_take_letter(pool, consumed);
        word[len] = ml;
        if (kwg_node_accepts(node) && len + 1 >= 2) {
          dictionary_word_list_add_word(ctx->list, word, len + 1);
        }
        wp_npt_walk(ctx, kwg, pool, kwg_node_arc_index_prefetch(node, kwg),
                    pos + 1, last_pos, word, len + 1);
        rack_add_letter(pool, consumed);
      }
    }
    if (kwg_node_is_end(node)) {
      break;
    }
  }
}

static void wp_add_nonplaythrough_words_from_lane(WpCtx *ctx, const KWG *kwg,
                                                  Rack *pool) {
  const BoardRow *row = &ctx->lanes->rows[ctx->lane];
  const uint32_t dawg_root = kwg_get_dawg_root_node_index(kwg);
  if (dawg_root == 0) {
    return;
  }
  MachineLetter word[BOARD_DIM];
  int pos = 0;
  while (pos < BOARD_DIM) {
    if (row->letters[pos] != ALPHABET_EMPTY_SQUARE_MARKER) {
      pos++;
      continue;
    }
    int end = pos;
    while (end + 1 < BOARD_DIM &&
           row->letters[end + 1] == ALPHABET_EMPTY_SQUARE_MARKER) {
      end++;
    }
    // Usable squares: not adjacent to a tile in this lane.
    const int first = pos == 0 ? 0 : pos + 1;
    const int last = end == BOARD_DIM - 1 ? end : end - 1;
    for (int start = first; start + 1 <= last; start++) {
      wp_npt_walk(ctx, kwg, pool, dawg_root, start, last, word, 0);
    }
    pos = end + 1;
  }
}

static void wp_add_playthrough_words_from_lane(WpCtx *ctx, const KWG *kwg,
                                               Rack *pool) {
  const BoardRow *board_row = &ctx->lanes->rows[ctx->lane];
  MachineLetter strip[BOARD_DIM];
  const uint32_t gaddag_root = kwg_get_root_node_index(kwg);
  int leftmost_col = 0;
  for (int col = 0; col < BOARD_DIM; col++) {
    MachineLetter current_letter = board_row->letters[col];
    if (current_letter == ALPHABET_EMPTY_SQUARE_MARKER) {
      continue;
    }
    while (col < BOARD_DIM - 1 &&
           board_row->letters[col + 1] != ALPHABET_EMPTY_SQUARE_MARKER) {
      col++;
    }
    current_letter = board_row->letters[col];
    uint32_t next_node_index = 0;
    for (uint32_t i = gaddag_root;; i++) {
      const uint32_t node = kwg_node(kwg, i);
      if (kwg_node_tile(node) == current_letter) {
        next_node_index = kwg_node_arc_index_prefetch(node, kwg);
        break;
      }
      if (kwg_node_is_end(node)) {
        break;
      }
    }
    wp_go_on(ctx, kwg, pool, col, col, current_letter, next_node_index, false,
             col, col, leftmost_col, 0, strip);
    leftmost_col = col + 2;
  }
}

void generate_possible_words_refined(const Game *game, const KWG *override_kwg,
                                     DictionaryWordList *possible_word_list,
                                     int max_passes,
                                     WordPruneRefineStats *stats) {
  const KWG *kwg = override_kwg;
  if (kwg == NULL) {
    kwg = player_get_kwg(
        game_get_player(game, game_get_player_on_turn_index(game)));
  }
  const int ld_size = ld_get_size(game_get_ld(game));
  Rack pool;
  rack_set_dist_size_and_reset(&pool, ld_size);
  const Bag *bag = game_get_bag(game);
  for (int i = 0; i < ld_size; i++) {
    for (int j = 0; j < bag_get_letter(bag, i); j++) {
      rack_add_letter(&pool, i);
    }
    for (int player_index = 0; player_index < 2; player_index++) {
      const Rack *rack = player_get_rack(game_get_player(game, player_index));
      for (int j = 0; j < rack_get_letter(rack, i); j++) {
        rack_add_letter(&pool, i);
      }
    }
  }
  BoardRows *lanes = wp_lanes_create(game);
  WpRacks racks;
  memset(&racks, 0, sizeof(racks));
  racks.kwg = kwg;
  racks.dawg_root = kwg_get_dawg_root_node_index(kwg);
  // Only sound with an empty bag: unseen tiles could otherwise reach either
  // rack. WORDPRUNE_RACKDP enables the prototype filter.
  racks.enabled = getenv("WORDPRUNE_RACKDP") != NULL &&
                  bag_get_letters(bag) == 0 && racks.dawg_root != 0;
  for (int player_index = 0; player_index < 2; player_index++) {
    const Rack *rack = player_get_rack(game_get_player(game, player_index));
    for (int ml = 0; ml < ld_size && ml <= MACHINE_LETTER_MAX_VALUE; ml++) {
      racks.counts[player_index][ml] = rack_get_letter(rack, ml);
    }
    racks.blanks[player_index] = rack_get_letter(rack, BLANK_MACHINE_LETTER);
    racks.counts[player_index][BLANK_MACHINE_LETTER] = 0;
  }
  if (stats != NULL) {
    stats->dp_rejected = 0;
  }
  int max_nonplaythrough_spaces = 0;
  for (int i = 0; i < lanes->num_rows; i++) {
    const int spaces = max_nonplaythrough_spaces_in_row(&lanes->rows[i]);
    if (spaces > max_nonplaythrough_spaces) {
      max_nonplaythrough_spaces = spaces;
    }
  }
  MachineLetter word[BOARD_DIM];
  DictionaryWordList *nonplaythrough = dictionary_word_list_create();
  add_words_without_playthrough_gaddag(kwg, &pool, max_nonplaythrough_spaces,
                                       word, nonplaythrough);
  if (stats != NULL) {
    stats->nonplaythrough_words =
        dictionary_word_list_get_count(nonplaythrough);
    stats->passes = 0;
  }

  LaneMasks *prev = NULL; // pass 0: unconstrained
  LaneMasks *cur = malloc_or_die(sizeof(LaneMasks));
  LaneMasks *prev_storage = malloc_or_die(sizeof(LaneMasks));
  DictionaryWordList *result = NULL;
  int previous_count = -1;
  if (max_passes < 1) {
    max_passes = 1;
  }
  for (int pass = 0; pass < max_passes; pass++) {
    memset(cur, 0, sizeof(LaneMasks));
    DictionaryWordList *temp = dictionary_word_list_create();
    const bool npt_per_lane = getenv("WORDPRUNE_NPT") != NULL;
    if (!npt_per_lane) {
      for (int i = 0; i < dictionary_word_list_get_count(nonplaythrough);
           i++) {
        const DictionaryWord *w =
            dictionary_word_list_get_word(nonplaythrough, i);
        dictionary_word_list_add_word(temp, dictionary_word_get_word(w),
                                      dictionary_word_get_length(w));
      }
    }
    for (int lane = 0; lane < lanes->num_rows; lane++) {
      WpCtx ctx = {.lanes = lanes, .lane = lane, .prev = prev, .cur = cur,
                   .list = temp, .racks = &racks, .dp_rejected = 0};
      if (npt_per_lane) {
        wp_add_nonplaythrough_words_from_lane(&ctx, kwg, &pool);
      }
      wp_add_playthrough_words_from_lane(&ctx, kwg, &pool);
      if (stats != NULL) {
        stats->dp_rejected += ctx.dp_rejected;
      }
    }
    DictionaryWordList *unique = dictionary_word_list_create();
    dictionary_word_list_sort(temp);
    dictionary_word_list_unique(temp, unique);
    dictionary_word_list_destroy(temp);
    const int count = dictionary_word_list_get_count(unique);
    if (stats != NULL && pass < 8) {
      stats->words_after_pass[pass] = count;
      stats->passes = pass + 1;
    }
    if (result != NULL) {
      dictionary_word_list_destroy(result);
    }
    result = unique;
    if (count == previous_count) {
      break;
    }
    previous_count = count;
    // Next pass constrains by this pass's masks.
    memcpy(prev_storage, cur, sizeof(LaneMasks));
    prev = prev_storage;
  }
  for (int i = 0; i < dictionary_word_list_get_count(result); i++) {
    const DictionaryWord *w = dictionary_word_list_get_word(result, i);
    dictionary_word_list_add_word(possible_word_list, dictionary_word_get_word(w),
                                  dictionary_word_get_length(w));
  }
  dictionary_word_list_destroy(result);
  dictionary_word_list_destroy(nonplaythrough);
  free(cur);
  free(prev_storage);
  board_rows_destroy(lanes);
}
