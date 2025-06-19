#ifndef WMP_MOVEGEN_H
#define WMP_MOVEGEN_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../def/kwg_defs.h"
#include "../def/move_defs.h"

#include "../ent/anchor.h"
#include "../ent/bit_rack.h"
#include "../ent/board.h"
#include "../ent/leave_map.h"
#include "../ent/wmp.h"

#define MAX_POSSIBLE_PLAYTHROUGH_BLOCKS ((BOARD_DIM / 2) + 1)
#define MAX_WMP_MOVE_GEN_ANCHORS                                               \
  ((RACK_SIZE + 1) * MAX_POSSIBLE_PLAYTHROUGH_BLOCKS)

typedef struct SubrackInfo {
  BitRack subrack;
  const WMPEntry *wmp_entry;
  Equity leave_value;
} SubrackInfo;

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
  int playthrough_blocks;
  int playthrough_blocks_copy;

  Anchor anchors[MAX_WMP_MOVE_GEN_ANCHORS];
} WMPMoveGen;

static inline void reset_anchors(WMPMoveGen *wmp_move_gen) {
  Anchor initial_anchor = {
      .playthrough = bit_rack_create_empty(),
      .highest_possible_equity = EQUITY_INITIAL_VALUE,
      .highest_possible_score = 0,
      .row = 0,
      .col = 0,
      .last_anchor_col = 0,
      .dir = 0,
  };
  for (int i = 0; i < MAX_WMP_MOVE_GEN_ANCHORS; i++) {
    memcpy(&wmp_move_gen->anchors[i], &initial_anchor, sizeof(Anchor));
  }
}

static inline void wmp_move_gen_init(WMPMoveGen *wmp_move_gen,
                                     const LetterDistribution *ld,
                                     Rack *player_rack, const WMP *wmp) {
  wmp_move_gen->wmp = wmp;
  if (wmp == NULL) {
    return;
  }
  wmp_move_gen->playthrough_blocks = 0;
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
                                                       uint8_t ml) {
  bit_rack_add_letter(&wmp_move_gen->playthrough_bit_rack, ml);
  wmp_move_gen->num_tiles_played_through++;
}

static inline void
wmp_move_gen_increment_playthrough_blocks(WMPMoveGen *wmp_move_gen) {
  assert(wmp_move_gen->playthrough_blocks < MAX_POSSIBLE_PLAYTHROUGH_BLOCKS);
  // printf("incrementing playthrough blocks from %d to %d\n",
  //        wmp_move_gen->playthrough_blocks,
  //        wmp_move_gen->playthrough_blocks + 1);
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

static inline bool wmp_move_gen_has_playthrough(WMPMoveGen *wmp_move_gen) {
  return wmp_move_gen->num_tiles_played_through > 0;
}

static inline void wmp_move_gen_reset_playthrough(WMPMoveGen *wmp_move_gen) {
  wmp_move_gen->playthrough_bit_rack = bit_rack_create_empty();
  wmp_move_gen->num_tiles_played_through = 0;
  wmp_move_gen->playthrough_blocks = 0;
}

static inline void wmp_move_gen_add_anchors(WMPMoveGen *wmp_move_gen, int row,
                                            int col, int last_anchor_col,
                                            int dir, AnchorHeap *anchor_heap) {
  for (int i = 0; i < MAX_WMP_MOVE_GEN_ANCHORS; i++) {
    Anchor *anchor = &wmp_move_gen->anchors[i];
    if (anchor->highest_possible_equity > EQUITY_INITIAL_VALUE) {
      anchor_heap_add_unheaped_wmp_anchor(
          anchor_heap, row, col, last_anchor_col, dir,
          anchor->highest_possible_equity, &anchor->playthrough,
          anchor->highest_possible_score);
    }
  }
}

static inline void
wmp_move_gen_print_playthrough(const WMPMoveGen *wmp_move_gen) {
  printf("wmp_move_gen_print_playthrough playthrough blocks: %d, tiles: ",
         wmp_move_gen->playthrough_blocks);
  for (int i = 0; i < 32; i++) {
    for (int j = 0;
         j < bit_rack_get_letter(&wmp_move_gen->playthrough_bit_rack, i); j++) {
      char c = '?';
      if (i > 0) {
        c = 'A' + i - 1;
      }
      printf("%c", c);
    }
  }
  printf("\n");
}

static inline int wmp_move_gen_anchor_index(int playthrough_blocks,
                                            int tiles_played) {
  printf("playthrough_blocks: %d, tiles_played: %d\n", playthrough_blocks,
         tiles_played);                                              
  return playthrough_blocks * (RACK_SIZE + 1) + (tiles_played - 1);
}

static inline Anchor *wmp_move_gen_get_anchor(WMPMoveGen *wmp_move_gen,
                                              int playthrough_blocks,
                                              int tiles_played) {
  int index = wmp_move_gen_anchor_index(playthrough_blocks, tiles_played);
  printf("wmp_move_gen_get_anchor: index: %d\n", index);
  assert(index >= 0 && index < MAX_WMP_MOVE_GEN_ANCHORS);
  return &wmp_move_gen->anchors[index];
}

static inline void wmp_move_gen_maybe_update_anchor(WMPMoveGen *wmp_move_gen,
                                                    int tiles_played,
                                                    Equity score,
                                                    Equity equity) {
  Anchor *anchor = wmp_move_gen_get_anchor(
      wmp_move_gen, wmp_move_gen->playthrough_blocks, tiles_played);
  anchor->highest_possible_score = score;
  anchor->highest_possible_equity = equity;
  anchor->playthrough = wmp_move_gen->playthrough_bit_rack;
}
#endif
