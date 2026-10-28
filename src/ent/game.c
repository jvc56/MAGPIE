#include "game.h"

#include "../def/board_defs.h"
#include "../def/cross_set_defs.h"
#include "../def/game_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/players_data_defs.h"
#include "../ent/equity.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "bag.h"
#include "board.h"
#include "cross_set_cache.h"
#include "kwg.h"
#include "kwg_alpha.h"
#include "kwg_woadwage.h"
#include "letter_distribution.h"
#include "move_undo.h"
#include "player.h"
#include "players_data.h"
#include "rack.h"
#include "word_info_table.h"
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct MinimalGameBackup {
  Board *board;
  // Whether this backup recorded the board's changes in the game's rollout
  // board undo rather than copying the board.
  bool board_recorded;
  Bag *bag;
  Rack *p0rack;
  Rack *p1rack;
  Equity p0score;
  Equity p1score;
  int player_on_turn_index;
  int starting_player_index;
  int consecutive_scoreless_turns;
  game_end_reason_t game_end_reason;
} MinimalGameBackup;

// The Game struct does not own the
// letter distribution, which is the
// caller's responsibility. There are
// also some fields in Player which
// are owned by the caller. See the Player
// struct for more details.
struct Game {
  int player_on_turn_index;
  int starting_player_index;
  int consecutive_scoreless_turns;
  int max_scoreless_turns;
  Equity bingo_bonus;
  game_end_reason_t game_end_reason;
  bool data_is_shared[NUMBER_OF_DATA];
  Board *board;
  Bag *bag;
  // Some data in the Player objects
  // are owned by the caller. See Player
  // for details.
  Player *players[2];

  // Owned by the caller
  const LetterDistribution *ld;
  game_variant_t variant;
  // Optional override KWGs for cross-set generation (e.g., word-pruned KWGs
  // in the endgame solver). When non-NULL, cross-set computation uses these
  // instead of the player's full KWG. Not owned by Game.
  const KWG *override_kwgs[2];
  dual_lexicon_mode_t dual_lexicon_mode;
  // Backups. Each simulation backup is allocated the first time a backup
  // reaches its depth: sims back up only the candidate move, and many games
  // that enable them never back up at all.
  MinimalGameBackup *sim_game_backups[MAX_SEARCH_DEPTH];
  // Whether simulation backups were ever enabled, so that unplays pop them
  // rather than the GCG backup.
  bool sim_backups_enabled;
  int backup_cursor;
  MinimalGameBackup *gcg_game_backup;
  backup_mode_t backup_mode;
  // The record of the board squares changed since an open simulation backup,
  // when the game records them (see game_enable_rollout_board_undo), and
  // whether such a backup is open.
  MoveUndo *rollout_board_undo;
  bool rollout_board_undo_open;
  // Classic cross sets by the words beside the empty square, or NULL. Not
  // owned by the game; see game_set_cross_set_cache.
  CrossSetCache *cross_set_cache;
};

// The odd multiplier of game_cross_set_cache_key's steps, and the symbol it
// takes for the empty square, above every letter's (letter + 1).
static const uint64_t CROSS_SET_KEY_MULTIPLIER = 0x9E3779B97F4A7C15ULL;
static const uint64_t CROSS_SET_KEY_SEPARATOR = 0x101;

game_variant_t game_get_variant(const Game *game) { return game->variant; }

Equity game_get_bingo_bonus(const Game *game) { return game->bingo_bonus; }

game_variant_t get_game_variant_type_from_name(const char *variant_name) {
  game_variant_t game_variant = GAME_VARIANT_UNKNOWN;
  if (strings_iequal(variant_name, GAME_VARIANT_CLASSIC_NAME)) {
    game_variant = GAME_VARIANT_CLASSIC;
  } else if (strings_iequal(variant_name, GAME_VARIANT_WORDSMOG_NAME)) {
    game_variant = GAME_VARIANT_WORDSMOG;
  } else if (strings_iequal(variant_name, GAME_VARIANT_WOADWAGE_NAME)) {
    game_variant = GAME_VARIANT_WOADWAGE;
  }
#ifdef IF_ONLY
  else if (strings_iequal(variant_name, GAME_VARIANT_IF_ONLY_NAME)) {
    game_variant = GAME_VARIANT_IF_ONLY;
  } else if (strings_iequal(variant_name, GAME_VARIANT_LONIFY_NAME)) {
    game_variant = GAME_VARIANT_LONIFY;
  }
#endif
  return game_variant;
}

Board *game_get_board(const Game *game) { return game->board; }

Bag *game_get_bag(const Game *game) { return game->bag; }

const LetterDistribution *game_get_ld(const Game *game) { return game->ld; }

Player *game_get_player(const Game *game, int player_index) {
  return game->players[player_index];
}

int game_get_player_on_turn_index(const Game *game) {
  return game->player_on_turn_index;
}

game_end_reason_t game_get_game_end_reason(const Game *game) {
  return game->game_end_reason;
}

bool game_over(const Game *game) {
  return game->game_end_reason != GAME_END_REASON_NONE;
}

void game_set_game_end_reason(Game *game, game_end_reason_t game_end_reason) {
  game->game_end_reason = game_end_reason;
}

