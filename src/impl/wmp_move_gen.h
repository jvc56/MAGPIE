#ifndef WMP_MOVEGEN_H
#define WMP_MOVEGEN_H

#include "../def/bit_rack_defs.h"
#include "../def/kwg_defs.h"
#include "../def/move_defs.h"
#include "../ent/anchor.h"
#include "../ent/bit_rack.h"
#include "../ent/board.h"
#include "../ent/leave_map.h"
#include "../ent/wmp.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

enum {
  MAX_POSSIBLE_PLAYTHROUGH_BLOCKS = ((BOARD_DIM / 2) + 1),
  MAX_WMP_MOVE_GEN_ANCHORS =
      ((RACK_SIZE + 1) * MAX_POSSIBLE_PLAYTHROUGH_BLOCKS),
};

typedef struct SubrackInfo {
  BitRack subrack;
  const WMPEntry *wmp_entry;
  Equity leave_value;
} SubrackInfo;

// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding)
typedef struct WMPMoveGen {
  const WMP *wmp;
  BitRack player_bit_rack;
  int full_rack_size;

  // Tiles already played on board
  BitRack playthrough_bit_rack;
  int num_tiles_played_through;
  // Copies of above to reset after finishing shadow_play_right
  BitRack playthrough_bit_rack_copy;
  int num_tiles_played_through_copy;
  uint8_t playthrough_pattern[BOARD_DIM];
  int leftmost_start_col;
  int rightmost_start_col;
  int tiles_to_connect_playthrough;

  SubrackInfo nonplaythrough_infos[1 << RACK_SIZE];
  SubrackInfo playthrough_infos[1 << RACK_SIZE];
  Equity nonplaythrough_best_leave_values[RACK_SIZE + 1];
  bool nonplaythrough_has_word_of_length[RACK_SIZE + 1];
  uint8_t count_by_size[RACK_SIZE + 1];

  Anchor anchors[MAX_WMP_MOVE_GEN_ANCHORS];
  int playthrough_blocks;
  int playthrough_blocks_copy;

  uint8_t buffer[WMP_RESULT_BUFFER_SIZE];
  int tiles_to_play;
  int word_length;
  int num_words;
  Equity leave_value;
} WMPMoveGen;

static inline void wmp_move_gen_reset_anchors(WMPMoveGen *wmp_move_gen) {
  for (int i = 0; i < MAX_WMP_MOVE_GEN_ANCHORS; i++) {
    wmp_move_gen->anchors[i].highest_possible_equity = EQUITY_MIN_VALUE;
    wmp_move_gen->anchors[i].highest_possible_score = EQUITY_MIN_VALUE;
    wmp_move_gen->anchors[i].tiles_to_play = 0;
  }
}

static inline void wmp_move_gen_init(WMPMoveGen *wmp_move_gen,
                                     const LetterDistribution *ld,
                                     const Rack *player_rack, const WMP *wmp) {
  wmp_move_gen->wmp = wmp;
  if (wmp == NULL || player_rack == NULL || ld == NULL) {
    return;
  }
  wmp_move_gen->player_bit_rack = bit_rack_create_from_rack(ld, player_rack);
  wmp_move_gen->full_rack_size = rack_get_total_letters(player_rack);
  memset(wmp_move_gen->nonplaythrough_has_word_of_length, false,
         sizeof(wmp_move_gen->nonplaythrough_has_word_of_length));
}

static inline bool wmp_move_gen_is_active(const WMPMoveGen *wmp_move_gen) {
  return wmp_move_gen->wmp != NULL;
}

static const uint8_t subracks_combination_offsets[] = {
    BIT_RACK_COMBINATION_OFFSETS};

static inline uint8_t subracks_get_combination_offset(int size) {
  return subracks_combination_offsets[size];
}

