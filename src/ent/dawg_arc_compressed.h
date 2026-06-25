#ifndef DAWG_ARC_COMPRESSED_H
#define DAWG_ARC_COMPRESSED_H

#include "../def/kwg_defs.h"
#include "../util/io_util.h"
#include "dawg_packed.h"
#include "dictionary_word.h"
#include "kwg.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// An "arc-compressed DAWG" is a niche, opt-in serialization of a DAWG-only KWG
// that shrinks the in-RAM footprint further than the packed DAWG (.pdawg) by
// compressing the one field with real slack: the first-child arc. Like the
// packed DAWG it is intended for fitting a full word list onto retro hardware,
// NOT for mainline MAGPIE; the regular 32-bit KWG remains the default
// everywhere and is produced unchanged, and nothing here touches kwg_maker or
// the gaddag path that wordprune depends on.
//
// Records stay FIXED-WIDTH (node j lives at a known bit offset), so the graph
// is still randomly addressable and traversable in place. Each record carries
// the same tile / accepts / is_end fields as a KWG node, plus a 1-bit flag and
// a field-bit subfield that together encode the arc four ways:
//   * flag = 1                  -> subfield indexes a "popular" table of the K
//                                  highest in-degree target nodes;
//   * flag = 0, subfield = 0    -> no child (leaf);
//   * flag = 0, subfield = max  -> escape: the full target lives in a side
//                                  array, located by rank over an escape
//                                  bit-map (a one-level rank directory,
//                                  Jacobson 1989 sec 5.1);
//   * flag = 0, otherwise       -> target = j + unzigzag(subfield), a local
//                                  signed gap to a nearby node.
// Nodes are renumbered in state-DFS order (sibling runs kept contiguous) so
// most arcs point near their source and the gap stays small. See
// tools/RETRO_DAWG.md for the full study and the prior-art attribution.
//
// Layout (all bit fields are LSB-first within bytes, identical on any CPU; the
// few multi-byte header fields use the writer's native byte order, so an
// arc-compressed DAWG reads back on a machine of the same endianness):
//   header | popular | escapes | escape bit-map | rank directory | records
//
// Like the packed DAWG, child order within a node's list is preserved, so an
// arc-compressed DAWG built from a KWG_MAKER_MERGE_TAIL_REORDER DAWG inherits
// that DAWG's caveat: valid for linear-scan readers and word lookup, NOT for
// the Alpha cross-set path.

enum {
  // On-disk format version (started at 2; version 1 was never shipped).
  DAWG_ARC_COMPRESSED_VERSION = 2,
  // 4 magic + 1 version + 1 tile_bits + 1 arc_bits + 1 rec_width + 1 field +
  // 3 pad + 4 node_count + 4 root_index + 4 popular_count + 4 escape_count.
  DAWG_ARC_COMPRESSED_HEADER_BYTES = 32,
  // The escape rank directory stores one cumulative escape count per block of
  // this many nodes; the in-block remainder is finished with a population
  // count.
  DAWG_ARC_COMPRESSED_RANK_BLOCK = 64,
};

#define DAWG_ARC_COMPRESSED_MAGIC "ACDW"

// Build mode: which point on the size/speed curve to target. Both modes produce
// the identical on-disk format and the identical decoder; they differ only in
// the chosen (field, popular-table size K), and therefore in the escape rate.
// Escapes cost a popcount-rank + a side-array read on every *descended* node,
// so a lower escape rate traverses faster, not just larger -- the rate is both
// a RAM lever and a speed lever. Measured on CSW24, lazy/inlined traversal, vs
// the fixed-width packed DAWG:
//   MIN_RAM  -- 472.7 KB, ~33% escapes -> ~1.47x word-lookup, ~1.23x rack-gen.
//               Smallest possible footprint (the retro goal); the slow end.
//   BALANCED -- ~488 KB (still smaller than the packed DAWG), ~14% escapes ->
//               ~1.32x word-lookup, ~1.14x rack-gen. A little more RAM for
//               materially faster traversal; for lookup/move-gen-heavy use.
typedef enum {
  DAWG_ARC_COMPRESSED_MODE_MIN_RAM,
  DAWG_ARC_COMPRESSED_MODE_BALANCED,
} dawg_arc_compressed_mode_t;

typedef struct DawgArcCompressed {
  // The entire serialized blob (header through records), so writing the file is
  // a single fwrite and the decoder reads its tables straight out of memory
  // with zero load-time reconstruction.
  uint8_t *bytes;
  size_t num_bytes;
  uint32_t node_count;    // includes the reserved index-0 no-child sentinel
  uint32_t root_index;    // renumbered index of the DAWG root
  uint32_t popular_count; // K: entries in the popular table
  uint32_t escape_count;  // entries in the escape side array
  uint8_t tile_bits;
  uint8_t arc_bits;  // bits to hold any node index (popular/escape targets)
  uint8_t rec_width; // tile_bits + 3 + field
  uint8_t field;     // subfield width in bits
  // Cached section bit/byte offsets into bytes, derived from the header fields.
  size_t popular_bit_off;
  size_t escape_bit_off;
  size_t bitmap_byte_off;
  size_t directory_bit_off;
  size_t record_bit_off;
} DawgArcCompressed;

