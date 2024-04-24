

#ifndef KWG_PRUNER_H
#define KWG_PRUNER_H

#include "../def/cross_set_defs.h"

#include "kwg.h"
#include "rack.h"

typedef enum {
  KWGP_MODE_PRUNE,
  KWGP_MODE_UNPRUNE,
  KWGP_MODE_CLEAR,
} kwgp_mode_t;

typedef struct KWGPruner {
  uint8_t strip[15];
  kwgp_mode_t mode;
  int tiles_played;
  int max_tiles_played;
} KWGPruner;

static inline void acc_word(KWGPruner *kwgp) {
  if (kwgp->tiles_played > kwgp->max_tiles_played) {
    kwgp->max_tiles_played = kwgp->tiles_played;
  }
}

static inline void print_go_on(KWGPruner *kwgp, int current_left_col,
                               int current_right_col, uint32_t nni) {
  return;
  for (int i = 0; i < current_left_col; i++) {
    printf("#");
  }

  for (int i = current_left_col; i <= current_right_col; i++) {
    char uvc = get_unblanked_machine_letter(kwgp->strip[i]) + 'A' - 1;
    if (get_is_blanked(kwgp->strip[i])) {
      uvc += 32;
    }

    printf("%c", uvc);
  }

  for (int i = 0; i < BOARD_DIM - current_right_col - 1; i++) {
    printf("#");
  }

  printf(" %d %d %d \n", current_left_col, current_right_col, nni);
}

static inline bool kwgp_go_on(KWGPruner *kwgp, KWG *kwg, uint8_t current_letter,
                              Rack *rack, uint32_t new_node_index, bool accepts,
                              bool already_switched_dir, int cc, int li,
                              int ri);

static inline bool kwgp_recursive_gen(KWGPruner *kwgp, KWG *kwg, Rack *rack,
                                      uint32_t node_index,
                                      bool already_switched_dir, int cc, int li,
                                      int ri) {
  bool acceptable_word_found = false;
  if (!rack_is_empty(rack)) {
    for (uint32_t i = node_index;; i++) {
      const uint32_t node = kwg_node(kwg, i);
      const uint8_t ml = kwg_node_tile(node);
      int number_of_ml = rack_get_letter(rack, ml);
      if (ml != 0 && (number_of_ml != 0 ||
                      rack_get_letter(rack, BLANK_MACHINE_LETTER) != 0)) {
        const uint32_t next_node_index = kwg_node_arc_index_prefetch(node, kwg);
        bool accepts = kwg_node_accepts(node);
        acceptable_word_found |= accepts;
        // FIXME: explain why we use else if instead of another if for blanks
        if (number_of_ml > 0) {
          rack_take_letter(rack, ml);
          kwgp->tiles_played++;
          acceptable_word_found |=
              kwgp_go_on(kwgp, kwg, ml, rack, next_node_index, accepts,
                         already_switched_dir, cc, li, ri);
          kwgp->tiles_played--;
          rack_add_letter(rack, ml);
        } else if (rack_get_letter(rack, BLANK_MACHINE_LETTER) > 0) {
          rack_take_letter(rack, BLANK_MACHINE_LETTER);
          kwgp->tiles_played++;
          acceptable_word_found |= kwgp_go_on(
              kwgp, kwg, get_blanked_machine_letter(ml), rack, next_node_index,
              accepts, already_switched_dir, cc, li, ri);
          kwgp->tiles_played--;
          rack_add_letter(rack, BLANK_MACHINE_LETTER);
        }
      }
      if (kwg_node_is_end(node)) {
        break;
      }
    }
  }
  return acceptable_word_found;
}

#define START_COL (BOARD_DIM / 2)

static inline bool kwgp_go_on(KWGPruner *kwgp, KWG *kwg, uint8_t current_letter,
                              Rack *rack, uint32_t new_node_index, bool accepts,
                              bool already_switched_dir, int cc, int li,
                              int ri) {
  // printf("go_on: %d, %d, %d \n", cc, current_letter, new_node_index);

  kwgp->strip[cc] = current_letter;

  bool acceptable_word_found = accepts;

  if (accepts) {
    acc_word(kwgp);
  } else {
  }

  print_go_on(kwgp, li, ri, new_node_index);

  if (!already_switched_dir) {

    li = cc;
    print_go_on(kwgp, li, ri, new_node_index);
    if (new_node_index == 0) {
      return acceptable_word_found;
    }

    acceptable_word_found |= kwgp_recursive_gen(
        kwgp, kwg, rack, new_node_index, already_switched_dir, cc - 1, li, ri);
    uint32_t separation_node_index =
        kwg_get_next_node_index(kwg, new_node_index, SEPARATION_MACHINE_LETTER);
    if (separation_node_index != 0) {
      acceptable_word_found |= kwgp_recursive_gen(
          kwgp, kwg, rack, separation_node_index, true, START_COL + 1, li, ri);
    }
  } else {
    ri = cc;
    print_go_on(kwgp, li, ri, new_node_index);
    if (new_node_index != 0) {
      acceptable_word_found |=
          kwgp_recursive_gen(kwgp, kwg, rack, new_node_index,
                             already_switched_dir, cc + 1, li, ri);
    }
  }

  switch (kwgp->mode) {
  case KWGP_MODE_CLEAR:
    kwg_clear_dead_end(kwg, new_node_index);
    break;
  case KWGP_MODE_PRUNE:
    if (!acceptable_word_found) {
      kwg_set_dead_end(kwg, new_node_index);
      // printf("sd:%d\n", new_node_index);
    }
    break;
  case KWGP_MODE_UNPRUNE:
    if (acceptable_word_found) {
      kwg_clear_dead_end(kwg, new_node_index);
    }
    break;
  }
  print_go_on(kwgp, li, ri, new_node_index);

  return acceptable_word_found;
}

static inline void kwgp_prune(KWGPruner *kwgp, Rack *rack, KWG *kwg) {
  kwgp->max_tiles_played = 0;
  kwgp->tiles_played = 0;
  kwgp->mode = KWGP_MODE_PRUNE;
  kwgp_recursive_gen(kwgp, kwg, rack, kwg_get_root_node_index(kwg), false,
                     START_COL, START_COL, START_COL);
  kwgp->mode = KWGP_MODE_UNPRUNE;
  kwgp_recursive_gen(kwgp, kwg, rack, kwg_get_root_node_index(kwg), false,
                     START_COL, START_COL, START_COL);
}

static inline void kwgp_unprune(KWGPruner *kwgp, Rack *rack, KWG *kwg) {
  kwgp->mode = KWGP_MODE_CLEAR;
  kwgp_recursive_gen(kwgp, kwg, rack, kwg_get_root_node_index(kwg), false,
                     START_COL, START_COL, START_COL);
}

static inline int kwgp_get_max_tiles_played(KWGPruner *kwgp) {
  return kwgp->max_tiles_played;
}

#endif