static inline void
wmp_move_gen_enumerate_nonplaythrough_subracks(WMPMoveGen *wmp_move_gen,
                                               BitRack *current, int next_ml,
                                               int count, LeaveMap *leave_map) {
  int max_num_this = 0;
  for (; next_ml < BIT_RACK_MAX_ALPHABET_SIZE; next_ml++) {
    max_num_this = bit_rack_get_letter(&wmp_move_gen->player_bit_rack, next_ml);
    if (max_num_this > 0) {
      break;
    }
  }
  if (next_ml >= BIT_RACK_MAX_ALPHABET_SIZE) {
    const int insert_index = subracks_get_combination_offset(count) +
                             wmp_move_gen->count_by_size[count];
    SubrackInfo *subrack_info =
        wmp_move_gen->nonplaythrough_infos + insert_index;
    subrack_info->subrack = *current;
    subrack_info->leave_value = leave_map_get_current_value(leave_map);
    wmp_move_gen->count_by_size[count]++;
    return;
  }
  for (int i = 0; i < max_num_this; i++) {
    wmp_move_gen_enumerate_nonplaythrough_subracks(
        wmp_move_gen, current, next_ml + 1, count + i, leave_map);
    bit_rack_add_letter(current, next_ml);
    leave_map_complement_add_letter(leave_map, next_ml, i);
  }
  wmp_move_gen_enumerate_nonplaythrough_subracks(
      wmp_move_gen, current, next_ml + 1, count + max_num_this, leave_map);
  for (int i = max_num_this - 1; i >= 0; i--) {
    bit_rack_take_letter(current, next_ml);
    leave_map_complement_take_letter(leave_map, next_ml, i);
  }
}

static inline void playthrough_subracks_init(WMPMoveGen *wmp_move_gen,
                                             int subrack_size) {
  const int offset = subracks_get_combination_offset(subrack_size);
  const int count = wmp_move_gen->count_by_size[subrack_size];
  for (int idx_for_size = 0; idx_for_size < count; idx_for_size++) {
    const SubrackInfo *nonplaythrough_subrack_info =
        &wmp_move_gen->nonplaythrough_infos[offset + idx_for_size];
    SubrackInfo *playthrough_subrack_info =
        &wmp_move_gen->playthrough_infos[offset + idx_for_size];
    playthrough_subrack_info->subrack = nonplaythrough_subrack_info->subrack;
    bit_rack_add_bit_rack(&playthrough_subrack_info->subrack,
                          &wmp_move_gen->playthrough_bit_rack);
  }
  wmp_move_gen->tiles_to_play = subrack_size;
}

static inline bool
wmp_move_gen_check_playthrough_full_rack_existence(WMPMoveGen *wmp_move_gen) {
  // Not necessarily a bingo (RACK_SIZE) as this could be in an endgame
  // position, but only one combination is possible, and the leave is empty.
  const int size = wmp_move_gen->full_rack_size;
  const int idx = subracks_get_combination_offset(size);
  SubrackInfo *playthrough_info = &wmp_move_gen->playthrough_infos[idx];
  playthrough_info->subrack = wmp_move_gen->player_bit_rack;
  bit_rack_add_bit_rack(&playthrough_info->subrack,
                        &wmp_move_gen->playthrough_bit_rack);
  const int word_size = size + wmp_move_gen->num_tiles_played_through;
  playthrough_info->wmp_entry = wmp_get_word_entry(
      wmp_move_gen->wmp, &playthrough_info->subrack, word_size);
  return playthrough_info->wmp_entry != NULL;
}

static inline void
wmp_move_gen_check_nonplaythroughs_of_size(WMPMoveGen *wmp_move_gen, int size,
                                           bool check_leaves) {
  const int leave_size = wmp_move_gen->full_rack_size - size;
  wmp_move_gen->nonplaythrough_best_leave_values[leave_size] =
      check_leaves ? EQUITY_MIN_VALUE : 0;
  const int offset = subracks_get_combination_offset(size);
  const int count = wmp_move_gen->count_by_size[size];
  for (int idx_for_size = 0; idx_for_size < count; idx_for_size++) {
    SubrackInfo *subrack_info =
        &wmp_move_gen->nonplaythrough_infos[offset + idx_for_size];
    subrack_info->wmp_entry =
        wmp_get_word_entry(wmp_move_gen->wmp, &subrack_info->subrack, size);
    if (subrack_info->wmp_entry == NULL) {
      continue;
    }
    wmp_move_gen->nonplaythrough_has_word_of_length[size] = true;
    if (!check_leaves) {
      continue;
    }
    if (subrack_info->leave_value >
        wmp_move_gen->nonplaythrough_best_leave_values[leave_size]) {
      wmp_move_gen->nonplaythrough_best_leave_values[leave_size] =
          subrack_info->leave_value;
    }
  }
}

