#include <stdint.h>

#include "kwg.h"
#include "rack.h"

// These kwg alpha functions were originally
// developed in wolges. For more details see
// https://github.com/andy-k/wolges/blob/main/details.txt

static inline uint32_t kwg_seek(const KWG *kwg, uint32_t node_index,
                                MachineLetter tile) {
  node_index = kwg_node_arc_index(kwg_node(kwg, node_index));
  if (node_index > 0) {
    for (;;) {
      uint32_t node = kwg_node(kwg, node_index);
      if (kwg_node_tile(node) == tile) {
        return node_index;
      }
      if (kwg_node_is_end(node)) {
        return 0;
      }
      node_index++;
    }
  }
  return 0;
}

static inline bool
kwg_completes_alpha_cross_set(const KWG *kwg, uint32_t node_index,
                              const Rack *rack,
                              const MachineLetter next_letter) {
  const uint16_t dist_size = rack_get_dist_size(rack);
  for (MachineLetter letter = next_letter; letter < dist_size; letter++) {
    int num_letter = rack_get_letter(rack, letter);
    for (int k = 0; k < num_letter; k++) {
      node_index = kwg_seek(kwg, node_index, letter);
      if (node_index == 0) {
        return false;
      }
    }
  }
  return kwg_node_accepts(kwg_node(kwg, node_index));
}

static inline bool kwg_accepts_alpha(const KWG *kwg, const Rack *rack) {
  return kwg_completes_alpha_cross_set(kwg, 0, rack, 1);
}

static inline bool kwg_accepts_alpha_with_blanks(const KWG *kwg,
                                                 const Rack *rack) {
  if (rack_get_letter(rack, 0) == 0) {
    return kwg_accepts_alpha(kwg, rack);
  }
  const uint16_t dist_size = rack_get_dist_size(rack);
  Rack designated_rack;
  rack_copy(&designated_rack, rack);
  rack_take_letter(&designated_rack, 0);
  for (MachineLetter letter = 1; letter < dist_size; letter++) {
    rack_add_letter(&designated_rack, letter);
    if (kwg_accepts_alpha_with_blanks(kwg, &designated_rack)) {
      return true;
    }
    rack_take_letter(&designated_rack, letter);
  }
  return false;
}

static inline uint64_t kwg_compute_alpha_cross_set(const KWG *kwg,
                                                   const Rack *rack) {
  uint64_t cross_set = 0;
  uint32_t node_index = kwg_get_dawg_root_node_index(kwg);
  if (node_index == 0) {
    return cross_set;
  }
  const uint16_t dist_size = rack_get_dist_size(rack);
  // Use 1 to skip the blank
  for (MachineLetter letter = 1; letter < dist_size; letter++) {
    int num_letter = rack_get_letter(rack, letter);
    for (int k = 0; k < num_letter; k++) {
      for (;;) {
        const uint32_t node = kwg_node(kwg, node_index);
        const MachineLetter tile = kwg_node_tile(node);
        if (tile > letter) {
          return cross_set;
        } else if (tile < letter) {
          if (kwg_completes_alpha_cross_set(kwg, node_index, rack, letter)) {
            cross_set |= 1 << tile;
          }
          if (kwg_node_is_end(node)) {
            return cross_set;
          }
          node_index++;
        } else {
          uint32_t next_node_index = kwg_node_arc_index(node);
          if (next_node_index == 0) {
            return cross_set;
          }
          node_index = next_node_index;
          break;
        }
      }
    }
  }
  for (;;) {
    uint32_t node = kwg_node(kwg, node_index);
    if (kwg_completes_alpha_cross_set(kwg, node_index, rack, dist_size)) {
      cross_set |= 1 << kwg_node_tile(node);
    }
    if (kwg_node_is_end(node)) {
      break;
    }
    node_index++;
  }
  return cross_set;
}