int game_get_starting_player_index(const Game *game) {
  return game->starting_player_index;
}

int game_get_player_draw_index(const Game *game, int player_index) {
  return player_index ^ game->starting_player_index;
}

int game_get_player_on_turn_draw_index(const Game *game) {
  return game_get_player_draw_index(game, game->player_on_turn_index);
}

backup_mode_t game_get_backup_mode(const Game *game) {
  return game->backup_mode;
}

int game_get_consecutive_scoreless_turns(const Game *game) {
  return game->consecutive_scoreless_turns;
}

bool game_get_data_is_shared(const Game *game,
                             players_data_t players_data_type) {
  return game->data_is_shared[(int)players_data_type];
}

void game_set_consecutive_scoreless_turns(Game *game, int value) {
  game->consecutive_scoreless_turns = value;
}

void game_set_player_on_turn_index(Game *game, int player_index) {
  game->player_on_turn_index = player_index;
}

void game_increment_consecutive_scoreless_turns(Game *game) {
  game->consecutive_scoreless_turns++;
}

void game_set_endgame_solving_mode(Game *game) {
  if (game->consecutive_scoreless_turns + 1 >= game->max_scoreless_turns) {
    game->consecutive_scoreless_turns = 1;
  } else {
    game->consecutive_scoreless_turns = 0;
  }
  game->max_scoreless_turns = 2;
}

void game_start_next_player_turn(Game *game) {
  game->player_on_turn_index = 1 - game->player_on_turn_index;
}

Equity traverse_backwards_for_score(const Board *board,
                                    const LetterDistribution *ld, int row,
                                    int col) {
  Equity score = 0;
  while (board_is_position_in_bounds_and_not_bricked(board, row, col)) {
    MachineLetter ml = board_get_letter(board, row, col);
    if (ml == ALPHABET_EMPTY_SQUARE_MARKER) {
      break;
    }
    if (get_is_blanked(ml)) {
      score += ld_get_score(ld, BLANK_MACHINE_LETTER);
    } else {
      score += ld_get_score(ld, ml);
    }
    col--;
  }
  return score;
}

static inline uint32_t traverse_backwards(const KWG *kwg, const Board *board,
                                          int row, int col, uint32_t node_index,
                                          bool check_letter_set,
                                          int left_most_col) {
  while (board_is_position_in_bounds_and_not_bricked(board, row, col)) {
    MachineLetter ml = board_get_letter(board, row, col);
    if (ml == ALPHABET_EMPTY_SQUARE_MARKER) {
      break;
    }

    if (node_index == 0) {
      return node_index;
    }

    if (check_letter_set && col == left_most_col) {
      if (kwg_in_letter_set(kwg, ml, node_index)) {
        return node_index;
      }
      return 0;
    }

    node_index = kwg_get_next_node_index(kwg, node_index,
                                         get_unblanked_machine_letter(ml));
    col--;
  }

  return node_index;
}

static inline void traverse_backwards_add_to_rack(const Board *board, int row,
                                                  int col, Rack *rack) {
  while (board_is_position_in_bounds_and_not_bricked(board, row, col)) {
    MachineLetter ml = board_get_letter(board, row, col);
    if (ml == ALPHABET_EMPTY_SQUARE_MARKER) {
      break;
    }
    rack_add_letter(rack, get_unblanked_machine_letter(ml));
    col--;
  }
}

void game_set_override_kwgs(Game *game, const KWG *kwg0, const KWG *kwg1,
                            dual_lexicon_mode_t mode) {
  game->override_kwgs[0] = kwg0;
  game->override_kwgs[1] = kwg1;
  game->dual_lexicon_mode = mode;
}

void game_clear_override_kwgs(Game *game) {
  game->override_kwgs[0] = NULL;
  game->override_kwgs[1] = NULL;
  game->dual_lexicon_mode = DUAL_LEXICON_MODE_IGNORANT;
}

const KWG *game_get_effective_kwg(const Game *game, int player_index) {
  if (game->override_kwgs[0] != NULL) {
    if (game->dual_lexicon_mode == DUAL_LEXICON_MODE_IGNORANT) {
      return game->override_kwgs[0];
    }
    return game->override_kwgs[player_index];
  }
  return player_get_kwg(game_get_player(game, player_index));
}

// Returns the KWG to use for cross-set generation at the given cross_set_index.
// If override KWGs are set, uses those (respecting dual-lexicon mode).
// Otherwise falls back to the player's standard KWG.
static inline const KWG *get_kwg_for_cross_set(const Game *game,
                                               int cross_set_index) {
  if (game->override_kwgs[0] != NULL) {
    if (game->dual_lexicon_mode == DUAL_LEXICON_MODE_IGNORANT) {
      return game->override_kwgs[0];
    }
    return game->override_kwgs[cross_set_index];
  }
  return player_get_kwg(game_get_player(game, cross_set_index));
}