static inline void wmp_move_gen_check_nonplaythrough_existence(
    WMPMoveGen *wmp_move_gen, bool check_leaves, LeaveMap *leave_map) {
  leave_map_set_current_index(leave_map,
                              (1 << wmp_move_gen->full_rack_size) - 1);
  memset(wmp_move_gen->count_by_size, 0, sizeof(wmp_move_gen->count_by_size));
  BitRack empty = bit_rack_create_empty();
  wmp_move_gen_enumerate_nonplaythrough_subracks(
      wmp_move_gen, &empty, BLANK_MACHINE_LETTER, 0, leave_map);
  for (int size = MINIMUM_WORD_LENGTH; size <= wmp_move_gen->full_rack_size;
       size++) {
    wmp_move_gen_check_nonplaythroughs_of_size(wmp_move_gen, size,
                                               check_leaves);
  }
}

static inline bool wmp_move_gen_nonplaythrough_word_of_length_exists(
    const WMPMoveGen *wmp_move_gen, int word_length) {
  return wmp_move_gen->nonplaythrough_has_word_of_length[word_length];
}

static inline const Equity *wmp_move_gen_get_nonplaythrough_best_leave_values(
    const WMPMoveGen *wmp_move_gen) {
  return wmp_move_gen->nonplaythrough_best_leave_values;
}

static inline void wmp_move_gen_add_playthrough_letter(WMPMoveGen *wmp_move_gen,
                                                       MachineLetter ml) {
  bit_rack_add_letter(&wmp_move_gen->playthrough_bit_rack, ml);
  wmp_move_gen->num_tiles_played_through++;
}

static inline void
wmp_move_gen_increment_playthrough_blocks(WMPMoveGen *wmp_move_gen) {
  assert(wmp_move_gen->playthrough_blocks < MAX_POSSIBLE_PLAYTHROUGH_BLOCKS);
  wmp_move_gen->playthrough_blocks++;
}

static inline void
wmp_move_gen_save_playthrough_state(WMPMoveGen *wmp_move_gen) {
  wmp_move_gen->playthrough_bit_rack_copy = wmp_move_gen->playthrough_bit_rack;
  wmp_move_gen->num_tiles_played_through_copy =
      wmp_move_gen->num_tiles_played_through;
  wmp_move_gen->playthrough_blocks_copy = wmp_move_gen->playthrough_blocks;
}

static inline void
wmp_move_gen_restore_playthrough_state(WMPMoveGen *wmp_move_gen) {
  wmp_move_gen->playthrough_bit_rack = wmp_move_gen->playthrough_bit_rack_copy;
  wmp_move_gen->num_tiles_played_through =
      wmp_move_gen->num_tiles_played_through_copy;
  wmp_move_gen->playthrough_blocks = wmp_move_gen->playthrough_blocks_copy;
}

static inline bool
wmp_move_gen_has_playthrough(const WMPMoveGen *wmp_move_gen) {
  return wmp_move_gen->num_tiles_played_through > 0;
}

static inline void wmp_move_gen_reset_playthrough(WMPMoveGen *wmp_move_gen) {
  wmp_move_gen->playthrough_bit_rack = bit_rack_create_empty();
  wmp_move_gen->num_tiles_played_through = 0;
  wmp_move_gen->playthrough_blocks = 0;
}

static inline int wmp_move_gen_anchor_index(int playthrough_blocks,
                                            int tiles_played) {
  return playthrough_blocks * (RACK_SIZE + 1) + tiles_played;
}

static inline Anchor *wmp_move_gen_get_anchor(WMPMoveGen *wmp_move_gen,
                                              int playthrough_blocks,
                                              int tiles_played) {

  return &wmp_move_gen->anchors[wmp_move_gen_anchor_index(playthrough_blocks,
                                                          tiles_played)];
}

static inline void wmp_move_gen_maybe_update_anchor(WMPMoveGen *wmp_move_gen,
                                                    int tiles_played,
                                                    Equity score,
                                                    Equity equity) {
  Anchor *anchor = wmp_move_gen_get_anchor(
      wmp_move_gen, wmp_move_gen->playthrough_blocks, tiles_played);
  anchor->tiles_to_play = tiles_played;
  anchor->playthrough_blocks = wmp_move_gen->playthrough_blocks;
  if (equity > anchor->highest_possible_equity) {
    anchor->highest_possible_equity = equity;
  }
  if (score > anchor->highest_possible_score) {
    anchor->highest_possible_score = score;
  }
}

