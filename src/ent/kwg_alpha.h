#include <stdint.h>

#include "kwg.h"
#include "rack.h"

// These kwg alpha functions were originally
// developed in wolges. For more details see
// https://github.com/andy-k/wolges/blob/main/details.txt

// FIXME: why are we using int instead of uint in this file?
static inline int32_t kwg_seek(const KWG *kwg, int32_t node_index,
                               uint8_t tile) {
  if (node_index >= 0) {
    node_index = kwg_node_arc_index(kwg_node(kwg, node_index));
    if (node_index > 0) {
      for (;;) {
        uint32_t node = kwg_node(kwg, node_index);
        if (kwg_node_tile(node) == tile) {
          return node_index;
        }
        if (kwg_node_is_end(node)) {
          return -1;
        }
        node_index++;
      }
    }
  }
  return -1;
}

static inline bool kwg_completes_alpha_cross_set(const KWG *kwg,
                                                 int32_t node_index,
                                                 const Rack *rack,
                                                 const uint8_t next_letter) {
  const int dist_size = rack_get_dist_size(rack);
  for (uint8_t letter = next_letter; letter < dist_size; letter++) {
    int num_letter = rack_get_letter(rack, letter);
    for (int k = 0; k < num_letter; k++) {
      node_index = kwg_seek(kwg, node_index, letter);
      if (node_index <= 0) {
        return false;
      }
    }
  }
  return kwg_node_accepts(kwg_node(kwg, node_index));
}

static inline bool kwg_accepts_alpha(const KWG *kwg, const Rack *rack) {
  return kwg_completes_alpha_cross_set(kwg, 0, rack, 1);
}

static inline uint64_t kwg_compute_alpha_cross_set(const KWG *kwg,
                                                   const Rack *rack) {
  // FIXME: this is set to 1 in wolges, find out why
  uint64_t cross_set = 0;
  uint32_t node_index = kwg_get_dawg_root_node_index(kwg);
  if (node_index <= 0) {
    return cross_set;
  }
  const int dist_size = rack_get_dist_size(rack);
  // Use 1 to skip the blank
  for (uint8_t letter = 1; letter < dist_size; letter++) {
    int num_letter = rack_get_letter(rack, letter);
    for (int k = 0; k < num_letter; k++) {
      for (;;) {
        const uint32_t node = kwg_node(kwg, node_index);
        const uint8_t tile = kwg_node_tile(node);
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
          if (next_node_index <= 0) {
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