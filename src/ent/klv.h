#ifndef KLV_H
#define KLV_H

#include "../compat/endian_conv.h"
#include "../def/bit_rack_defs.h"
#include "../def/klv_defs.h"
#include "../ent/kwg.h"
#include "../util/fileproxy.h"
#include "../util/fnv.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "data_filepaths.h"
#include "kwg.h"
#include "rack.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

// The KLV data structure was originally
// developed in wolges. For more details
// on how the KLV data structure works, see
// https://github.com/andy-k/wolges/blob/main/details.txt
typedef struct KLV {
  uint32_t number_of_leaves;
  KWG *kwg;
  char *name;
  uint32_t *word_counts;
  Equity *leave_values;
  // Optional KLV3 contextual model. The base KWG and leave_values remain
  // ordinary KLV2 data; these small tensors add a pool-dependent adjustment
  // for every tile retained in a leave:
  //
  //   adjustment[held] =
  //       bias[pool_bin][draw_count][held] +
  //       sum_unseen(weights[draw_count][held][unseen] *
  //                  unseen_count[unseen]) / unseen_total
  //
  // A leave's contextual residual is the sum of its retained-tile
  // adjustments, with multiplicity. NULL arrays mean a legacy KLV2.
  int context_alphabet_size;
  int context_num_pool_bins;
  uint16_t *context_pool_bin_upper_bounds;
  Equity *context_biases;
  Equity *context_weights;
  // KLV3 v2 stores one empirically calibrated upper adjustment per ordinary
  // KLV leave index. A KLV3-capable RIT folds these caps through every
  // canonical subrack once at construction time, producing cheap
  // best-leave bounds without evaluating the contextual model for all
  // subracks at runtime. The caps are intentionally quantile bounds, not hard
  // correctness bounds; final leave values are always evaluated exactly.
  Equity *context_leave_caps;
  uint32_t context_cap_quantile_ppm;
  uint32_t context_cap_min_samples;
  // Stable content fingerprints used to reject stale RIT leave data. The
  // base fingerprint covers the KWG and KLV2 values. The context fingerprint
  // additionally covers the KLV3 tensors and per-leave caps.
  uint64_t base_content_fingerprint;
  uint64_t context_content_fingerprint;
  // Optional, explicitly lossy shadow-pruning experiment. When enabled,
  // movegen may cap the contextual leave upper bound at
  // base_best_leave + context_pruning_cap. Final leave evaluation remains
  // exact; only shadow/WMP pruning consumes the capped bound.
  bool context_pruning_cap_enabled;
  Equity context_pruning_cap;
  // Optional hybrid evaluator. When a cheap sampled contextual-adjustment
  // magnitude is no greater than this threshold, movegen uses the embedded
  // KLV2 base and its rack-keyed caches.
  // The threshold is a runtime policy and is not part of the KLV3 file or its
  // content fingerprint.
  bool context_fallback_threshold_enabled;
  Equity context_fallback_threshold;
  // Bumped on every mutation of leave_values. MoveGen caches that store
  // leave-derived data (subrack cache, anchor cache upper bounds, etc.)
  // compare this counter to the value captured at the last gen_load_position
  // and invalidate when it has changed, even when the KLV pointer itself
  // hasn't moved. Only test code currently mutates leave_values in-place;
  // production KLVs are loaded once and treated as immutable.
  uint64_t mutation_counter;
} KLV;

static inline const char *klv_get_name(const KLV *klv) { return klv->name; }

static inline void klv_set_context_fallback_threshold(KLV *klv, bool enabled,
                                                      Equity threshold) {
  if (klv == NULL) {
    return;
  }
  klv->context_fallback_threshold_enabled = enabled;
  klv->context_fallback_threshold = threshold;
}

// A value unique to this loaded KLV instance: a hash of the base addresses and
// size of its freshly-allocated leave_values/word_counts arrays. Two different
// loads (even of the same lexicon) produce different fingerprints, so MoveGen
// caches that hold leave-derived data can detect that the backing KLV has been
// replaced even when a new KLV is allocated at a freed one's address (ABA),
// which the pointer comparison cannot see.
static inline uint64_t klv_get_instance_fingerprint(const KLV *klv) {
  uint64_t hash = FNV_64_OFFSET_BASIS;
  hash = fnv64a_step(hash, (uintptr_t)klv->leave_values);
  hash = fnv64a_step(hash, (uintptr_t)klv->word_counts);
  hash = fnv64a_step(hash, (uintptr_t)klv->kwg);
  hash = fnv64a_step(hash, (uintptr_t)klv->context_weights);
  hash = fnv64a_step(hash, (uint64_t)klv->number_of_leaves);
  return hash;
}