static inline bool
wmp_move_gen_set_anchor_playthrough(WMPMoveGen *wmp_move_gen,
                                    const Anchor *anchor,
                                    Square row_cache[BOARD_DIM]) {
  wmp_move_gen_reset_playthrough(wmp_move_gen);
  memset(wmp_move_gen->playthrough_pattern, ALPHABET_EMPTY_SQUARE_MARKER,
         sizeof(wmp_move_gen->playthrough_pattern));
  int blocks = 0;
  bool in_playthrough = false;
  int pattern_start_col = anchor->col;
  if (square_get_letter(&row_cache[pattern_start_col]) !=
      ALPHABET_EMPTY_SQUARE_MARKER) {
    while (pattern_start_col > 0 &&
           square_get_letter(&row_cache[pattern_start_col - 1]) !=
               ALPHABET_EMPTY_SQUARE_MARKER) {
      pattern_start_col--;
    }
  }
  int col;
  int empty_squares_filled = 0;
  int last_playthrough_col = -1;
  for (col = pattern_start_col; col < BOARD_DIM; col++) {
    const uint8_t ml = square_get_letter(&row_cache[col]);
    if (ml == ALPHABET_EMPTY_SQUARE_MARKER) {
      //  Don't fill more empty squares if we're already at our target number.
      if (empty_squares_filled == anchor->tiles_to_play) {
        break;
      }
      empty_squares_filled++;
      // Only break if both empty squares and playthrough blocks are satisfied.
      // Even if we've filled all our empty squares, we may need to add the
      // abutting playthrough block (which would have nothing more played to its
      // right).
      if ((empty_squares_filled == anchor->tiles_to_play) &&
          (blocks == anchor->playthrough_blocks)) {
        break;
      }
      in_playthrough = false;
      continue;
    }
    if (!in_playthrough) {
      // Don't consume another playthrough block if we already have all the
      // ones this anchor should get.
      if (blocks == anchor->playthrough_blocks) {
        // Backtrack to the last square that wouldn't touch this playthrough.
        col -= 2;
        break;
      }
      in_playthrough = true;
      blocks++;
      assert(blocks <= anchor->playthrough_blocks);
    }
    wmp_move_gen->tiles_to_connect_playthrough = empty_squares_filled;
    last_playthrough_col = col;
    const uint8_t unblanked_ml = get_unblanked_machine_letter(ml);
    wmp_move_gen_add_playthrough_letter(wmp_move_gen, unblanked_ml);
    wmp_move_gen->playthrough_pattern[col] = unblanked_ml;
  }
  if ((col != last_playthrough_col) && (col + 1 < BOARD_DIM) &&
      (square_get_letter(&row_cache[col + 1]) !=
       ALPHABET_EMPTY_SQUARE_MARKER)) {
    // If we placed our final tile on an empty square but it turns out the
    // next column has playthrough tiles, backtrack one column to avoid touching
    // that playthrough block. We're already at our maximum number of
    // playthrough blocks.
    col--;
  }
  int rightmost_end_col = col;
  if (rightmost_end_col == BOARD_DIM) {
    rightmost_end_col = BOARD_DIM - 1;
  }
  assert(rightmost_end_col < BOARD_DIM);
  if (last_playthrough_col >= 0) {
    assert(rightmost_end_col >= last_playthrough_col);
  }

  if (last_playthrough_col != -1) {
    // do i need wmp_move_gen->tiles_to_play?
    const int num_placed_outside_playthrough =
        anchor->tiles_to_play - wmp_move_gen->tiles_to_connect_playthrough;
    assert(num_placed_outside_playthrough >= 0);
    int tiles_playable_to_right_of_playthrough =
        rightmost_end_col - last_playthrough_col;
    if (tiles_playable_to_right_of_playthrough >
        num_placed_outside_playthrough) {
      tiles_playable_to_right_of_playthrough = num_placed_outside_playthrough;
    }
    assert(num_placed_outside_playthrough >= 0);
    assert(tiles_playable_to_right_of_playthrough >= 0);
    wmp_move_gen->leftmost_start_col =
        pattern_start_col - num_placed_outside_playthrough;
    const int tiles_needed_to_left =
        num_placed_outside_playthrough - tiles_playable_to_right_of_playthrough;
    assert(tiles_needed_to_left >= 0);
    wmp_move_gen->rightmost_start_col =
        pattern_start_col - tiles_needed_to_left;
  } else {
    wmp_move_gen->leftmost_start_col = anchor->col - anchor->tiles_to_play + 1;
    wmp_move_gen->rightmost_start_col =
        rightmost_end_col - anchor->tiles_to_play + 1;
    if (wmp_move_gen->rightmost_start_col > anchor->col) {
      wmp_move_gen->rightmost_start_col = anchor->col;
    }
  }
  if (wmp_move_gen->leftmost_start_col < 0) {
    wmp_move_gen->leftmost_start_col = 0;
  }
  if ((anchor->last_anchor_col != BOARD_DIM) &&
      (wmp_move_gen->leftmost_start_col <= anchor->last_anchor_col)) {
    wmp_move_gen->leftmost_start_col = anchor->last_anchor_col + 1;
  }

  wmp_move_gen->word_length =
      wmp_move_gen->num_tiles_played_through + anchor->tiles_to_play;
  assert(wmp_move_gen->word_length > 0);
  if (wmp_move_gen->word_length > BOARD_DIM) {
    return false;
  }
  playthrough_subracks_init(wmp_move_gen, anchor->tiles_to_play);
  return true;
}