// The word info table parallel to get_kwg_for_cross_set. Returns NULL when no
// table is loaded or when an override KWG is in use (endgame/word-prune runs a
// reduced word list the full-lexicon table does not match), in which case the
// board caches no WIT block rows and move generation falls back to no pruning.
static inline const WordInfoTable *
get_word_info_table_for_cross_set(const Game *game, int cross_set_index) {
  if (game->override_kwgs[0] != NULL) {
    return NULL;
  }
  return player_get_word_info_table(game_get_player(game, cross_set_index));
}

// Computes and caches the word info table value-row pointer for the block of
// tiles [block_left, block_right] on `row` (this orientation), keyed to the
// block's leftmost square in the word direction. Move generation reads it with
// no per-call lookup. See Board.wit_block_rows.
static inline void game_store_wit_block(const WordInfoTable *wit, Board *board,
                                        int row, int block_left,
                                        int block_right, int through_dir,
                                        int csi) {
  MachineLetter block[BOARD_DIM] = {0};
  int len = 0;
  for (int block_col = block_left; block_col <= block_right; block_col++) {
    block[len++] =
        get_unblanked_machine_letter(board_get_letter(board, row, block_col));
  }
  const uint32_t *block_row = word_info_table_lookup(wit, block, len);
  board_set_wit_block(board, row, block_left, through_dir, csi, block_row,
                      (uint8_t)len);
}

static inline void game_gen_alpha_cross_set(const Game *game, int row, int col,
                                            int dir, int cross_set_index) {
  if (!board_is_position_in_bounds(row, col)) {
    return;
  }

  Board *board = game_get_board(game);

  if (board_is_nonempty_or_bricked(board, row, col)) {
    board_set_cross_set(board, row, col, dir, cross_set_index, 0);
    board_set_cross_score(board, row, col, dir, cross_set_index, 0);
    return;
  }
  if (board_are_left_and_right_empty(board, row, col)) {
    board_set_cross_set(board, row, col, dir, cross_set_index,
                        TRIVIAL_CROSS_SET);
    board_set_cross_score(board, row, col, dir, cross_set_index, 0);
    return;
  }

  const LetterDistribution *ld = game_get_ld(game);

  const int left_col =
      board_get_word_edge(board, row, col - 1, WORD_DIRECTION_LEFT);
  const int right_col =
      board_get_word_edge(board, row, col + 1, WORD_DIRECTION_RIGHT);
  Equity score = 0;

  Rack cross_set_rack;
  rack_set_dist_size_and_reset(&cross_set_rack, ld_get_size(ld));
  if (left_col < col) {
    traverse_backwards_add_to_rack(board, row, col - 1, &cross_set_rack);
    score += traverse_backwards_for_score(board, ld, row, col - 1);
  }

  if (right_col > col) {
    traverse_backwards_add_to_rack(board, row, right_col, &cross_set_rack);
    score += traverse_backwards_for_score(board, ld, row, right_col);
  }

  const KWG *kwg = get_kwg_for_cross_set(game, cross_set_index);

  board_set_cross_set_with_blank(
      board, row, col, dir, cross_set_index,
      kwg_compute_alpha_cross_set(kwg, &cross_set_rack));
  board_set_cross_score(board, row, col, dir, cross_set_index, score);
}

// Woadwage's cross set at (row, col): the letters that make a woadwage word
// with the tiles beside the square. Woadwage move generation reads no
// extension sets, so the square's own are left trivial for the shadow, which
// does read them, in case classic ones were computed for this board.
static inline void game_gen_woadwage_cross_set(const Game *game, int row,
                                               int col, int dir,
                                               int cross_set_index) {
  if (!board_is_position_in_bounds(row, col)) {
    return;
  }

  Board *board = game_get_board(game);
  const int through_dir = board_toggle_dir(dir);
  board_set_left_extension_set(board, row, col, through_dir, cross_set_index,
                               TRIVIAL_CROSS_SET);
  board_set_right_extension_set(board, row, col, through_dir, cross_set_index,
                                TRIVIAL_CROSS_SET);

  if (board_is_nonempty_or_bricked(board, row, col)) {
    board_set_cross_set(board, row, col, dir, cross_set_index, 0);
    board_set_cross_score(board, row, col, dir, cross_set_index, 0);
    return;
  }
  if (board_are_left_and_right_empty(board, row, col)) {
    board_set_cross_set(board, row, col, dir, cross_set_index,
                        TRIVIAL_CROSS_SET);
    board_set_cross_score(board, row, col, dir, cross_set_index, 0);
    return;
  }

  const LetterDistribution *ld = game_get_ld(game);
  const int left_col =
      board_get_word_edge(board, row, col - 1, WORD_DIRECTION_LEFT);
  const int right_col =
      board_get_word_edge(board, row, col + 1, WORD_DIRECTION_RIGHT);
  MachineLetter before[BOARD_DIM] = {0};
  MachineLetter after[BOARD_DIM] = {0};
  const int before_length = col - left_col;
  const int after_length = right_col - col;
  for (int letter_idx = 0; letter_idx < before_length; letter_idx++) {
    before[letter_idx] = get_unblanked_machine_letter(
        board_get_letter(board, row, left_col + letter_idx));
  }
  for (int letter_idx = 0; letter_idx < after_length; letter_idx++) {
    after[letter_idx] = get_unblanked_machine_letter(
        board_get_letter(board, row, col + 1 + letter_idx));
  }
  Equity score = 0;
  if (before_length > 0) {
    score += traverse_backwards_for_score(board, ld, row, col - 1);
  }
  if (after_length > 0) {
    score += traverse_backwards_for_score(board, ld, row, right_col);
  }

  const KWG *kwg = get_kwg_for_cross_set(game, cross_set_index);
  board_set_cross_set_with_blank(
      board, row, col, dir, cross_set_index,
      kwg_compute_woadwage_cross_set(kwg, before, before_length, after,
                                     after_length));
  board_set_cross_score(board, row, col, dir, cross_set_index, score);
}

