#include "inference_move_gen.h"

#include "../def/board_defs.h"
#include "../def/cross_set_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/move_defs.h"
#include "../def/rack_defs.h"
#include "../ent/bit_rack.h"
#include "../ent/board.h"
#include "../ent/klv.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/player.h"
#include "../str/move_string.h"
#include "../str/rack_string.h"
#include "../util/math_util.h"
#include "../util/string_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Debug flag - set to 0 for production
#define DEBUG_INFERENCE_MOVEGEN 0

// Internal state for the custom board search
typedef struct InferenceMoveGenState {
  const InferenceMoveGenArgs *args;
  const LetterDistribution *ld;
  int ld_size;
  const KLV *klv;
  const KWG *kwg;
  const Board *board;
  Equity bingo_bonus;
  Equity tile_scores[MAX_ALPHABET_SIZE + BLANK_MASK];
  Square lanes_cache[BOARD_DIM * BOARD_DIM * 2];

  // Tiles available for placing on board (full bag contents)
  Rack available_tiles;
  // Original bag contents (for draw counting)
  Rack original_bag;

  // Board search state
  Square row_cache[BOARD_DIM];
  MachineLetter strip[BOARD_DIM];  // Current word being built
  Rack tiles_played_rack;          // Tiles played from hand in current play
  int tiles_played;                // Count of tiles played from hand
  int current_anchor_col;
  int current_row_index;
  int dir;
  int last_anchor_col;

  // For leave enumeration within record_play
  Rack remaining_bag;       // Bag after removing tiles_played
  Rack current_leave;       // Leave being enumerated
  Rack full_rack;           // tiles_played_rack + current_leave
  int score;                // Score of current play
  MachineLetter move_tiles[BOARD_DIM];
  int tiles_length;
  int row_start;
  int col_start;

  // Scratch space for exchange enumeration
  MachineLetter exchange_tiles[RACK_SIZE];
  Rack exchange_rack;

  // Debug counters
  int play_count;
  int rack_count;
  int exchange_count;
} InferenceMoveGenState;

// Forward declarations
static void search_board_for_plays(InferenceMoveGenState *state);
static void search_row(InferenceMoveGenState *state, int row, int dir);
static void recursive_gen(InferenceMoveGenState *state, int col,
                          uint32_t node_index, int leftstrip, int rightstrip,
                          bool unique_play);
static void go_on(InferenceMoveGenState *state, int col, MachineLetter L,
                  uint32_t new_node_index, bool accepts, int leftstrip,
                  int rightstrip, bool unique_play);
static void record_play(InferenceMoveGenState *state, int leftstrip,
                        int rightstrip);

// Row cache accessors
static inline MachineLetter cache_get_letter(const InferenceMoveGenState *state,
                                             int col) {
  return square_get_letter(&state->row_cache[col]);
}

static inline bool cache_is_empty(const InferenceMoveGenState *state, int col) {
  return cache_get_letter(state, col) == ALPHABET_EMPTY_SQUARE_MARKER;
}

static inline uint64_t cache_get_cross_set(const InferenceMoveGenState *state,
                                           int col) {
  return square_get_cross_set(&state->row_cache[col]);
}

static inline uint64_t
cache_get_left_extension_set(const InferenceMoveGenState *state, int col) {
  return state->row_cache[col].left_extension_set;
}

static inline uint64_t
cache_get_right_extension_set(const InferenceMoveGenState *state, int col) {
  return state->row_cache[col].right_extension_set;
}

static inline bool cache_get_anchor(const InferenceMoveGenState *state,
                                    int col) {
  return square_get_anchor(&state->row_cache[col]);
}

static inline Equity cache_get_cross_score(const InferenceMoveGenState *state,
                                           int col) {
  return square_get_cross_score(&state->row_cache[col]);
}

static inline bool cache_get_is_cross_word(const InferenceMoveGenState *state,
                                           int col) {
  return state->row_cache[col].is_cross_word;
}

static inline BonusSquare
cache_get_bonus_square(const InferenceMoveGenState *state, int col) {
  return square_get_bonus_square(&state->row_cache[col]);
}