// rank over the escape bit-map: directory[block] + population count of the
// set escape bits in this node's block below node_index.
static inline uint32_t
dawg_arc_compressed_escape_rank(const DawgArcCompressed *dp,
                                uint32_t node_index) {
  const uint32_t block = node_index / DAWG_ARC_COMPRESSED_RANK_BLOCK;
  uint32_t rank = dawg_packed_bits_read(
      dp->bytes, dp->directory_bit_off + (size_t)block * 32, 32);
  const uint8_t *bitmap = dp->bytes + dp->bitmap_byte_off;
  const uint32_t block_start_byte =
      block * (DAWG_ARC_COMPRESSED_RANK_BLOCK / 8);
  const uint32_t end_byte = node_index >> 3;
  for (uint32_t byte_idx = block_start_byte; byte_idx < end_byte; byte_idx++) {
    rank += (uint32_t)__builtin_popcount(bitmap[byte_idx]);
  }
  rank += (uint32_t)__builtin_popcount(
      bitmap[end_byte] & (uint8_t)((1U << (node_index & 7)) - 1U));
  return rank;
}

// Decodes record node_index back into a 32-bit value using the standard KWG
// node layout, so kwg_node_is_end / kwg_node_accepts / kwg_node_arc_index /
// kwg_node_tile all apply.
static inline uint32_t dawg_arc_compressed_get_node(const DawgArcCompressed *dp,
                                                    uint32_t node_index) {
  const uint32_t record = dawg_packed_bits_read(
      dp->bytes, dp->record_bit_off + (size_t)node_index * dp->rec_width,
      dp->rec_width);
  const uint32_t sentinel = (1U << dp->field) - 1U;
  const uint32_t subfield = record & sentinel;
  const uint32_t flag = (record >> dp->field) & 1U;
  const uint32_t is_end = (record >> (dp->field + 1)) & 1U;
  const uint32_t accepts = (record >> (dp->field + 2)) & 1U;
  const uint32_t tile = record >> (dp->field + 3);
  uint32_t arc;
  if (flag != 0) {
    arc = dawg_packed_bits_read(
        dp->bytes, dp->popular_bit_off + (size_t)subfield * dp->arc_bits,
        dp->arc_bits);
  } else if (subfield == 0) {
    arc = 0;
  } else if (subfield == sentinel) {
    const uint32_t slot = dawg_arc_compressed_escape_rank(dp, node_index);
    arc = dawg_packed_bits_read(
        dp->bytes, dp->escape_bit_off + (size_t)slot * dp->arc_bits,
        dp->arc_bits);
  } else {
    // unzigzag: even -> +n/2, odd -> -(n+1)/2.
    const int64_t gap = (subfield & 1U) ? -(int64_t)((subfield + 1U) >> 1)
                                        : (int64_t)(subfield >> 1);
    arc = (uint32_t)((int64_t)node_index + gap);
  }
  uint32_t node = arc;
  if (is_end != 0) {
    node |= KWG_NODE_IS_END_FLAG;
  }
  if (accepts != 0) {
    node |= KWG_NODE_ACCEPTS_FLAG;
  }
  node |= tile << KWG_TILE_BIT_OFFSET;
  return node;
}

static inline uint32_t
dawg_arc_compressed_get_node_count(const DawgArcCompressed *dp) {
  return dp->node_count;
}

static inline uint32_t
dawg_arc_compressed_get_root_index(const DawgArcCompressed *dp) {
  return dp->root_index;
}

static inline size_t
dawg_arc_compressed_get_num_bytes(const DawgArcCompressed *dp) {
  return dp->num_bytes;
}

void dawg_arc_compressed_destroy(DawgArcCompressed *dp);

// Builds an arc-compressed DAWG from a DAWG-only KWG (read only; not modified).
// mode selects the size/speed operating point (see dawg_arc_compressed_mode_t).
DawgArcCompressed *
dawg_arc_compressed_create_from_kwg(const KWG *kwg,
                                    dawg_arc_compressed_mode_t mode);

void dawg_arc_compressed_write_to_file(const DawgArcCompressed *dp,
                                       const char *filename,
                                       ErrorStack *error_stack);

DawgArcCompressed *dawg_arc_compressed_read_from_file(const char *filename,
                                                      ErrorStack *error_stack);

// Appends every word reachable from the arc-compressed DAWG's root to words.
void dawg_arc_compressed_write_words(const DawgArcCompressed *dp,
                                     DictionaryWordList *words);

#endif