static inline bool klv_has_context_model(const KLV *klv) {
  return klv != NULL && klv->context_weights != NULL;
}

static inline bool klv_has_context_leave_caps(const KLV *klv) {
  return klv_has_context_model(klv) && klv->context_leave_caps != NULL;
}

static inline void klv_set_context_pruning_cap(KLV *klv, Equity cap) {
  if (cap < 0) {
    log_fatal("KLV3 context pruning cap must be nonnegative");
  }
  klv->context_pruning_cap = cap;
  klv->context_pruning_cap_enabled = true;
}

static inline void klv_disable_context_pruning_cap(KLV *klv) {
  klv->context_pruning_cap_enabled = false;
}

static inline Equity klv_get_context_leave_cap(const KLV *klv, uint32_t index) {
  if (!klv_has_context_leave_caps(klv) || index == KLV_UNFOUND_INDEX) {
    return 0;
  }
  return klv->context_leave_caps[index];
}

static inline size_t klv3_bias_index(const KLV *klv, int pool_bin,
                                     int draw_count, MachineLetter held_tile) {
  return (((size_t)pool_bin * KLV3_DRAW_COUNT_HEADS + (size_t)draw_count) *
              (size_t)klv->context_alphabet_size +
          held_tile);
}

static inline size_t klv3_weight_index(const KLV *klv, int draw_count,
                                       MachineLetter held_tile,
                                       MachineLetter unseen_tile) {
  return (
      ((size_t)draw_count * (size_t)klv->context_alphabet_size + held_tile) *
          (size_t)klv->context_alphabet_size +
      unseen_tile);
}

static inline int klv3_get_pool_bin(const KLV *klv, int unseen_total) {
  for (int bin = 0; bin < klv->context_num_pool_bins; bin++) {
    if (unseen_total <= klv->context_pool_bin_upper_bounds[bin]) {
      return bin;
    }
  }
  return klv->context_num_pool_bins - 1;
}

static inline Equity
klv3_compute_one_tile_adjustment(const KLV *klv, const int *unseen_counts,
                                 int unseen_total, int pool_bin, int draw_count,
                                 MachineLetter held) {
  int64_t weighted_sum = 0;
  for (int unseen = 0; unseen < klv->context_alphabet_size; unseen++) {
    weighted_sum += (int64_t)klv->context_weights[klv3_weight_index(
                        klv, draw_count, held, (MachineLetter)unseen)] *
                    unseen_counts[unseen];
  }
  const int64_t rounded_average =
      weighted_sum >= 0 ? (weighted_sum + unseen_total / 2) / unseen_total
                        : -((-weighted_sum + unseen_total / 2) / unseen_total);
  const int64_t value =
      (int64_t)klv
          ->context_biases[klv3_bias_index(klv, pool_bin, draw_count, held)] +
      rounded_average;
  if (value > EQUITY_MAX_VALUE || value < EQUITY_MIN_VALUE) {
    log_fatal("KLV3 tile adjustment out of Equity range");
  }
  return (Equity)value;
}

// Compute every held-tile adjustment once per position. Querying any leave
// after this is only an addition per retained tile. unseen_counts is the
// public unseen multiset (bag + opponent rack), not the engine's private bag
// partition.
static inline void klv3_compute_tile_adjustments(
    const KLV *klv, const int *unseen_counts, int unseen_total,
    Equity adjustments[KLV3_DRAW_COUNT_HEADS][MACHINE_LETTER_MAX_VALUE]) {
  memset(adjustments, 0,
         sizeof(Equity) * KLV3_DRAW_COUNT_HEADS * MACHINE_LETTER_MAX_VALUE);
  if (!klv_has_context_model(klv) || unseen_total <= 0) {
    return;
  }
  const int alphabet_size = klv->context_alphabet_size;
  const int pool_bin = klv3_get_pool_bin(klv, unseen_total);
  // draw_count == 0 deliberately remains zero: the contextual model is a
  // replenishment model and is trained only on moves that draw.
  for (int draw_count = 1; draw_count < KLV3_DRAW_COUNT_HEADS; draw_count++) {
    for (int held = 0; held < alphabet_size; held++) {
      adjustments[draw_count][held] = klv3_compute_one_tile_adjustment(
          klv, unseen_counts, unseen_total, pool_bin, draw_count,
          (MachineLetter)held);
    }
  }
}