// The values game_gen_classic_cross_set writes for the empty square at
// (row, col) between the words that end at left_col and right_col, at least
// one of them nonempty, read from the lexicon.
static void game_compute_classic_cross_set(const KWG *kwg,
                                           const LetterDistribution *ld,
                                           const Board *board, int row, int col,
                                           int left_col, int right_col,
                                           CrossSetValues *values) {
  const uint32_t kwg_root = kwg_get_root_node_index(kwg);
  Equity score = 0;
  uint64_t front_hook_set = 0;
  uint64_t back_hook_set = 0;
  uint32_t right_lnode_index = 0;
  bool left_lpath_is_valid = false;
  bool right_lpath_is_valid = false;
  uint64_t leftside_leftx_set = 0;
  uint64_t leftside_rightx_set = 0;
  uint64_t rightside_leftx_set = 0;
  uint64_t rightside_rightx_set = 0;

  const bool nonempty_to_left = left_col < col;
  if (nonempty_to_left) {
    const uint32_t lnode_index =
        traverse_backwards(kwg, board, row, col - 1, kwg_root, false, 0);
    left_lpath_is_valid = lnode_index != 0;
    score += traverse_backwards_for_score(board, ld, row, col - 1);
    if (left_lpath_is_valid) {
      kwg_get_letter_sets(kwg, lnode_index, &leftside_leftx_set);
      const uint32_t s_index =
          kwg_get_next_node_index(kwg, lnode_index, SEPARATION_MACHINE_LETTER);
      if (s_index != 0) {
        back_hook_set = kwg_get_letter_sets(kwg, s_index, &leftside_rightx_set);
      }
    }
  }

  const bool nonempty_to_right = right_col > col;
  if (nonempty_to_right) {
    right_lnode_index =
        traverse_backwards(kwg, board, row, right_col, kwg_root, false, 0);
    right_lpath_is_valid = right_lnode_index != 0;
    score += traverse_backwards_for_score(board, ld, row, right_col);
    if (right_lpath_is_valid) {
      front_hook_set =
          kwg_get_letter_sets(kwg, right_lnode_index, &rightside_leftx_set);
      const uint32_t s_index = kwg_get_next_node_index(
          kwg, right_lnode_index, SEPARATION_MACHINE_LETTER);
      if (s_index != 0) {
        kwg_get_letter_sets(kwg, s_index, &rightside_rightx_set);
      }
    }
  }

  uint64_t cross_set = 0;
  if (nonempty_to_left && nonempty_to_right) {
    if (left_lpath_is_valid && right_lpath_is_valid) {
      // Visit only the arcs whose tiles are right extensions from the left
      // side of the empty square.
      const uint64_t list_tiles = kwg_get_sibling_tiles(kwg)[right_lnode_index];
      for (uint64_t remaining = list_tiles & leftside_rightx_set;
           remaining != 0; remaining &= remaining - 1) {
        const uint64_t tile_bit = remaining & (~remaining + 1);
        const uint32_t node =
            kwg_list_node(kwg, right_lnode_index, list_tiles, tile_bit);
        if (traverse_backwards(kwg, board, row, col - 1,
                               kwg_node_arc_index_prefetch(node, kwg), true,
                               left_col) != 0) {
          cross_set |= tile_bit;
        }
      }
    }
  } else if (nonempty_to_left) {
    cross_set = back_hook_set;
  } else {
    cross_set = front_hook_set;
  }
  values->cross_set = cross_set;
  values->leftside_leftx_set = leftside_leftx_set;
  values->leftside_rightx_set = leftside_rightx_set;
  values->rightside_leftx_set = rightside_leftx_set;
  values->rightside_rightx_set = rightside_rightx_set;
  values->score = score;
}

// The key of the words beside the empty square at (row, col), from left_col
// to right_col, with the lexicon and the cache's salt. Each letter, with its
// blank flag, goes in by a multiply-add, a bijection of the hash so far, and a
// separator marks the empty square, so words that differ in a letter or a
// length always differ before the finalizer. A key is never 0, which marks
// an empty entry.
static uint64_t game_cross_set_cache_key(const CrossSetCache *cache,
                                         const KWG *kwg, const Board *board,
                                         int row, int col, int left_col,
                                         int right_col) {
  uint64_t key =
      cache->salt * CROSS_SET_KEY_MULTIPLIER + (uint64_t)(uintptr_t)kwg;
  for (int word_col = left_col; word_col <= right_col; word_col++) {
    const uint64_t symbol =
        word_col == col ? CROSS_SET_KEY_SEPARATOR
                        : (uint64_t)board_get_letter(board, row, word_col) + 1;
    key = key * CROSS_SET_KEY_MULTIPLIER + symbol;
  }
  // MurmurHash3's 64-bit finalizer.
  key ^= key >> 33;
  key *= UINT64_C(0xFF51AFD7ED558CCD);
  key ^= key >> 33;
  key *= UINT64_C(0xC4CEB9FE1A85EC53);
  key ^= key >> 33;
  return key | 1;
}

