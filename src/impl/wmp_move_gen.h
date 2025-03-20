#ifndef WMP_MOVEGEN_H
#define WMP_MOVEGEN_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "../def/kwg_defs.h"
#include "../def/move_defs.h"

#include "../ent/anchor.h"
#include "../ent/bit_rack.h"
#include "../ent/board.h"
#include "../ent/leave_map.h"
#include "../ent/static_eval.h"
#include "../ent/wmp.h"

#define WORD_SPOT_HEAP_CAPACITY (BOARD_DIM * BOARD_DIM * RACK_SIZE)

typedef struct SubrackInfo {
  BitRack subrack;
  const WMPEntry *wmp_entry;
  Equity leave_value;
} SubrackInfo;

typedef struct WordSpot {
  Equity best_possible_equity;
  Equity best_possible_score;
  Equity preendgame_adjustment;
  Equity endgame_adjustment;
  uint8_t row;
  uint8_t col;
  uint8_t dir;
  uint8_t num_tiles;
} WordSpot;

typedef struct WordSpotHeap {
  int count;
  WordSpot spots[WORD_SPOT_HEAP_CAPACITY];
} WordSpotHeap;

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
  WordSpotHeap word_spot_heap;
  WordSpot word_spot;
  BoardSpot board_spot;

  int subrack_idx;
  int num_words;
  int word_idx;
  int buffer_pos;
  int num_lookups;
  int lookups_by_size[RACK_SIZE + 1];
  Equity score;
  Equity leave_value;
  uint8_t buffer[WMP_RESULT_BUFFER_SIZE];
} WMPMoveGen;