// Move generation only evaluates subracks of held_rack, so adjustments for
// tile types absent from that rack can never be read. Computing just those
// rows avoids most of the KLV3 matrix-vector product on ordinary racks while
// preserving the same zero-filled table and lookup interface.
static inline void klv3_compute_rack_tile_adjustments(
    const KLV *klv, const int *unseen_counts, int unseen_total,
    const Rack *held_rack,
    Equity adjustments[KLV3_DRAW_COUNT_HEADS][MACHINE_LETTER_MAX_VALUE]) {
  memset(adjustments, 0,
         sizeof(Equity) * KLV3_DRAW_COUNT_HEADS * MACHINE_LETTER_MAX_VALUE);
  if (!klv_has_context_model(klv) || unseen_total <= 0) {
    return;
  }
  const int pool_bin = klv3_get_pool_bin(klv, unseen_total);
  for (int held = 0; held < klv->context_alphabet_size; held++) {
    if (rack_get_letter(held_rack, held) == 0) {
      continue;
    }
    for (int draw_count = 1; draw_count < KLV3_DRAW_COUNT_HEADS; draw_count++) {
      adjustments[draw_count][held] = klv3_compute_one_tile_adjustment(
          klv, unseen_counts, unseen_total, pool_bin, draw_count,
          (MachineLetter)held);
    }
  }
}

// Cheap hybrid-policy signal: sample the model at a representative
// four-tile draw (or the remaining bag size when smaller) and sum the
// absolute per-tile adjustments over the current rack. This costs one
// matrix row per distinct held tile instead of every draw-count row and does
// not enumerate subracks. It is deliberately a heuristic magnitude, not a
// bound; thresholds must be validated for missed KLV2/KLV3 disagreements.
static inline Equity
klv3_sample_rack_adjustment_magnitude(const KLV *klv, const int *unseen_counts,
                                      int unseen_total, const Rack *held_rack,
                                      int tiles_in_bag) {
  if (!klv_has_context_model(klv) || unseen_total <= 0 || tiles_in_bag <= 0) {
    return 0;
  }
  int draw_count = tiles_in_bag < 4 ? tiles_in_bag : 4;
  if (draw_count >= KLV3_DRAW_COUNT_HEADS) {
    draw_count = KLV3_DRAW_COUNT_HEADS - 1;
  }
  const int pool_bin = klv3_get_pool_bin(klv, unseen_total);
  int64_t magnitude = 0;
  for (int held = 0; held < klv->context_alphabet_size; held++) {
    const int count = rack_get_letter(held_rack, held);
    if (count == 0) {
      continue;
    }
    const Equity adjustment = klv3_compute_one_tile_adjustment(
        klv, unseen_counts, unseen_total, pool_bin, draw_count,
        (MachineLetter)held);
    magnitude +=
        (int64_t)count * (adjustment < 0 ? -(int64_t)adjustment : adjustment);
  }
  if (magnitude > EQUITY_MAX_VALUE) {
    log_fatal("KLV3 sampled adjustment magnitude out of Equity range");
  }
  return (Equity)magnitude;
}

// Return the largest possible difference between contextual adjustments of
// any two subracks of held_rack. This is an inexpensive, model-native measure
// of whether context could plausibly reorder KLV2-evaluated moves. It is an
// upper range over all subracks, not a claim that every subrack is formable by
// a legal move.
static inline Equity klv3_get_rack_adjustment_range(
    const Rack *held_rack, int tiles_in_bag,
    const Equity adjustments[KLV3_DRAW_COUNT_HEADS][MACHINE_LETTER_MAX_VALUE]) {
  MachineLetter rack_tiles[RACK_SIZE];
  int num_rack_tiles = 0;
  for (int ml = 0; ml < rack_get_dist_size(held_rack); ml++) {
    const int count = rack_get_letter(held_rack, ml);
    for (int i = 0; i < count; i++) {
      if (num_rack_tiles >= RACK_SIZE) {
        log_fatal("rack exceeds RACK_SIZE while measuring KLV3 adjustment");
      }
      rack_tiles[num_rack_tiles++] = (MachineLetter)ml;
    }
  }

  int64_t minimum = 0;
  int64_t maximum = 0;
  const uint32_t num_subracks = (uint32_t)1 << num_rack_tiles;
  for (uint32_t mask = 0; mask < num_subracks; mask++) {
    int leave_size = 0;
    for (int tile_index = 0; tile_index < num_rack_tiles; tile_index++) {
      leave_size += (mask >> tile_index) & 1U;
    }
    int draw_count = num_rack_tiles - leave_size;
    if (draw_count > tiles_in_bag) {
      draw_count = tiles_in_bag;
    }
    if (draw_count >= KLV3_DRAW_COUNT_HEADS) {
      draw_count = KLV3_DRAW_COUNT_HEADS - 1;
    }

    int64_t value = 0;
    for (int tile_index = 0; tile_index < num_rack_tiles; tile_index++) {
      if (((mask >> tile_index) & 1U) != 0) {
        value += adjustments[draw_count][rack_tiles[tile_index]];
      }
    }
    if (value < minimum) {
      minimum = value;
    }
    if (value > maximum) {
      maximum = value;
    }
  }
  const int64_t range = maximum - minimum;
  if (range > EQUITY_MAX_VALUE) {
    log_fatal("KLV3 rack adjustment range out of Equity range");
  }
  return (Equity)range;
}