// Main entry point
void generate_rack_moves_from_bag(const InferenceMoveGenArgs *args) {
  InferenceMoveGenState state;
  memset(&state, 0, sizeof(state));

  state.args = args;
  state.ld = game_get_ld(args->game);
  state.ld_size = ld_get_size(state.ld);
  state.klv = player_get_klv(game_get_player(args->game, args->target_index));
  state.kwg = args->override_kwg ? args->override_kwg
                                 : player_get_kwg(game_get_player(
                                       args->game, args->target_index));
  state.board = game_get_board(args->game);
  state.bingo_bonus = game_get_bingo_bonus(args->game);

  // Initialize tile scores
  memset(state.tile_scores, 0, sizeof(state.tile_scores));
  for (int i = 0; i < state.ld_size; i++) {
    state.tile_scores[i] = ld_get_score(state.ld, i);
    state.tile_scores[get_blanked_machine_letter(i)] =
        ld_get_score(state.ld, BLANK_MACHINE_LETTER);
  }

  // Load board lanes cache
  board_load_lanes_cache(state.board, 0, state.lanes_cache);

  // Initialize racks
  rack_set_dist_size(&state.available_tiles, state.ld_size);
  rack_set_dist_size(&state.original_bag, state.ld_size);
  rack_set_dist_size(&state.tiles_played_rack, state.ld_size);
  rack_set_dist_size(&state.remaining_bag, state.ld_size);
  rack_set_dist_size(&state.current_leave, state.ld_size);
  rack_set_dist_size(&state.full_rack, state.ld_size);
  rack_set_dist_size(&state.exchange_rack, state.ld_size);

  // Copy bag contents - this is our pool for placing tiles
  rack_copy(&state.available_tiles, args->bag_as_rack);
  rack_copy(&state.original_bag, args->bag_as_rack);

#if DEBUG_INFERENCE_MOVEGEN
  printf("generate_rack_moves_from_bag: ENTER\n");
  printf("  bag has %d tiles\n", rack_get_total_letters(&state.original_bag));
  fflush(stdout);
#endif

  // Search board for all valid plays using GADDAG
  search_board_for_plays(&state);

#if DEBUG_INFERENCE_MOVEGEN
  printf("Board search: Found %d plays, recorded %d rack entries\n",
         state.play_count, state.rack_count);
  fflush(stdout);
#endif

  // Note: Exchange processing is deferred to inference.c filtering phase
  // to avoid exponential enumeration of all possible exchange subsets.
  // Exchanges are handled by checking if target_num_exch > 0 in inference.c.

#if DEBUG_INFERENCE_MOVEGEN
  printf("=== SUMMARY: plays=%d, rack_entries=%d ===\n",
         state.play_count, state.rack_count);
  fflush(stdout);
#endif
}

// Search the board for all valid plays
static void search_board_for_plays(InferenceMoveGenState *state) {
  // Search horizontal plays (dir=0)
  for (int row = 0; row < BOARD_DIM; row++) {
    search_row(state, row, BOARD_HORIZONTAL_DIRECTION);
  }

  // Search vertical plays (dir=1)
  for (int col = 0; col < BOARD_DIM; col++) {
    search_row(state, col, BOARD_VERTICAL_DIRECTION);
  }
}

// Search a single row for plays
static void search_row(InferenceMoveGenState *state, int row, int dir) {
  state->current_row_index = row;
  state->dir = dir;

  // Load row cache from pre-loaded lanes cache
  const Square *src = board_get_row_cache(state->lanes_cache, row, dir);
  memcpy(state->row_cache, src, BOARD_DIM * sizeof(Square));

  // Find anchors and process them
  int last_anchor = -2;  // -2 means no anchor seen yet
  for (int col = 0; col < BOARD_DIM; col++) {
    if (cache_get_anchor(state, col)) {
      state->current_anchor_col = col;
      state->last_anchor_col = last_anchor;

      // Reset for this anchor
      state->tiles_played = 0;
      rack_reset(&state->tiles_played_rack);

      uint32_t root = kwg_get_root_node_index(state->kwg);
      recursive_gen(state, col, root, col, col,
                    dir == BOARD_HORIZONTAL_DIRECTION);

      last_anchor = col;
    }
  }
}