static inline void game_gen_classic_cross_set(const Game *game, int row,
                                              int col, int dir,
                                              int cross_set_index) {
  if (!board_is_position_in_bounds(row, col)) {
    return;
  }

  Board *board = game_get_board(game);

  if (board_is_nonempty_or_bricked(board, row, col)) {
    board_set_cross_set(board, row, col, dir, cross_set_index, 0);
    board_set_cross_score(board, row, col, dir, cross_set_index, 0);
    return;
  }
  if (board_are_left_and_right_empty(board, row, col)) {
    board_set_cross_set(board, row, col, dir, cross_set_index,
                        TRIVIAL_CROSS_SET);
    board_set_cross_score(board, row, col, dir, cross_set_index, 0);
    return;
  }

  const KWG *kwg = get_kwg_for_cross_set(game, cross_set_index);
  const LetterDistribution *ld = game_get_ld(game);

  const int through_dir = board_toggle_dir(dir);

  const int left_col =
      board_get_word_edge(board, row, col - 1, WORD_DIRECTION_LEFT);
  const int right_col =
      board_get_word_edge(board, row, col + 1, WORD_DIRECTION_RIGHT);

  // Cache word info table block rows for the blocks flanking this empty square,
  // keyed to each block's leftmost square in the word (through) direction.
  const WordInfoTable *wit =
      get_word_info_table_for_cross_set(game, cross_set_index);
  if (wit != NULL) {
    if (left_col < col) {
      game_store_wit_block(wit, board, row, left_col, col - 1, through_dir,
                           cross_set_index);
    }
    if (right_col > col) {
      game_store_wit_block(wit, board, row, col + 1, right_col, through_dir,
                           cross_set_index);
    }
  }

  // The values depend only on the words beside the square and the lexicon,
  // so the game's cache, if it has one, answers for words it has seen.
  CrossSetValues values;
  const CrossSetCache *cache = game->cross_set_cache;
  if (cache != NULL) {
    const uint64_t key = game_cross_set_cache_key(cache, kwg, board, row, col,
                                                  left_col, right_col);
    CrossSetCacheEntry *entry = cross_set_cache_entry(cache, key);
    if (entry->key == key) {
      values = entry->values;
#ifndef NDEBUG
      CrossSetValues computed;
      game_compute_classic_cross_set(kwg, ld, board, row, col, left_col,
                                     right_col, &computed);
      assert(computed.cross_set == values.cross_set &&
             computed.leftside_leftx_set == values.leftside_leftx_set &&
             computed.leftside_rightx_set == values.leftside_rightx_set &&
             computed.rightside_leftx_set == values.rightside_leftx_set &&
             computed.rightside_rightx_set == values.rightside_rightx_set &&
             computed.score == values.score);
#endif
    } else {
      game_compute_classic_cross_set(kwg, ld, board, row, col, left_col,
                                     right_col, &values);
      entry->key = key;
      entry->values = values;
    }
  } else {
    game_compute_classic_cross_set(kwg, ld, board, row, col, left_col,
                                   right_col, &values);
  }

  if (left_col < col) {
    board_set_left_extension_set_with_blank(board, row, col - 1, through_dir,
                                            cross_set_index,
                                            values.leftside_leftx_set);
    board_set_right_extension_set_with_blank(board, row, col - 1, through_dir,
                                             cross_set_index,
                                             values.leftside_rightx_set);
    // Mark the empty square left of the leftside played tiles with the leftx
    // set for this sequence of tiles. Move generation can use this to avoid
    // trying to play through tiles we can prove to be dead ends.
    if (left_col > 0) {
      board_set_left_extension_set_with_blank(board, row, left_col - 1,
                                              through_dir, cross_set_index,
                                              values.leftside_leftx_set);
    }
  }
  if (right_col > col) {
    board_set_left_extension_set_with_blank(board, row, right_col, through_dir,
                                            cross_set_index,
                                            values.rightside_leftx_set);
    board_set_right_extension_set_with_blank(board, row, right_col, through_dir,
                                             cross_set_index,
                                             values.rightside_rightx_set);
    // Mark this empty square with the leftx set for rightside played tiles.
    board_set_left_extension_set_with_blank(board, row, col, through_dir,
                                            cross_set_index,
                                            values.rightside_leftx_set);
  }
  board_set_cross_set_with_blank(board, row, col, dir, cross_set_index,
                                 values.cross_set);
  board_set_cross_score(board, row, col, dir, cross_set_index, values.score);
}