static inline Equity klv_get_contextual_indexed_leave_value(
    const KLV *klv, uint32_t index, const Rack *leave, int draw_count,
    const Equity adjustments[KLV3_DRAW_COUNT_HEADS][MACHINE_LETTER_MAX_VALUE]) {
  Equity value = index == KLV_UNFOUND_INDEX ? 0 : klv->leave_values[index];
  if (!klv_has_context_model(klv) || index == KLV_UNFOUND_INDEX ||
      draw_count <= 0) {
    return value;
  }
  if (draw_count >= KLV3_DRAW_COUNT_HEADS) {
    draw_count = KLV3_DRAW_COUNT_HEADS - 1;
  }
  int64_t adjusted = value;
  for (int ml = 0; ml < rack_get_dist_size(leave); ml++) {
    adjusted +=
        (int64_t)rack_get_letter(leave, ml) * adjustments[draw_count][ml];
  }
  if (adjusted > EQUITY_MAX_VALUE || adjusted < EQUITY_MIN_VALUE) {
    log_fatal("KLV3 leave value out of Equity range");
  }
  return (Equity)adjusted;
}

static inline const KWG *klv_get_kwg(const KLV *klv) { return klv->kwg; }

static inline uint32_t klv_get_number_of_leaves(const KLV *klv) {
  return klv->number_of_leaves;
}

static inline void klv_set_all_leave_values_to_zero(KLV *klv) {
  memset(klv->leave_values, int_to_equity(0),
         klv->number_of_leaves * sizeof(Equity));
  klv->mutation_counter++;
  klv->base_content_fingerprint = 0;
  klv->context_content_fingerprint = 0;
}

static inline uint64_t klv_get_mutation_counter(const KLV *klv) {
  return klv->mutation_counter;
}

static inline Equity klv_get_indexed_leave_value(const KLV *klv,
                                                 uint32_t index) {
  if (index == KLV_UNFOUND_INDEX) {
    return 0;
  }
  return klv->leave_values[index];
}

// Assumes the index is valid
static inline Equity klv_set_indexed_leave_value(const KLV *klv, uint32_t index,
                                                 Equity value) {
  // Bumping the mutation counter requires a non-const KLV, but the public
  // API historically accepts const to avoid churn at call sites that treat
  // the KLV as mostly-immutable. Cast away const for the counter bump only.
  ((KLV *)klv)->mutation_counter++;
  ((KLV *)klv)->base_content_fingerprint = 0;
  ((KLV *)klv)->context_content_fingerprint = 0;
  return klv->leave_values[index] = value;
}

static inline uint32_t klv_get_root_node_index(const KLV *klv) {
  const uint32_t dawg_pointer_node = kwg_node(klv->kwg, 0);
  return kwg_node_arc_index(dawg_pointer_node);
}

