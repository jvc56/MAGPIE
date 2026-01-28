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

  SubrackInfo nonplaythrough_infos[1 << RACK_SIZE];
  SubrackInfo playthrough_infos[1 << RACK_SIZE];
  Equity nonplaythrough_best_leave_values[RACK_SIZE + 1];
  bool nonplaythrough_has_word_of_length[RACK_SIZE + 1];
  uint8_t count_by_size[RACK_SIZE + 1];

  Anchor anchors[MAX_WMP_MOVE_GEN_ANCHORS];
  int playthrough_blocks;
  int playthrough_blocks_copy;

  MachineLetter buffer[WMP_RESULT_BUFFER_SIZE];
  int tiles_to_play;
  int word_length;
  int num_words;
  Equity leave_value;
} WMPMoveGen;

static inline void wmp_move_gen_reset_anchors(WMPMoveGen *wmp_move_gen) {
  for (int i = 0; i < MAX_WMP_MOVE_GEN_ANCHORS; i++) {
    wmp_move_gen->anchors[i].highest_possible_equity = EQUITY_MIN_VALUE;
    wmp_move_gen->anchors[i].highest_possible_score = EQUITY_MIN_VALUE;
    wmp_move_gen->anchors[i].rightmost_start_col = 0;
    wmp_move_gen->anchors[i].leftmost_start_col = BOARD_DIM - 1;
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

static inline void
wmp_move_gen_playthrough_subracks_init(WMPMoveGen *wmp_move_gen,
                                       const Anchor *anchor) {
  const int subrack_size = anchor->tiles_to_play;
  wmp_move_gen->word_length = anchor->word_length;
  wmp_move_gen->num_tiles_played_through = anchor->word_length - subrack_size;
  wmp_move_gen->tiles_to_play = subrack_size;
  if (wmp_move_gen->num_tiles_played_through == 0) {
    // We can use nonplaythrough subracks
    return;
  }
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

// Optimized subrack enumeration that skips leave_map tracking entirely.
// Use this for record-all mode where leave values aren't needed.
static inline void
wmp_move_gen_enumerate_subracks_no_leaves(WMPMoveGen *wmp_move_gen,
                                          BitRack *current, int next_ml,
                                          int count) {
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
    subrack_info->leave_value = 0;
    wmp_move_gen->count_by_size[count]++;
    return;
  }
  for (int i = 0; i < max_num_this; i++) {
    wmp_move_gen_enumerate_subracks_no_leaves(wmp_move_gen, current,
                                              next_ml + 1, count + i);
    bit_rack_add_letter(current, next_ml);
  }
  wmp_move_gen_enumerate_subracks_no_leaves(wmp_move_gen, current, next_ml + 1,
                                            count + max_num_this);
  for (int i = max_num_this - 1; i >= 0; i--) {
    bit_rack_take_letter(current, next_ml);
  }
}

// Optimized check for record-all mode - no leave_map needed
static inline void wmp_move_gen_check_nonplaythrough_existence_no_leaves(
    WMPMoveGen *wmp_move_gen) {
  memset(wmp_move_gen->count_by_size, 0, sizeof(wmp_move_gen->count_by_size));
  BitRack empty = bit_rack_create_empty();
  wmp_move_gen_enumerate_subracks_no_leaves(wmp_move_gen, &empty,
                                            BLANK_MACHINE_LETTER, 0);
  for (int size = MINIMUM_WORD_LENGTH; size <= wmp_move_gen->full_rack_size;
       size++) {
    wmp_move_gen_check_nonplaythroughs_of_size(wmp_move_gen, size, false);
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
                                                    int word_length,
                                                    int start_col, Equity score,
                                                    Equity equity) {
  assert(start_col >= 0 && start_col < BOARD_DIM);
  Anchor *anchor = wmp_move_gen_get_anchor(
      wmp_move_gen, wmp_move_gen->playthrough_blocks, tiles_played);
  anchor->tiles_to_play = tiles_played;
  anchor->playthrough_blocks = wmp_move_gen->playthrough_blocks;
  anchor->word_length = word_length;
  if (start_col < anchor->leftmost_start_col) {
    anchor->leftmost_start_col = start_col;
  }
  if (start_col > anchor->rightmost_start_col) {
    anchor->rightmost_start_col = start_col;
  }
  if (equity > anchor->highest_possible_equity) {
    anchor->highest_possible_equity = equity;
  }
  if (score > anchor->highest_possible_score) {
    anchor->highest_possible_score = score;
  }
}

static inline void
wmp_move_gen_set_playthrough_bit_rack(WMPMoveGen *wmp_move_gen,
                                      const Anchor *anchor,
                                      Square row_cache[BOARD_DIM]) {
  wmp_move_gen_reset_playthrough(wmp_move_gen);
  if (anchor->playthrough_blocks == 0) {
    return;
  }
  bool in_block = false;
  int blocks_found = 0;
  for (int col = anchor->rightmost_start_col; col < BOARD_DIM; col++) {
    const MachineLetter ml = row_cache[col].letter;
    if (ml == ALPHABET_EMPTY_SQUARE_MARKER) {
      if (in_block) {
        if (blocks_found == anchor->playthrough_blocks) {
          break;
        }
        in_block = false;
      }
      continue;
    }
    const MachineLetter unblanked_ml = get_unblanked_machine_letter(ml);
    wmp_move_gen_add_playthrough_letter(wmp_move_gen, unblanked_ml);
    if (!in_block) {
      in_block = true;
      blocks_found++;
    }
  }
  assert(blocks_found == anchor->playthrough_blocks);
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
  const bool is_playthrough =
      wmp_move_gen->word_length > wmp_move_gen->tiles_to_play;
  SubrackInfo *subrack_info =
      is_playthrough ? &wmp_move_gen->playthrough_infos[subrack_idx]
                     : &wmp_move_gen->nonplaythrough_infos[subrack_idx];
  // Nonplaythrough subracks' wmp entries were already looked up during
  // shadow.
  if (is_playthrough) {
    subrack_info->wmp_entry = wmp_get_word_entry(
        wmp_move_gen->wmp, &subrack_info->subrack, wmp_move_gen->word_length);
  }

  if (subrack_info->wmp_entry == NULL) {
    return false;
  }

  assert(wmp_move_gen->word_length >= MINIMUM_WORD_LENGTH);
  assert(wmp_move_gen->word_length <= BOARD_DIM);

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
                                            int dir,
                                            Equity inference_cutoff_equity,
                                            AnchorHeap *anchor_heap) {
  for (int i = 0; i < MAX_WMP_MOVE_GEN_ANCHORS; i++) {
    const Anchor *anchor = &wmp_move_gen->anchors[i];
    if (anchor->tiles_to_play == 0) {
      continue;
    }

    // Skip subanchors whose highest possible equity is below the cutoff
    // threshold. This is safe when cutoff_equity is fixed (as in inference
    // with stop_on_threshold).
    if (inference_cutoff_equity != EQUITY_MAX_VALUE &&
        anchor->highest_possible_equity < inference_cutoff_equity) {
      continue;
    }

    assert(anchor->word_length >= MINIMUM_WORD_LENGTH);
    assert(anchor->word_length <= wmp_move_gen->wmp->board_dim);
    anchor_heap_add_unheaped_wmp_anchor(
        anchor_heap, row, col, last_anchor_col, anchor->leftmost_start_col,
        anchor->rightmost_start_col, dir, anchor->highest_possible_equity,
        anchor->highest_possible_score, anchor->tiles_to_play,
        anchor->playthrough_blocks, anchor->word_length);
  }
}

static inline const MachineLetter *
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