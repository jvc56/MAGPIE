#ifndef KLV_H
#define KLV_H

#include "../def/klv_defs.h"
#include "kwg.h"
#include "rack.h"

// The KLV data structure was originally
// developed in wolges. For more details
// on how the KLV data structure works, see
// https://github.com/andy-k/wolges/blob/main/details.txt
typedef struct KLV {
  KWG *kwg;
  uint32_t *word_counts;
  double *leave_values;
} KLV;

KLV *klv_create(const char *data_dir, const char *klv_name);
void klv_destroy(KLV *klv);

static inline double klv_get_indexed_leave_value(const KLV *klv,
                                                 uint32_t index) {
  if (index == KLV_UNFOUND_INDEX) {
    return 0.0;
  }
  return klv->leave_values[index];
}

double klv_get_leave_value(const KLV *klv, const Rack *leave);

static inline uint32_t klv_get_root_node_index(const KLV *klv) {
  const uint32_t dawg_pointer_node = kwg_node(klv->kwg, 0);
  return kwg_node_arc_index(dawg_pointer_node);
}

static inline uint32_t increment_node_to_ml(const KLV *klv, uint32_t node_index,
                                            uint32_t word_index,
                                            uint32_t *next_word_index,
                                            uint8_t ml) {
  if (node_index == 0) {
    *next_word_index = KLV_UNFOUND_INDEX;
    return 0;
  }
  uint32_t w_idx = word_index;
  for (;;) {
    const uint32_t node = kwg_node(klv->kwg, node_index);
    if (kwg_node_tile(node) == ml) {
      *next_word_index = w_idx;
      return node_index;
    }
    if (kwg_node_is_end(node)) {
      *next_word_index = KLV_UNFOUND_INDEX;
      return 0;
    }
    w_idx += klv->word_counts[node_index] - klv->word_counts[node_index + 1];
    node_index++;
  }
}

static inline uint32_t follow_arc(const KLV *klv, uint32_t node_index,
                                  uint32_t word_index,
                                  uint32_t *next_word_index) {
  if (node_index == 0) {
    *next_word_index = KLV_UNFOUND_INDEX;
    return 0;
  }
  *next_word_index = word_index + 1;
  const uint32_t node = kwg_node(klv->kwg, node_index);
  return kwg_node_arc_index(node);
}
#endif