static inline void wmp_move_gen_init(WMPMoveGen *wmp_move_gen,
                                     const LetterDistribution *ld,
                                     Rack *player_rack, const WMP *wmp) {
  wmp_move_gen->num_lookups = 0;
  memset(wmp_move_gen->lookups_by_size, 0,
         sizeof(wmp_move_gen->lookups_by_size));
  wmp_move_gen->wmp = wmp;
  if (wmp == NULL) {
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
  wmp_move_gen->num_lookups++;
  wmp_move_gen->lookups_by_size[size]++;
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
    wmp_move_gen->num_lookups++;
    wmp_move_gen->lookups_by_size[size]++;
    subrack_info->wmp_entry =
        wmp_get_word_entry(wmp_move_gen->wmp, &subrack_info->subrack, size);
    if (subrack_info->wmp_entry == NULL) {
      continue;
    }
    // printf("word for ");
    // for (int i = 0; i <= 26; i++) {
    //   for (int j = 0; j < bit_rack_get_letter(&subrack_info->subrack, i);
    //   j++) {
    //     printf("%c", 'A' + i - 1);
    //   }
    // }
    // printf(" has leave value %d\n", subrack_info->leave_value);
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
wmp_move_gen_save_playthrough_state(WMPMoveGen *wmp_move_gen) {
  wmp_move_gen->playthrough_bit_rack_copy = wmp_move_gen->playthrough_bit_rack;
  wmp_move_gen->num_tiles_played_through_copy =
      wmp_move_gen->num_tiles_played_through;
}

static inline void
wmp_move_gen_restore_playthrough_state(WMPMoveGen *wmp_move_gen) {
  wmp_move_gen->playthrough_bit_rack = wmp_move_gen->playthrough_bit_rack_copy;
  wmp_move_gen->num_tiles_played_through =
      wmp_move_gen->num_tiles_played_through_copy;
}

static inline bool wmp_move_gen_has_playthrough(WMPMoveGen *wmp_move_gen) {
  return wmp_move_gen->num_tiles_played_through > 0;
}

static inline void wmp_move_gen_reset_playthrough(WMPMoveGen *wmp_move_gen) {
  wmp_move_gen->playthrough_bit_rack = bit_rack_create_empty();
  wmp_move_gen->num_tiles_played_through = 0;
}

static inline void word_spot_heap_reset(WordSpotHeap *word_spot_heap) {
  word_spot_heap->count = 0;
}

static inline void word_spot_heap_make_spot(
    WMPMoveGen *wmg, const BoardSpot *board_spot, int row, int col, int dir,
    int tiles, move_sort_t move_sort_type, const Equity *descending_tile_scores,
    const Equity *best_leaves, Equity preendgame_adjustment,
    Equity endgame_adjustment) {
  // printf("row: %d, col: %d, dir: %d, tiles: %d\n", row, col, dir, tiles);
  WordSpotHeap *heap = &wmg->word_spot_heap;
  const bool has_playthrough = board_spot->word_length > tiles;
  if (!has_playthrough && !wmg->nonplaythrough_has_word_of_length[tiles]) {
    // printf("can't make word of length %d\n", tiles);
    return;
  }
  WordSpot *spot = &heap->spots[heap->count++];
  spot->row = row;
  spot->col = col;
  spot->dir = dir;
  spot->num_tiles = tiles;
  spot->best_possible_score = 0;
  spot->preendgame_adjustment = preendgame_adjustment;
  spot->endgame_adjustment = endgame_adjustment;
  for (int tile_idx = 0; tile_idx < WORD_ALIGNING_RACK_SIZE; tile_idx++) {
    const Equity tile_score =
        descending_tile_scores[tile_idx] *
        board_spot->descending_effective_multipliers[tile_idx];
    spot->best_possible_score += tile_score;
  }
  spot->best_possible_score += board_spot->additional_score;
  const int leave_size = wmg->full_rack_size - tiles;
  spot->best_possible_equity =
      spot->best_possible_score + preendgame_adjustment + endgame_adjustment;
  if (leave_size == 0 || move_sort_type == MOVE_SORT_SCORE) {
    // printf("spot->best_possible_equity: %d\n", spot->best_possible_equity);
    return;
  }
  if (has_playthrough) {
    spot->best_possible_equity += best_leaves[leave_size];
  } else {
    // printf("leave_size: %d\n", leave_size);
    // printf("wmg->nonplaythrough_best_leave_values[leave_size]: %d\n",
    //        wmg->nonplaythrough_best_leave_values[leave_size]);
    spot->best_possible_equity +=
        wmg->nonplaythrough_best_leave_values[leave_size];
  }
  // printf("spot->best_possible_equity: %d\n", spot->best_possible_equity);
}

static inline bool word_spot_is_better(const WordSpot *a, const WordSpot *b) {
  return a->best_possible_equity > b->best_possible_equity;
}

static inline void swap_word_spots(WordSpot *a, WordSpot *b) {
  WordSpot temp = *a;
  *a = *b;
  *b = temp;
}

static inline void word_spot_heapify_down(WordSpotHeap *heap, int parent_node) {
  int left = parent_node * 2 + 1;
  int right = parent_node * 2 + 2;
  int min;

  if (left >= heap->count || left < 0)
    left = -1;
  if (right >= heap->count || right < 0)
    right = -1;

  if (left != -1 &&
      word_spot_is_better(&heap->spots[left], &heap->spots[parent_node]))
    min = left;
  else
    min = parent_node;
  if (right != -1 &&
      word_spot_is_better(&heap->spots[right], &heap->spots[min]))
    min = right;

  if (min != parent_node) {
    swap_word_spots(&heap->spots[min], &heap->spots[parent_node]);
    word_spot_heapify_down(heap, min);
  }
}

static inline void word_spot_heapify_all(WordSpotHeap *heap) {
  for (int node_idx = heap->count / 2; node_idx >= 0; node_idx--) {
    word_spot_heapify_down(heap, node_idx);
  }
}

static inline void wmp_move_gen_build_word_spot_heap(
    WMPMoveGen *wmp_move_gen, const Board *board, move_sort_t move_sort_type,
    const Equity *descending_tile_scores, const Equity *best_leaves,
    int number_of_tiles_in_bag, Equity opp_rack_score, int ci) {
  // printf("wmp_move_gen_build_word_spot_heap\n");
  WordSpotHeap *heap = &wmp_move_gen->word_spot_heap;
  word_spot_heap_reset(heap);
  for (int dir = 0; dir < 2; dir++) {
    for (int row = 0; row < BOARD_DIM; row++) {
      for (int col = 0; col < BOARD_DIM; col++) {
        for (int tiles = 1; tiles <= wmp_move_gen->full_rack_size; tiles++) {
          // printf("row: %d, col: %d, dir: %d, tiles: %d\n", row, col, dir,
          //  tiles);
          const BoardSpot *spot =
              board_get_readonly_spot(board, row, col, dir, tiles, ci);
          if (!spot->is_usable) {
            // printf("unusable spot\n");
            continue;
          }
          const int bag_plus_rack_size =
              number_of_tiles_in_bag - tiles + RACK_SIZE;
          const bool is_outplay = (number_of_tiles_in_bag == 0) &&
                                  (tiles == wmp_move_gen->full_rack_size);
          Equity lowest_possible_rack_score = 0;
          for (int i = tiles; i < wmp_move_gen->full_rack_size; i++) {
            lowest_possible_rack_score += descending_tile_scores[i];
          }
          Equity preendgame_adjustment = 0;
          Equity endgame_adjustment = 0;
          if (move_sort_type == MOVE_SORT_EQUITY) {
            if (number_of_tiles_in_bag == 0) {
              endgame_adjustment =
                  is_outplay ? endgame_outplay_adjustment(opp_rack_score)
                             : endgame_nonoutplay_adjustment(
                                   lowest_possible_rack_score);
            } else if (bag_plus_rack_size < PEG_ADJUST_VALUES_LENGTH) {
              preendgame_adjustment = peg_adjust_values[bag_plus_rack_size];
            }
          }
          word_spot_heap_make_spot(wmp_move_gen, spot, row, col, dir, tiles,
                                   move_sort_type, descending_tile_scores,
                                   best_leaves, preendgame_adjustment,
                                   endgame_adjustment);
        }
      }
    }
  }
  // printf("heap->count: %d\n", heap->count);
  word_spot_heapify_all(heap);
}

// Removes max spot, returns a copy.
static inline WordSpot word_spot_heap_extract_max(WordSpotHeap *heap) {
  const WordSpot max_spot = heap->spots[0];
  heap->spots[0] = heap->spots[heap->count - 1];
  heap->count--;
  word_spot_heapify_down(heap, 0);
  return max_spot;
}

static inline Equity wmp_move_gen_look_up_leave_value(const WMPMoveGen *wmg) {
  return wmg->nonplaythrough_infos[wmg->subrack_idx].leave_value;
}

static inline int
wmp_move_gen_get_num_subrack_combinations(const WMPMoveGen *wmp_move_gen) {
  return wmp_move_gen->count_by_size[wmp_move_gen->word_spot.num_tiles];
}

static inline bool wmp_move_gen_get_subrack_words(WMPMoveGen *wmg) {
  const bool is_playthrough =
      wmg->board_spot.word_length > wmg->word_spot.num_tiles;
  // printf("is_playthrough: %d\n", is_playthrough);
  const SubrackInfo *nonplaythrough_info =
      &wmg->nonplaythrough_infos[wmg->subrack_idx];
  const SubrackInfo *playthrough_info =
      &wmg->playthrough_infos[wmg->subrack_idx];
  const SubrackInfo *subrack_info =
      is_playthrough ? playthrough_info : nonplaythrough_info;
  const WMPEntry *entry = NULL;
  BitRack bit_rack = bit_rack_create_empty();
  // printf("wmg->word_spot.num_tiles: %d\n", wmg->word_spot.num_tiles);
  //   if (!is_playthrough || wmp_move_gen->word_spot.num_tiles == RACK_SIZE) {
  if (!is_playthrough) {
    entry = subrack_info->wmp_entry;
    // printf("entry: %p\n", entry);
    // printf("subrack_info->subrack: ");
    // for (int i = 0; i <= 26; i++) {
    //   int num_this = bit_rack_get_letter(&subrack_info->subrack, i);
    //   for (int j = 0; j < num_this; j++) {
    //     printf("%c", 'A' + i - 1);
    //   }
    // }
    // printf("\n");
    bit_rack = subrack_info->subrack;
  } else {
    bit_rack = nonplaythrough_info->subrack;
    // printf("nonplaythrough_info->subrack: ");
    // for (int i = 0; i <= 26; i++) {
    //   int num_this = bit_rack_get_letter(&bit_rack, i);
    //   for (int j = 0; j < num_this; j++) {
    //     printf("%c", 'A' + i - 1);
    //   }
    // }
    // printf("\n");
    bit_rack_add_bit_rack(&bit_rack, &wmg->board_spot.playthrough_bit_rack);
    // printf("after adding playthrough_bit_rack: ");
    // for (int i = 0; i <= 26; i++) {
    //   int num_this = bit_rack_get_letter(&bit_rack, i);
    //   for (int j = 0; j < num_this; j++) {
    //     printf("%c", 'A' + i - 1);
    //   }
    // }
    // printf("\n");
    wmg->num_lookups++;
    wmg->lookups_by_size[wmg->word_spot.num_tiles]++;
    entry =
        wmp_get_word_entry(wmg->wmp, &bit_rack, wmg->board_spot.word_length);
  }
  // printf("getting words with ");
  // for (int i = 0; i <= 26; i++) {
  //   int num_this = bit_rack_get_letter(&bit_rack, i);
  //   for (int j = 0; j < num_this; j++) {
  //     printf("%c", 'A' + i - 1);
  //   }
  // }
  // printf("\n");
  if (entry == NULL) {
    // printf("entry is NULL, no words");
    return false;
  }
  // printf("wmg->board_spot.word_length: %d\n", wmg->board_spot.word_length);
  const int result_bytes = wmp_entry_write_words_to_buffer(
      entry, wmg->wmp, &bit_rack, wmg->board_spot.word_length, wmg->buffer);
  // printf("result_bytes: %d\n", result_bytes);
  assert(result_bytes > 0);
  assert(result_bytes % wmg->board_spot.word_length == 0);
  wmg->num_words = result_bytes / wmg->board_spot.word_length;
  return true;
}

static inline uint8_t wmp_move_gen_get_word_letter(WMPMoveGen *wmp_move_gen) {
  return wmp_move_gen->buffer[wmp_move_gen->buffer_pos++];
}

static inline const BitRack *
wmp_move_gen_get_nonplaythrough_tiles(const WMPMoveGen *wmp_move_gen) {
  return &wmp_move_gen->nonplaythrough_infos[wmp_move_gen->subrack_idx].subrack;
}
#endif