

#ifndef KWG_DEAD_ENDS_H
#define KWG_DEAD_ENDS_H

#include "../def/cross_set_defs.h"

#include "kwg.h"
#include "rack.h"

#define MAX_DEAD_ENDS 1000000

typedef struct KWGDeadEnds {
  // Internal vars
  const KWG *kwg;
  Rack rack;
  int tiles_played;
  int dead_end_level;
  uint64_t dead_end_level_offsets[RACK_SIZE + 1];
  int base_offset;
  // Results
  int max_tiles_played;
  int number_of_dead_ends;
  uint64_t dead_ends[MAX_DEAD_ENDS];
} KWGDeadEnds;

static inline uint64_t kwgde_get_next_sequence(KWGDeadEnds *kwgde,
                                               uint64_t current_tile_sequence,
                                               uint8_t letter) {
  int letter_val = letter;
  if (letter == SEPARATION_MACHINE_LETTER) {
    letter_val = kwgde->base_offset - 1;
  }
  return current_tile_sequence +
         kwgde->dead_end_level_offsets[kwgde->dead_end_level - 1] * letter_val;
}

static inline uint64_t kwgde_get_dead_end(KWGDeadEnds *kwgde,
                                          int tile_sequence_index) {
  return kwgde->dead_ends[tile_sequence_index];
}

static inline void set_dead_end(KWGDeadEnds *kwgde, uint64_t tile_sequence) {
  kwgde->dead_ends[kwgde->number_of_dead_ends++] = tile_sequence;
}

static inline int kwgde_get_max_tiles_played(KWGDeadEnds *kwgde) {
  return kwgde->max_tiles_played;
}

static inline int kwgde_get_number_of_dead_ends(KWGDeadEnds *kwgde) {
  return kwgde->number_of_dead_ends;
}

static inline bool kwgde_go_on(KWGDeadEnds *kwgde, uint8_t current_letter,
                               uint32_t new_node_index, bool accepts,
                               bool already_switched_dir,
                               uint64_t current_tile_sequence);

static inline bool kwgde_recursive_gen(KWGDeadEnds *kwgde, uint32_t node_index,
                                       bool already_switched_dir,
                                       uint64_t current_tile_sequence) {
  bool acceptable_word_found = false;
  if (!rack_is_empty(&kwgde->rack)) {
    for (uint32_t i = node_index;; i++) {
      const uint32_t node = kwg_node(kwgde->kwg, i);
      const uint8_t ml = kwg_node_tile(node);
      int number_of_ml = rack_get_letter(&kwgde->rack, ml);
      if (ml != 0 &&
          (number_of_ml != 0 ||
           rack_get_letter(&kwgde->rack, BLANK_MACHINE_LETTER) != 0)) {
        const uint32_t next_node_index =
            kwg_node_arc_index_prefetch(node, kwgde->kwg);
        bool accepts = kwg_node_accepts(node);
        acceptable_word_found |= accepts;
        // FIXME: explain why we use else if instead of another if for blanks
        if (number_of_ml > 0) {
          rack_take_letter(&kwgde->rack, ml);
          kwgde->tiles_played++;
          kwgde->dead_end_level++;
          acceptable_word_found |=
              kwgde_go_on(kwgde, ml, next_node_index, accepts,
                          already_switched_dir, current_tile_sequence);
          kwgde->tiles_played--;
          kwgde->dead_end_level--;
          rack_add_letter(&kwgde->rack, ml);
        } else if (rack_get_letter(&kwgde->rack, BLANK_MACHINE_LETTER) > 0) {
          rack_take_letter(&kwgde->rack, BLANK_MACHINE_LETTER);
          kwgde->tiles_played++;
          kwgde->dead_end_level++;
          acceptable_word_found |= kwgde_go_on(
              kwgde, get_blanked_machine_letter(ml), next_node_index, accepts,
              already_switched_dir, current_tile_sequence);
          kwgde->tiles_played--;
          kwgde->dead_end_level--;
          rack_add_letter(&kwgde->rack, BLANK_MACHINE_LETTER);
        }
      }
      if (kwg_node_is_end(node)) {
        break;
      }
    }
  }
  return acceptable_word_found;
}

static inline bool kwgde_go_on(KWGDeadEnds *kwgde, uint8_t current_letter,
                               uint32_t new_node_index, bool accepts,
                               bool already_switched_dir,
                               uint64_t previous_tile_sequence) {
  bool acceptable_word_found = accepts;

  if (accepts && kwgde->tiles_played > kwgde->max_tiles_played) {
    kwgde->max_tiles_played = kwgde->tiles_played;
  }

  uint64_t current_tile_sequence =
      kwgde_get_next_sequence(kwgde, previous_tile_sequence, current_letter);

  int current_number_of_dead_ends = kwgde->number_of_dead_ends;

  if (!already_switched_dir) {
    if (new_node_index == 0) {
      return acceptable_word_found;
    }
    acceptable_word_found |= kwgde_recursive_gen(
        kwgde, new_node_index, already_switched_dir, current_tile_sequence);
    uint32_t separation_node_index = kwg_get_next_node_index(
        kwgde->kwg, new_node_index, SEPARATION_MACHINE_LETTER);
    if (separation_node_index != 0) {
      kwgde->dead_end_level++;
      acceptable_word_found |= kwgde_recursive_gen(
          kwgde, separation_node_index, true,
          kwgde_get_next_sequence(kwgde, current_tile_sequence,
                                  SEPARATION_MACHINE_LETTER));
      kwgde->dead_end_level--;
    }
  } else {
    if (new_node_index != 0) {
      acceptable_word_found |= kwgde_recursive_gen(
          kwgde, new_node_index, already_switched_dir, current_tile_sequence);
    }
  }

  // There's no point to marking sequences that use the whole rack
  // as dead ends since they can't be continued anyway.
  if (!acceptable_word_found && !rack_is_empty(&kwgde->rack)) {
    // If this sequence is a dead end, then we don't need to mark continuations
    // of the sequence as dead ends, thereby saving space in the dead ends
    // array.
    kwgde->number_of_dead_ends = current_number_of_dead_ends;
    set_dead_end(kwgde, current_tile_sequence);
  }

  return acceptable_word_found;
}

static inline void kwgde_set_dead_ends(KWGDeadEnds *kwgde, const KWG *kwg,
                                       const Rack *rack, int dist_size) {
  kwgde->tiles_played = 0;
  kwgde->max_tiles_played = 0;
  kwgde->number_of_dead_ends = 0;
  kwgde->dead_end_level = 0;
  memset(kwgde->dead_ends, 0, sizeof(kwgde->dead_ends));

  kwgde->kwg = kwg;
  rack_copy(&kwgde->rack, rack);

  // Use +2 to account for:
  // 1) The separation letter, and
  // 2) The sequence terminator
  // Without the separation letter, the following would be indistinguishable:
  // TAE
  // T^AE
  // Without the sequence terminator, the following would be indistinguishable:
  // TE
  // TEA
  kwgde->base_offset = dist_size + 2;
  uint64_t current_offset = 1;
  for (int i = RACK_SIZE; i >= 0; i--) {
    kwgde->dead_end_level_offsets[i] = current_offset;
    current_offset *= kwgde->base_offset;
  }

  kwgde_recursive_gen(kwgde, kwg_get_root_node_index(kwg), false, 0);
}

#endif