// Recursive GADDAG traversal
static void recursive_gen(InferenceMoveGenState *state, int col,
                          uint32_t node_index, int leftstrip, int rightstrip,
                          bool unique_play) {
  MachineLetter current_letter = cache_get_letter(state, col);
  uint64_t possible_letters_here =
      cache_get_cross_set(state, col) & cache_get_left_extension_set(state, col);

  if ((state->tiles_played == 0) && (col == state->current_anchor_col + 1)) {
    possible_letters_here &= cache_get_right_extension_set(state, state->current_anchor_col);
  }

  if (possible_letters_here == 1) {
    possible_letters_here = 0;
  }

  if (current_letter != ALPHABET_EMPTY_SQUARE_MARKER) {
    // Play through existing tile
    MachineLetter raw = get_unblanked_machine_letter(current_letter);
    uint32_t next_node_index = 0;
    bool accepts = false;

    for (uint32_t i = node_index;; i++) {
      uint32_t node = kwg_node(state->kwg, i);
      if (kwg_node_tile(node) == raw) {
        next_node_index = kwg_node_arc_index(node);
        accepts = kwg_node_accepts(node);
        break;
      }
      if (kwg_node_is_end(node)) {
        break;
      }
    }

    go_on(state, col, current_letter, next_node_index, accepts, leftstrip,
          rightstrip, unique_play);
  } else if (state->tiles_played < RACK_SIZE) {
    // Try placing tiles from available pool
    for (uint32_t i = node_index;; i++) {
      uint32_t node = kwg_node(state->kwg, i);
      MachineLetter ml = kwg_node_tile(node);

      if (ml != 0 && board_is_letter_allowed_in_cross_set(possible_letters_here, ml)) {
        int available = rack_get_letter(&state->available_tiles, ml);
        int blank_available =
            rack_get_letter(&state->available_tiles, BLANK_MACHINE_LETTER);

        if (available > 0 || blank_available > 0) {
          uint32_t next_node_index = kwg_node_arc_index(node);
          bool accepts = kwg_node_accepts(node);

          // Try regular tile
          if (available > 0) {
            rack_take_letter(&state->available_tiles, ml);
            rack_add_letter(&state->tiles_played_rack, ml);
            state->tiles_played++;

            go_on(state, col, ml, next_node_index, accepts, leftstrip,
                  rightstrip, unique_play);

            state->tiles_played--;
            rack_take_letter(&state->tiles_played_rack, ml);
            rack_add_letter(&state->available_tiles, ml);
          }

          // Try blank
          if (blank_available > 0) {
            rack_take_letter(&state->available_tiles, BLANK_MACHINE_LETTER);
            rack_add_letter(&state->tiles_played_rack, BLANK_MACHINE_LETTER);
            state->tiles_played++;

            go_on(state, col, get_blanked_machine_letter(ml), next_node_index,
                  accepts, leftstrip, rightstrip, unique_play);

            state->tiles_played--;
            rack_take_letter(&state->tiles_played_rack, BLANK_MACHINE_LETTER);
            rack_add_letter(&state->available_tiles, BLANK_MACHINE_LETTER);
          }
        }
      }

      if (kwg_node_is_end(node)) {
        break;
      }
    }
  }
}