void game_gen_cross_set(const Game *game, int row, int col, int dir,
                        int cross_set_index) {
  const game_variant_t variant =
      game_variant_word_rules(game_get_variant(game));
  if (variant == GAME_VARIANT_CLASSIC) {
    game_gen_classic_cross_set(game, row, col, dir, cross_set_index);
  } else if (variant == GAME_VARIANT_WOADWAGE) {
    game_gen_woadwage_cross_set(game, row, col, dir, cross_set_index);
  } else {
    game_gen_alpha_cross_set(game, row, col, dir, cross_set_index);
  }
}

void game_gen_all_cross_sets(const Game *game) {
  Board *board = game_get_board(game);
  board_clear_wit_cache(board);
  bool kwgs_are_shared = game_get_data_is_shared(game, PLAYERS_DATA_TYPE_KWG);

  // We only use the vertical direction here since the board
  // direction changes with transposition. Each cross set write
  // will make the corresponding write in the opposite direction
  // on the other grid. See board.h for more details.
  for (int i = 0; i < BOARD_DIM; i++) {
    for (int j = 0; j < BOARD_DIM; j++) {
      game_gen_cross_set(game, i, j, BOARD_VERTICAL_DIRECTION, 0);
      if (!kwgs_are_shared) {
        game_gen_cross_set(game, i, j, BOARD_VERTICAL_DIRECTION, 1);
      }
    }
  }
  board_transpose(board);
  for (int i = 0; i < BOARD_DIM; i++) {
    for (int j = 0; j < BOARD_DIM; j++) {
      game_gen_cross_set(game, i, j, BOARD_VERTICAL_DIRECTION, 0);
      if (!kwgs_are_shared) {
        game_gen_cross_set(game, i, j, BOARD_VERTICAL_DIRECTION, 1);
      }
    }
  }
  board_transpose(board);
}

void game_reset(Game *game) {
  board_reset(game->board);
  bag_reset(game->ld, game->bag);
  player_reset(game->players[0]);
  player_reset(game->players[1]);
  game->player_on_turn_index = 0;
  game->starting_player_index = 0;
  game->consecutive_scoreless_turns = 0;
  game->game_end_reason = GAME_END_REASON_NONE;
  game->backup_cursor = 0;
}

// Sets the bag state with the given seed, ensuring the
// next iteration will be consistent. Any leftover random
// racks should be returned before calling this function.
void game_seed(Game *game, uint64_t seed) { bag_seed(game->bag, seed); }

// This assumes the game has not started yet.
void game_set_starting_player_index(Game *game, int starting_player_index) {
  game->starting_player_index = starting_player_index;
  game->player_on_turn_index = starting_player_index;
}

void pre_allocate_single_game_backup(MinimalGameBackup *mgb, const Game *game) {
  const LetterDistribution *ld = game_get_ld(game);
  const int ld_size = ld_get_size(ld);
  mgb->bag = bag_create(ld, 0);
  mgb->board = board_duplicate(game_get_board(game));
  mgb->p0rack = rack_create(ld_size);
  mgb->p1rack = rack_create(ld_size);
}

void game_set_backup_mode(Game *game, backup_mode_t backup_mode) {
  game->backup_mode = backup_mode;
  switch (game->backup_mode) {
  case BACKUP_MODE_OFF:
    break;
  case BACKUP_MODE_SIMULATION:
    game->sim_backups_enabled = true;
    break;
  case BACKUP_MODE_GCG:
    if (!game->gcg_game_backup) {
      game->gcg_game_backup = malloc_or_die(sizeof(MinimalGameBackup));
      pre_allocate_single_game_backup(game->gcg_game_backup, game);
    }
    break;
  }
}

void game_set_cross_set_cache(Game *game, CrossSetCache *cache) {
  game->cross_set_cache = cache;
}

void game_enable_rollout_board_undo(Game *game) {
  if (game->rollout_board_undo == NULL) {
    game->rollout_board_undo = malloc_or_die(sizeof(MoveUndo));
  }
  game->rollout_board_undo_open = false;
}

MoveUndo *game_get_open_rollout_board_undo(const Game *game) {
  return game->rollout_board_undo_open ? game->rollout_board_undo : NULL;
}

void game_update(Game *game, const GameArgs *game_args) {
  game->ld = game_args->ld;
  game->bingo_bonus = int_to_equity(game_args->bingo_bonus);
  for (int player_index = 0; player_index < 2; player_index++) {
    player_update(game_args->players_data, game->players[player_index]);
  }
  for (int i = 0; i < NUMBER_OF_DATA; i++) {
    game->data_is_shared[i] =
        players_data_get_is_shared(game_args->players_data, (players_data_t)i);
  }
  board_apply_layout(game_args->board_layout, game->board);

  game->variant = game_args->game_variant;
  // The owner may have replaced or reloaded lexical data, including at the
  // same address. Both cross sets and borrowed WIT rows must match the new
  // tables; clearing only WIT would leave the old lexicon's cross sets.
  game_gen_all_cross_sets(game);
}