static inline int
wmp_move_gen_get_num_subrack_combinations(const WMPMoveGen *wmp_move_gen) {
  return wmp_move_gen->count_by_size[wmp_move_gen->tiles_to_play];
}

static inline const BitRack *
wmp_move_gen_get_nonplaythrough_subrack(const WMPMoveGen *wmp_move_gen,
                                        int idx_for_size) {
  const int offset =
      subracks_get_combination_offset(wmp_move_gen->tiles_to_play);
  return &wmp_move_gen->nonplaythrough_infos[offset + idx_for_size].subrack;
}

static inline bool wmp_move_gen_get_subrack_words(WMPMoveGen *wmp_move_gen,
                                                  int idx_for_size) {
  const int offset =
      subracks_get_combination_offset(wmp_move_gen->tiles_to_play);
  const int subrack_idx = offset + idx_for_size;
  const bool is_playthrough = wmp_move_gen->num_tiles_played_through > 0;
  SubrackInfo *subrack_info =
      is_playthrough ? &wmp_move_gen->playthrough_infos[subrack_idx]
                     : &wmp_move_gen->nonplaythrough_infos[subrack_idx];
  // Nonplaythrough subracks' wmp entries were already looked up during shadow.
  if (is_playthrough) {
    subrack_info->wmp_entry = wmp_get_word_entry(
        wmp_move_gen->wmp, &subrack_info->subrack, wmp_move_gen->word_length);
  }

  if (subrack_info->wmp_entry == NULL) {
    return false;
  }
  const int result_bytes = wmp_entry_write_words_to_buffer(
      subrack_info->wmp_entry, wmp_move_gen->wmp, &subrack_info->subrack,
      wmp_move_gen->word_length, wmp_move_gen->buffer);
  assert(result_bytes > 0);
  assert(result_bytes % wmp_move_gen->word_length == 0);
  wmp_move_gen->num_words = result_bytes / wmp_move_gen->word_length;
  return true;
}

static inline void wmp_move_gen_add_anchors(WMPMoveGen *wmp_move_gen, int row,
                                            int col, int last_anchor_col,
                                            int dir, AnchorHeap *anchor_heap) {
  for (int i = 0; i < MAX_WMP_MOVE_GEN_ANCHORS; i++) {
    const Anchor *anchor = &wmp_move_gen->anchors[i];
    if (anchor->tiles_to_play > 0) {
      const int word_length =
          wmp_move_gen->num_tiles_played_through + anchor->tiles_to_play;
      assert(word_length >= MINIMUM_WORD_LENGTH);
      assert(word_length <= wmp_move_gen->wmp->board_dim);
      anchor_heap_add_unheaped_wmp_anchor(
          anchor_heap, row, col, last_anchor_col, dir,
          anchor->highest_possible_equity, anchor->highest_possible_score,
          anchor->tiles_to_play, anchor->playthrough_blocks);
    }
  }
}

static inline const uint8_t *
wmp_move_gen_get_word(const WMPMoveGen *wmp_move_gen, int word_idx) {
  return wmp_move_gen->buffer + word_idx * wmp_move_gen->word_length;
}

static inline Equity wmp_move_gen_get_leave_value(WMPMoveGen *wmp_move_gen,
                                                  int subrack_idx) {
  const int offset =
      subracks_get_combination_offset(wmp_move_gen->tiles_to_play);
  const SubrackInfo *subrack_info =
      &wmp_move_gen->nonplaythrough_infos[offset + subrack_idx];
  wmp_move_gen->leave_value = subrack_info->leave_value;
  return wmp_move_gen->leave_value;
}
#endif