// Continue building word after placing a tile
static void go_on(InferenceMoveGenState *state, int col, MachineLetter L,
                  uint32_t new_node_index, bool accepts, int leftstrip,
                  int rightstrip, bool unique_play) {
  bool square_is_empty = cache_is_empty(state, col);

  if (!square_is_empty) {
    state->strip[col] = PLAYED_THROUGH_MARKER;
  } else {
    state->strip[col] = L;
  }

  if (col <= state->current_anchor_col) {
    if (square_is_empty && state->dir &&
        cache_get_cross_set(state, col) == TRIVIAL_CROSS_SET) {
      unique_play = true;
    }
    leftstrip = col;

    bool no_letter_left = (col == 0) || cache_is_empty(state, col - 1);
    bool valid_play = (state->tiles_played > 1) ||
                      ((state->tiles_played == 1) && unique_play);

    if (accepts && no_letter_left && valid_play) {
      record_play(state, leftstrip, rightstrip);
    }

    if (new_node_index == 0) {
      return;
    }

    // Extend left
    if (col > 0 && col - 1 != state->last_anchor_col) {
      recursive_gen(state, col - 1, new_node_index, leftstrip, rightstrip,
                    unique_play);
    }

    // Cross over to right side (GADDAG separation)
    uint32_t sep_node =
        kwg_get_next_node_index(state->kwg, new_node_index, SEPARATION_MACHINE_LETTER);
    if (sep_node != 0 && no_letter_left &&
        state->current_anchor_col < BOARD_DIM - 1) {
      recursive_gen(state, state->current_anchor_col + 1, sep_node, leftstrip,
                    rightstrip, unique_play);
    }
  } else {
    if (square_is_empty && !unique_play && state->dir &&
        cache_get_cross_set(state, col) == TRIVIAL_CROSS_SET) {
      unique_play = true;
    }
    rightstrip = col;

    bool no_letter_right =
        (col == BOARD_DIM - 1) || cache_is_empty(state, col + 1);
    bool valid_play = (state->tiles_played > 1) ||
                      ((state->tiles_played == 1) && unique_play);

    if (accepts && no_letter_right && valid_play) {
      record_play(state, leftstrip, rightstrip);
    }

    // Extend right
    if (new_node_index != 0 && col < BOARD_DIM - 1) {
      recursive_gen(state, col + 1, new_node_index, leftstrip, rightstrip,
                    unique_play);
    }
  }
}

// Record a valid play - store keyed by tiles_played (NO leave enumeration here)
static void record_play(InferenceMoveGenState *state, int leftstrip,
                        int rightstrip) {
#if DEBUG_INFERENCE_MOVEGEN
  state->play_count++;
#endif

  // Compute score
  Equity main_word_score = 0;
  Equity cross_scores = 0;
  int word_multiplier = 1;

  for (int col = leftstrip; col <= rightstrip; col++) {
    MachineLetter ml = state->strip[col];
    if (ml == PLAYED_THROUGH_MARKER) {
      ml = cache_get_letter(state, col);
      main_word_score += state->tile_scores[ml];
    } else {
      BonusSquare bs = cache_get_bonus_square(state, col);
      int letter_mult = bonus_square_get_letter_multiplier(bs);
      int word_mult = bonus_square_get_word_multiplier(bs);
      word_multiplier *= word_mult;

      Equity tile_score = state->tile_scores[ml] * letter_mult;
      main_word_score += tile_score;

      if (cache_get_is_cross_word(state, col)) {
        cross_scores +=
            (tile_score + cache_get_cross_score(state, col)) * word_mult;
      }
    }
  }

  Equity score = main_word_score * word_multiplier + cross_scores;
  if (state->tiles_played == RACK_SIZE) {
    score += state->bingo_bonus;
  }

  // Build move tiles array
  int tiles_length = rightstrip - leftstrip + 1;
  MachineLetter move_tiles[BOARD_DIM];
  for (int i = 0; i < tiles_length; i++) {
    move_tiles[i] = state->strip[leftstrip + i];
  }
  int row_start = state->dir == BOARD_HORIZONTAL_DIRECTION ? state->current_row_index
                                                           : leftstrip;
  int col_start = state->dir == BOARD_HORIZONTAL_DIRECTION
                      ? leftstrip
                      : state->current_row_index;

  // Create move (leave_value will be computed later during filtering)
  Move move;
  move_set_all(&move, move_tiles, 0, tiles_length - 1,
               score, row_start, col_start,
               state->tiles_played, state->dir, GAME_EVENT_TILE_PLACEMENT_MOVE,
               0);  // leave_value = 0 for now

  // Key by tiles_played (not full_rack)
  // This stores the play for later leave enumeration during filtering
  BitRack bit_rack = bit_rack_create_from_rack(state->ld, &state->tiles_played_rack);

  // Add to rack hash table keyed by tiles_played
  // draws=0, weight=1.0 - these will be computed during filtering
  rack_hash_table_add_move(state->args->rack_hash_table, &bit_rack,
                           0, 0, 1.0f, &move);

#if DEBUG_INFERENCE_MOVEGEN
  state->rack_count++;
#endif
}

// Note: Leave enumeration and exchange processing have been moved to inference.c
// to avoid exponential explosion during board search.
// The RHT is now keyed by tiles_played, and leave enumeration happens during
// filtering, only for plays that match target_played_tiles.