Game *game_create(const GameArgs *game_args) {
  Game *game = malloc_or_die(sizeof(Game));
  game->ld = game_args->ld;
  game->bingo_bonus = int_to_equity(game_args->bingo_bonus);
  game->bag = bag_create(game->ld, game_args->seed);
  game->board = board_create(game_args->board_layout);
  for (int player_index = 0; player_index < 2; player_index++) {
    game->players[player_index] =
        player_create(game_args->players_data, game_args->ld, player_index);
  }
  for (int i = 0; i < NUMBER_OF_DATA; i++) {
    game->data_is_shared[i] =
        players_data_get_is_shared(game_args->players_data, (players_data_t)i);
  }

  game->starting_player_index = 0;
  game->player_on_turn_index = 0;
  game->consecutive_scoreless_turns = 0;
  game->max_scoreless_turns = MAX_SCORELESS_TURNS;
  game->game_end_reason = GAME_END_REASON_NONE;
  game->variant = game_args->game_variant;
  game->override_kwgs[0] = NULL;
  game->override_kwgs[1] = NULL;
  game->dual_lexicon_mode = DUAL_LEXICON_MODE_IGNORANT;

  for (int i = 0; i < MAX_SEARCH_DEPTH; i++) {
    game->sim_game_backups[i] = NULL;
  }
  game->sim_backups_enabled = false;
  game->backup_cursor = 0;
  game->backup_mode = BACKUP_MODE_OFF;
  game->gcg_game_backup = NULL;
  game->rollout_board_undo = NULL;
  game->cross_set_cache = NULL;
  game->rollout_board_undo_open = false;
  return game;
}

// This creates a move list and a generator but does
// not copy them from game.
#ifdef IF_ONLY
// If Only: the player swaps the fake blank at (row, col) for the matching real
// tile from their rack. The square keeps its letter, now a real tile, and the
// rack takes the tile that was under the fake blank. Cross sets are left to
// the caller: only the scores of the words through the square change.
void game_if_only_swap(const Game *game, int player_index, int row, int col) {
  Board *board = game_get_board(game);
  assert(board_is_fake_blank(board, row, col));
  const MachineLetter letter =
      get_unblanked_machine_letter(board_get_letter(board, row, col));
  Rack *rack = player_get_rack(game_get_player(game, player_index));
  rack_take_letter(rack, letter);
  rack_add_letter(rack, board_get_fake_blank_tile(board, row, col));
  board_set_letter(board, row, col, letter);
  board_clear_fake_blank(board, row, col);
}
#endif

Game *game_duplicate(const Game *game) {
  Game *new_game = malloc_or_die(sizeof(Game));
  new_game->bag = bag_duplicate(game->bag);
  new_game->board = board_duplicate(game->board);
  new_game->ld = game->ld;
  new_game->bingo_bonus = game->bingo_bonus;

  for (int j = 0; j < 2; j++) {
    new_game->players[j] = player_duplicate(game->players[j]);
  }
  for (int i = 0; i < NUMBER_OF_DATA; i++) {
    new_game->data_is_shared[i] = game->data_is_shared[i];
  }
  new_game->player_on_turn_index = game->player_on_turn_index;
  new_game->starting_player_index = game->starting_player_index;
  new_game->consecutive_scoreless_turns = game->consecutive_scoreless_turns;
  new_game->max_scoreless_turns = game->max_scoreless_turns;
  new_game->game_end_reason = game->game_end_reason;
  new_game->variant = game->variant;
  new_game->override_kwgs[0] = game->override_kwgs[0];
  new_game->override_kwgs[1] = game->override_kwgs[1];
  new_game->dual_lexicon_mode = game->dual_lexicon_mode;

  // note: game backups must be explicitly handled by the caller if they want
  // game copies to have backups.
  for (int i = 0; i < MAX_SEARCH_DEPTH; i++) {
    new_game->sim_game_backups[i] = NULL;
  }
  new_game->sim_backups_enabled = false;
  new_game->backup_cursor = 0;
  new_game->backup_mode = BACKUP_MODE_OFF;
  new_game->gcg_game_backup = NULL;
  new_game->rollout_board_undo = NULL;
  new_game->cross_set_cache = NULL;
  new_game->rollout_board_undo_open = false;
  return new_game;
}

void game_copy(Game *dst, const Game *src) {
  bag_copy(dst->bag, src->bag);
  board_copy(dst->board, src->board);
  dst->ld = src->ld;
  dst->bingo_bonus = src->bingo_bonus;

  for (int j = 0; j < 2; j++) {
    player_copy(dst->players[j], src->players[j]);
  }
  for (int i = 0; i < NUMBER_OF_DATA; i++) {
    dst->data_is_shared[i] = src->data_is_shared[i];
  }
  dst->player_on_turn_index = src->player_on_turn_index;
  dst->starting_player_index = src->starting_player_index;
  dst->consecutive_scoreless_turns = src->consecutive_scoreless_turns;
  dst->max_scoreless_turns = src->max_scoreless_turns;
  dst->game_end_reason = src->game_end_reason;
  dst->variant = src->variant;
  dst->override_kwgs[0] = src->override_kwgs[0];
  dst->override_kwgs[1] = src->override_kwgs[1];
  dst->dual_lexicon_mode = src->dual_lexicon_mode;
  dst->backup_cursor = 0;
  dst->backup_mode = BACKUP_MODE_OFF;
  dst->rollout_board_undo_open = false;
}