static inline uint32_t increment_node_to_ml(const KLV *klv, uint32_t node_index,
                                            uint32_t word_index,
                                            uint32_t *next_word_index,
                                            MachineLetter ml) {
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

static inline int klv_count_words_at(const KLV *klv, uint32_t node_index,
                                     uint32_t kwg_size) {
  if (node_index >= kwg_size) {
    return 0;
  }
  if (klv->word_counts[node_index] == KLV_UNFOUND_INDEX) {
    log_fatal("unexpected word count %u at %u", klv->word_counts[node_index],
              node_index);
  }
  if (klv->word_counts[node_index] == 0) {
    klv->word_counts[node_index] = KLV_UNFOUND_INDEX;

    const uint32_t node = kwg_node(klv->kwg, node_index);
    int this_node_word_count = 0;
    if (kwg_node_accepts(node)) {
      this_node_word_count = 1;
    }
    int child_word_count = 0;
    const uint32_t next_node_index = kwg_node_arc_index(node);
    if (next_node_index != 0) {
      child_word_count = klv_count_words_at(klv, next_node_index, kwg_size);
    }
    int sibling_word_count = 0;
    const bool is_not_end = !kwg_node_is_end(node);
    if (is_not_end) {
      sibling_word_count = klv_count_words_at(klv, node_index + 1, kwg_size);
    }
    klv->word_counts[node_index] =
        this_node_word_count + child_word_count + sibling_word_count;
  }
  return (int)klv->word_counts[node_index];
}

static inline void klv_count_words(const KLV *klv, size_t kwg_size) {
  for (size_t p = kwg_size - 1;; p--) {
    klv_count_words_at(klv, p, kwg_size);
    if (p == 0) {
      break;
    }
  }
}

static inline uint64_t klv_compute_base_content_fingerprint(const KLV *klv) {
  uint64_t hash = FNV_64_OFFSET_BASIS;
  const int kwg_size = kwg_get_number_of_nodes(klv->kwg);
  hash = fnv64a_step(hash, (uint64_t)(uint32_t)kwg_size);
  for (int i = 0; i < kwg_size; i++) {
    hash = fnv64a_step(hash, (uint64_t)kwg_node(klv->kwg, (uint32_t)i));
  }
  hash = fnv64a_step(hash, klv->number_of_leaves);
  for (uint32_t i = 0; i < klv->number_of_leaves; i++) {
    hash = fnv64a_step(hash, (uint64_t)(uint32_t)klv->leave_values[i]);
  }
  return hash == 0 ? 1 : hash;
}

static inline uint64_t klv_compute_context_content_fingerprint(const KLV *klv) {
  uint64_t hash = klv_compute_base_content_fingerprint(klv);
  if (!klv_has_context_model(klv)) {
    return hash;
  }
  hash = fnv64a_step(hash, (uint64_t)(uint32_t)klv->context_alphabet_size);
  hash = fnv64a_step(hash, (uint64_t)(uint32_t)klv->context_num_pool_bins);
  for (int i = 0; i < klv->context_num_pool_bins; i++) {
    hash = fnv64a_step(hash, klv->context_pool_bin_upper_bounds[i]);
  }
  const size_t num_biases = (size_t)klv->context_num_pool_bins *
                            KLV3_DRAW_COUNT_HEADS *
                            (size_t)klv->context_alphabet_size;
  const size_t num_weights = (size_t)KLV3_DRAW_COUNT_HEADS *
                             (size_t)klv->context_alphabet_size *
                             (size_t)klv->context_alphabet_size;
  for (size_t i = 0; i < num_biases; i++) {
    hash = fnv64a_step(hash, (uint64_t)(uint32_t)klv->context_biases[i]);
  }
  for (size_t i = 0; i < num_weights; i++) {
    hash = fnv64a_step(hash, (uint64_t)(uint32_t)klv->context_weights[i]);
  }
  hash = fnv64a_step(hash, klv->context_cap_quantile_ppm);
  hash = fnv64a_step(hash, klv->context_cap_min_samples);
  if (klv->context_leave_caps != NULL) {
    for (uint32_t i = 0; i < klv->number_of_leaves; i++) {
      hash = fnv64a_step(hash, (uint64_t)(uint32_t)klv->context_leave_caps[i]);
    }
  }
  return hash == 0 ? 1 : hash;
}

static inline uint64_t klv_get_base_content_fingerprint(const KLV *klv) {
  assert(klv != NULL);
  if (klv->base_content_fingerprint == 0) {
    ((KLV *)klv)->base_content_fingerprint =
        klv_compute_base_content_fingerprint(klv);
  }
  return klv->base_content_fingerprint;
}

static inline uint64_t klv_get_context_content_fingerprint(const KLV *klv) {
  if (klv->context_content_fingerprint == 0) {
    ((KLV *)klv)->context_content_fingerprint =
        klv_compute_context_content_fingerprint(klv);
  }
  return klv->context_content_fingerprint;
}

static inline void klv3_load_context(FILE *stream, KLV *klv) {
  static const uint8_t expected_magic[8] = {'M', 'A', 'G', 'K',
                                            'L', 'V', '3', '\0'};
  uint8_t magic[8];
  if (fread(magic, sizeof(magic), 1, stream) != 1 ||
      memcmp(magic, expected_magic, sizeof(magic)) != 0) {
    log_fatal("invalid or missing KLV3 context trailer");
  }
  uint32_t header[4];
  if (fread(header, sizeof(uint32_t), 4, stream) != 4) {
    log_fatal("could not read KLV3 context header");
  }
  const uint32_t version = le32toh(header[0]);
  const uint32_t alphabet_size = le32toh(header[1]);
  const uint32_t draw_count_heads = le32toh(header[2]);
  const uint32_t num_pool_bins = le32toh(header[3]);
  if (version < KLV3_EARLIEST_SUPPORTED_VERSION || version > KLV3_VERSION ||
      alphabet_size == 0 || alphabet_size > BIT_RACK_MAX_ALPHABET_SIZE ||
      draw_count_heads != KLV3_DRAW_COUNT_HEADS || num_pool_bins == 0 ||
      num_pool_bins > KLV3_MAX_POOL_SIZE_BINS) {
    log_fatal("unsupported KLV3 context header");
  }
  klv->context_alphabet_size = (int)alphabet_size;
  klv->context_num_pool_bins = (int)num_pool_bins;
  klv->context_pool_bin_upper_bounds =
      malloc_or_die(num_pool_bins * sizeof(uint16_t));
  if (fread(klv->context_pool_bin_upper_bounds, sizeof(uint16_t), num_pool_bins,
            stream) != num_pool_bins) {
    log_fatal("could not read KLV3 pool-size bins");
  }
  for (uint32_t i = 0; i < num_pool_bins; i++) {
    klv->context_pool_bin_upper_bounds[i] =
        le16toh(klv->context_pool_bin_upper_bounds[i]);
    if (klv->context_pool_bin_upper_bounds[i] == 0 ||
        (i > 0 && klv->context_pool_bin_upper_bounds[i] <=
                      klv->context_pool_bin_upper_bounds[i - 1])) {
      log_fatal("KLV3 pool-size bounds must be positive and increasing");
    }
  }

  const size_t num_biases =
      (size_t)num_pool_bins * draw_count_heads * (size_t)alphabet_size;
  const size_t num_weights =
      (size_t)draw_count_heads * alphabet_size * (size_t)alphabet_size;
  float *floats = malloc_or_die(
      (num_biases > num_weights ? num_biases : num_weights) * sizeof(float));
  klv->context_biases = malloc_or_die(num_biases * sizeof(Equity));
  klv->context_weights = malloc_or_die(num_weights * sizeof(Equity));
  if (fread(floats, sizeof(float), num_biases, stream) != num_biases) {
    log_fatal("could not read KLV3 context biases");
  }
  for (size_t i = 0; i < num_biases; i++) {
    const double value = (double)convert_float_to_le(floats[i]);
    if (!isfinite(value)) {
      log_fatal("KLV3 context bias is not finite");
    }
    klv->context_biases[i] = double_to_equity(value);
  }
  if (fread(floats, sizeof(float), num_weights, stream) != num_weights) {
    log_fatal("could not read KLV3 context weights");
  }
  for (size_t i = 0; i < num_weights; i++) {
    const double value = (double)convert_float_to_le(floats[i]);
    if (!isfinite(value)) {
      log_fatal("KLV3 context weight is not finite");
    }
    klv->context_weights[i] = double_to_equity(value);
  }
  free(floats);

  if (version >= 2) {
    uint32_t cap_header[2];
    if (fread(cap_header, sizeof(uint32_t), 2, stream) != 2) {
      log_fatal("could not read KLV3 context cap header");
    }
    klv->context_cap_quantile_ppm = le32toh(cap_header[0]);
    klv->context_cap_min_samples = le32toh(cap_header[1]);
    if (klv->context_cap_quantile_ppm == 0 ||
        klv->context_cap_quantile_ppm > KLV3_CAP_QUANTILE_SCALE) {
      log_fatal("invalid KLV3 context cap quantile");
    }
    float *cap_floats =
        malloc_or_die((size_t)klv->number_of_leaves * sizeof(float));
    klv->context_leave_caps =
        malloc_or_die((size_t)klv->number_of_leaves * sizeof(Equity));
    if (fread(cap_floats, sizeof(float), klv->number_of_leaves, stream) !=
        klv->number_of_leaves) {
      log_fatal("could not read KLV3 context leave caps");
    }
    for (uint32_t i = 0; i < klv->number_of_leaves; i++) {
      const double value = (double)convert_float_to_le(cap_floats[i]);
      if (!isfinite(value)) {
        log_fatal("KLV3 context leave cap is not finite");
      }
      klv->context_leave_caps[i] = double_to_equity(value);
    }
    free(cap_floats);
  }
}

static inline void klv_load(const char *klv_name, const char *klv_filename,
                            bool load_context, KLV *klv,
                            ErrorStack *error_stack) {
  FILE *stream = stream_from_filename(klv_filename, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  klv->name = string_duplicate(klv_name);

  uint32_t kwg_size;
  size_t result;

  result = fread(&kwg_size, sizeof(kwg_size), 1, stream);
  if (result != 1) {
    log_fatal("kwg size fread failure: %zd != %d", result, 1);
  }
  kwg_size = le32toh(kwg_size);

  klv->kwg = kwg_create_empty();

  kwg_read_nodes_from_stream(klv->kwg, kwg_size, stream);

  uint32_t number_of_leaves;
  result = fread(&number_of_leaves, sizeof(number_of_leaves), 1, stream);
  if (result != 1) {
    log_fatal("number of leaves fread failure: %zd != %d", result, 1);
  }
  number_of_leaves = le32toh(number_of_leaves);
  klv->number_of_leaves = number_of_leaves;

  klv->leave_values =
      (Equity *)malloc_or_die(number_of_leaves * sizeof(Equity));
  float *temp_floats = (float *)malloc_or_die(number_of_leaves * sizeof(float));
  result = fread(temp_floats, sizeof(float), number_of_leaves, stream);
  if (result != number_of_leaves) {
    log_fatal("edges fread failure: %zd != %d", result, number_of_leaves);
  }

  for (uint32_t i = 0; i < number_of_leaves; i++) {
    klv->leave_values[i] =
        double_to_equity((double)convert_float_to_le(temp_floats[i]));
  }
  free(temp_floats);

  if (load_context) {
    klv3_load_context(stream, klv);
  }
  fclose_or_die(stream);

  klv->word_counts = malloc_or_die(kwg_size * sizeof(uint32_t));
  for (size_t i = 0; i < kwg_size; i++) {
    klv->word_counts[i] = 0;
  }

  klv_count_words(klv, kwg_size);
  klv->base_content_fingerprint = klv_compute_base_content_fingerprint(klv);
  klv->context_content_fingerprint =
      klv_compute_context_content_fingerprint(klv);
}

static inline void klv_destroy(KLV *klv) {
  if (!klv) {
    return;
  }
  kwg_destroy(klv->kwg);
  free(klv->leave_values);
  free(klv->word_counts);
  free(klv->context_pool_bin_upper_bounds);
  free(klv->context_biases);
  free(klv->context_weights);
  free(klv->context_leave_caps);
  free(klv->name);
  free(klv);
}

static inline KLV *klv_create(const char *data_paths, const char *klv_name,
                              ErrorStack *error_stack) {

  bool load_context = false;
  ErrorStack *klv3_probe_errors = error_stack_create();
  char *klv_filename = data_filepaths_get_readable_filename(
      data_paths, klv_name, DATA_FILEPATH_TYPE_KLV3, klv3_probe_errors);
  if (error_stack_is_empty(klv3_probe_errors)) {
    load_context = true;
  } else {
    error_stack_reset(klv3_probe_errors);
    klv_filename = data_filepaths_get_readable_filename(
        data_paths, klv_name, DATA_FILEPATH_TYPE_KLV, error_stack);
  }
  error_stack_destroy(klv3_probe_errors);
  KLV *klv = NULL;
  if (error_stack_is_empty(error_stack)) {
    klv = calloc_or_die(1, sizeof(KLV));
    klv_load(klv_name, klv_filename, load_context, klv, error_stack);
  }
  free(klv_filename);
  if (!error_stack_is_empty(error_stack)) {
    klv_destroy(klv);
    klv = NULL;
  }
  return klv;
}

// Takes ownership of the KWG
static inline KLV *klv_create_zeroed_from_kwg(KWG *kwg, int number_of_leaves,
                                              const char *klv_name) {
  KLV *klv = calloc_or_die(1, sizeof(KLV));
  klv->kwg = kwg;
  klv->name = string_duplicate(klv_name);
  klv->number_of_leaves = number_of_leaves;
  klv->leave_values = (Equity *)calloc_or_die(number_of_leaves, sizeof(Equity));
  const int number_of_kwg_nodes = kwg_get_number_of_nodes(kwg);
  klv->word_counts =
      (uint32_t *)calloc_or_die(number_of_kwg_nodes, sizeof(uint32_t));
  klv_count_words(klv, number_of_kwg_nodes);
  return klv;
}

static inline uint32_t klv_get_word_index_internal(const KLV *klv,
                                                   const Rack *leave,
                                                   uint32_t node_index) {
  uint32_t idx = 0;
  MachineLetter lidx = 0;
  int8_t lidx_letter_count = rack_get_letter(leave, lidx);
  uint16_t number_of_letters = rack_get_total_letters(leave);

  // Advance lidx
  while (lidx_letter_count == 0) {
    lidx++;
    lidx_letter_count = rack_get_letter(leave, lidx);
  }

  while (node_index != 0) {
    uint32_t next_word_index;
    node_index =
        increment_node_to_ml(klv, node_index, idx, &next_word_index, lidx);
    if (node_index == KLV_UNFOUND_INDEX) {
      return node_index;
    }
    idx = next_word_index;

    lidx_letter_count--;
    number_of_letters--;

    // Advance lidx
    while (lidx_letter_count == 0) {
      lidx++;
      if (lidx >= rack_get_dist_size(leave)) {
        break;
      }
      lidx_letter_count = rack_get_letter(leave, lidx);
    }

    if (number_of_letters == 0) {
      return idx;
    }

    node_index = follow_arc(klv, node_index, idx, &next_word_index);
    idx = next_word_index;
  }
  return KLV_UNFOUND_INDEX;
}

static inline uint32_t klv_get_word_index(const KLV *klv, const Rack *leave) {
  if (rack_is_empty(leave)) {
    return KLV_UNFOUND_INDEX;
  }
  if (!klv) {
    return KLV_UNFOUND_INDEX;
  }
  return klv_get_word_index_internal(klv, leave,
                                     kwg_get_dawg_root_node_index(klv->kwg));
}

static inline Equity klv_get_leave_value(const KLV *klv, const Rack *leave) {
  if (rack_is_empty(leave)) {
    return 0;
  }
  if (!klv) {
    return 0;
  }
  const uint32_t index = klv_get_word_index_internal(
      klv, leave, kwg_get_dawg_root_node_index(klv->kwg));
  return klv_get_indexed_leave_value(klv, index);
}

// Evaluate one leave against the current public unseen pool without building
// the full draw-count-by-held-tile adjustment matrix used by move generation.
// This is intended for simulation horizon residuals, where only the selected
// move's leave is needed. KLV2s and missing/empty contexts retain the ordinary
// rack-only behavior.
static inline Equity klv_get_contextual_leave_value(const KLV *klv,
                                                    const Rack *leave,
                                                    const int *unseen_counts,
                                                    int unseen_total,
                                                    int draw_count) {
  if (rack_is_empty(leave) || !klv) {
    return 0;
  }
  const uint32_t index = klv_get_word_index_internal(
      klv, leave, kwg_get_dawg_root_node_index(klv->kwg));
  const Equity base_value = klv_get_indexed_leave_value(klv, index);
  if (!klv_has_context_model(klv) || index == KLV_UNFOUND_INDEX ||
      unseen_counts == NULL || unseen_total <= 0 || draw_count <= 0) {
    return base_value;
  }
  if (draw_count >= KLV3_DRAW_COUNT_HEADS) {
    draw_count = KLV3_DRAW_COUNT_HEADS - 1;
  }
  const int pool_bin = klv3_get_pool_bin(klv, unseen_total);
  int64_t adjusted = base_value;
  for (int ml = 0; ml < rack_get_dist_size(leave); ml++) {
    const int count = rack_get_letter(leave, ml);
    if (count == 0) {
      continue;
    }
    adjusted += (int64_t)count * klv3_compute_one_tile_adjustment(
                                     klv, unseen_counts, unseen_total, pool_bin,
                                     draw_count, (MachineLetter)ml);
  }
  if (adjusted > EQUITY_MAX_VALUE || adjusted < EQUITY_MIN_VALUE) {
    log_fatal("KLV3 contextual leave value out of Equity range");
  }
  return (Equity)adjusted;
}

static inline void klv_write(const KLV *klv, const char *data_paths,
                             const char *klv_name, ErrorStack *error_stack) {
  char *klv_filename = data_filepaths_get_writable_filename(
      data_paths, klv_name, DATA_FILEPATH_TYPE_KLV, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  // Open the file stream for writing
  FILE *stream = fopen_safe(klv_filename, "wb", error_stack);
  free(klv_filename);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  const uint32_t kwg_number_of_nodes = kwg_get_number_of_nodes(klv->kwg);

  uint32_t kwg_size = htole32(kwg_number_of_nodes);
  fwrite_or_die(&kwg_size, sizeof(kwg_size), 1, stream, "kwg size");

  uint32_t *le_nodes =
      (uint32_t *)malloc_or_die(kwg_number_of_nodes * sizeof(uint32_t));
  for (uint32_t i = 0; i < kwg_number_of_nodes; i++) {
    le_nodes[i] = htole32(kwg_node(klv->kwg, i));
  }

  // Write the nodes to the stream
  fwrite_or_die(le_nodes, sizeof(uint32_t), kwg_number_of_nodes, stream,
                "kwg nodes");

  // Free the temporary buffer
  free(le_nodes);

  uint32_t number_of_leaves = klv->number_of_leaves;
  uint32_t le_number_of_leaves = htole32(number_of_leaves);
  fwrite_or_die(&le_number_of_leaves, sizeof(le_number_of_leaves), 1, stream,
                "number of leaves");

  float *le_floats = (float *)malloc_or_die(number_of_leaves * sizeof(float));
  for (uint32_t i = 0; i < number_of_leaves; i++) {
    const double leave_double = equity_to_double(klv->leave_values[i]);
    le_floats[i] = convert_float_to_le((float)leave_double);
  }
  fwrite_or_die(le_floats, sizeof(float), number_of_leaves, stream,
                "leave values");
  free(le_floats);

  fclose_or_die(stream);
}

#endif
