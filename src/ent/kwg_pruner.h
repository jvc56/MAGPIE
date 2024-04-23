

#ifndef KWG_PRUNER_H
#define KWG_PRUNER_H

#include "../def/cross_set_defs.h"

#include "kwg.h"
#include "rack.h"

typedef struct KWGPruner {
  bool clear_dead_ends;
  int tiles_played;
  int max_tiles_played;
} KWGPruner;

static inline bool kwgp_go_on(KWGPruner *kwgp, KWG *kwg, Rack *rack,
                              uint32_t new_node_index, bool accepts,
                              bool already_switched_dir);

static inline bool kwgp_recursive_gen(KWGPruner *kwgp, KWG *kwg, Rack *rack,
                                      uint32_t node_index,
                                      bool already_switched_dir) {
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
        if (number_of_ml > 0) {
          rack_take_letter(rack, ml);
          kwgp->tiles_played++;
          acceptable_word_found |= kwgp_go_on(kwgp, kwg, rack, next_node_index,
                                              accepts, already_switched_dir);
          kwgp->tiles_played--;
          rack_add_letter(rack, ml);
        }
        // check blank
        if (rack_get_letter(rack, BLANK_MACHINE_LETTER) > 0) {
          rack_take_letter(rack, BLANK_MACHINE_LETTER);
          kwgp->tiles_played++;
          acceptable_word_found |= kwgp_go_on(kwgp, kwg, rack, next_node_index,
                                              accepts, already_switched_dir);
          kwgp->tiles_played--;
          rack_add_letter(rack, BLANK_MACHINE_LETTER);
        }
      }
      if (kwg_node_is_end(node)) {
        break;
      }
    }
  }
  if (kwgp->clear_dead_ends) {
    kwg_clear_dead_end(kwg, node_index);
  } else if (!acceptable_word_found) {
    kwg_set_dead_end(kwg, node_index);
  }
  return acceptable_word_found;
}

static inline bool kwgp_go_on(KWGPruner *kwgp, KWG *kwg, Rack *rack,
                              uint32_t new_node_index, bool accepts,
                              bool already_switched_dir) {
  bool acceptable_word_found = false;
  if (!already_switched_dir) {
    if (accepts) {
      acceptable_word_found = true;
      if (kwgp->tiles_played > kwgp->max_tiles_played) {
        kwgp->max_tiles_played = kwgp->tiles_played;
      }
    }

    if (new_node_index == 0) {
      return acceptable_word_found;
    }

    acceptable_word_found |= kwgp_recursive_gen(kwgp, kwg, rack, new_node_index,
                                                already_switched_dir);
    uint32_t separation_node_index =
        kwg_get_next_node_index(kwg, new_node_index, SEPARATION_MACHINE_LETTER);
    if (separation_node_index != 0) {
      acceptable_word_found |=
          kwgp_recursive_gen(kwgp, kwg, rack, separation_node_index, true);
    }
  } else {
    if (accepts) {
      acceptable_word_found = true;
      if (kwgp->tiles_played > kwgp->max_tiles_played) {
        kwgp->max_tiles_played = kwgp->tiles_played;
      }
    }

    if (new_node_index != 0) {
      acceptable_word_found |= kwgp_recursive_gen(
          kwgp, kwg, rack, new_node_index, already_switched_dir);
    }
  }
  return acceptable_word_found;
}

static inline void kwgp_prune(KWGPruner *kwgp, Rack *rack, KWG *kwg) {
  kwgp->max_tiles_played = 0;
  kwgp->tiles_played = 0;
  kwgp->clear_dead_ends = false;
  kwgp_recursive_gen(kwgp, kwg, rack, kwg_get_root_node_index(kwg), false);
}

static inline void kwgp_unprune(KWGPruner *kwgp, Rack *rack, KWG *kwg) {
  kwgp->clear_dead_ends = true;
  kwgp_recursive_gen(kwgp, kwg, rack, kwg_get_root_node_index(kwg), false);
}

static inline int kwgp_get_max_tiles_played(KWGPruner *kwgp) {
  return kwgp->max_tiles_played;
}

#endif