// Backups do not restore the move list or
// generator.
void game_backup(Game *game) {
  MinimalGameBackup *state = NULL;
  switch (game->backup_mode) {
  case BACKUP_MODE_OFF:
    return;
    break;
  case BACKUP_MODE_SIMULATION:
    if (!game->sim_game_backups[game->backup_cursor]) {
      game->sim_game_backups[game->backup_cursor] =
          malloc_or_die(sizeof(MinimalGameBackup));
      pre_allocate_single_game_backup(
          game->sim_game_backups[game->backup_cursor], game);
    }
    state = game->sim_game_backups[game->backup_cursor];
    game->backup_cursor++;
    break;
  case BACKUP_MODE_GCG:
    state = game->gcg_game_backup;
    break;
  }
  // The board's WIT cache is no Square, so a record of squares cannot restore
  // it: a populated one is copied with the board.
  state->board_recorded = game->backup_mode == BACKUP_MODE_SIMULATION &&
                          game->rollout_board_undo != NULL &&
                          !game->rollout_board_undo_open &&
                          !board_get_wit_cache_populated(game->board);
  if (state->board_recorded) {
    MoveUndo *undo = game->rollout_board_undo;
    move_undo_reset(undo);
    undo->old_tiles_played = board_get_tiles_played(game->board);
    memcpy(undo->old_number_of_row_anchors, game->board->number_of_row_anchors,
           sizeof(undo->old_number_of_row_anchors));
    undo->old_cross_sets_valid = board_get_cross_sets_valid(game->board);
    game->rollout_board_undo_open = true;
  } else {
    board_copy(state->board, game->board);
  }
  bag_copy(state->bag, game->bag);
  state->game_end_reason = game->game_end_reason;
  state->player_on_turn_index = game->player_on_turn_index;
  state->starting_player_index = game->starting_player_index;
  state->consecutive_scoreless_turns = game->consecutive_scoreless_turns;
  const Player *player0 = game->players[0];
  const Player *player1 = game->players[1];
  rack_copy(state->p0rack, player_get_rack(player0));
  state->p0score = player_get_score(player0);
  rack_copy(state->p1rack, player_get_rack(player1));
  state->p1score = player_get_score(player1);
}

void game_unplay_last_move(Game *game) {
  // restore from backup (pop the last element).
  const MinimalGameBackup *state = NULL;
  // Since there are two kinds of backups and a restore from backup can occur
  // with the backup mode off, we check which kind of backup to perform by
  // checking for sim backups first.
  if (game->sim_backups_enabled) {
    if (game->backup_cursor == 0) {
      log_fatal("cannot unplay last move without a game backup");
    }
    // cppcheck-suppress negativeIndex
    state = game->sim_game_backups[game->backup_cursor - 1];
    game->backup_cursor--;
  } else {
    state = game->gcg_game_backup;
  }

  game->consecutive_scoreless_turns = state->consecutive_scoreless_turns;
  game->game_end_reason = state->game_end_reason;
  game->player_on_turn_index = state->player_on_turn_index;
  game->starting_player_index = state->starting_player_index;

  Player *player0 = game->players[0];
  Player *player1 = game->players[1];
  player_set_score(player0, state->p0score);
  player_set_score(player1, state->p1score);
  rack_copy(player_get_rack(player0), state->p0rack);
  rack_copy(player_get_rack(player1), state->p1rack);
  bag_copy(game->bag, state->bag);
  if (state->board_recorded) {
    // A WIT cache the moves populated describes their board, not this one.
    const MoveUndo *undo = game->rollout_board_undo;
    move_undo_restore_squares(undo, game->board);
    board_clear_wit_cache(game->board);
    board_set_tiles_played(game->board, undo->old_tiles_played);
    memcpy(game->board->number_of_row_anchors, undo->old_number_of_row_anchors,
           sizeof(game->board->number_of_row_anchors));
    board_set_cross_sets_valid(game->board, undo->old_cross_sets_valid);
    game->rollout_board_undo_open = false;
  } else {
    board_copy(game->board, state->board);
  }
}

void single_game_backup_destroy(MinimalGameBackup *mgb) {
  rack_destroy(mgb->p0rack);
  rack_destroy(mgb->p1rack);
  bag_destroy(mgb->bag);
  board_destroy(mgb->board);
  free(mgb);
}

void backups_destroy(Game *game) {
  if (game->gcg_game_backup) {
    single_game_backup_destroy(game->gcg_game_backup);
  }
  for (int i = 0; i < MAX_SEARCH_DEPTH; i++) {
    if (game->sim_game_backups[i]) {
      single_game_backup_destroy(game->sim_game_backups[i]);
    }
  }
}

// This does not destroy the letter distribution,
// the caller must handle that.
void game_destroy(Game *game) {
  if (!game) {
    return;
  }
  board_destroy(game->board);
  bag_destroy(game->bag);
  player_destroy(game->players[0]);
  player_destroy(game->players[1]);
  backups_destroy(game);
  free(game->rollout_board_undo);
  free(game);
}

int game_get_max_scoreless_turns(const Game *game) {
  return game->max_scoreless_turns;
}

bool game_reached_max_scoreless_turns(const Game *game) {
  return game->consecutive_scoreless_turns >= game->max_scoreless_turns;
}
