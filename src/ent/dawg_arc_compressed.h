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
// the same tile / accepts / is_end fields as a KWG node plus a `field`-bit
// code whose single value space partitions four ways:
//   * code 0                    -> no child (leaf);
//   * 1 <= code <= K            -> code-1 indexes a "popular" table of the K
//                                  highest in-degree target nodes;
//   * K < code < escape_base    -> target = j + (code - K), a small positive
//                                  gap to a nearby later node (in the state-DFS
//                                  renumbering nearly all local arcs point
//                                  forward, so no code space is spent on
//                                  negative gaps -- backward arcs are either
//                                  popular or escape);
//   * code >= escape_base       -> escape: the full target lives in a side
//                                  array at slot escape_start(block) +
//                                  (code - escape_base), where the R reserved
//                                  top codes give the escape's index within
//                                  its own fixed-size node block and a small
//                                  two-level directory (32-bit superblock
//                                  anchors + 16-bit per-block deltas) gives
//                                  escape_start. No per-node bit-map and no
//                                  popcount rank at decode time.
// Nodes are renumbered in state-DFS order (sibling runs kept contiguous) so
// most arcs point a short distance forward and the gap window stays small.
// Gap coding after WebGraph (Boldi & Vigna 2004); popular-table escape coding
// after the common most-frequent-value dictionary trick.
//
// Layout (all bit fields are LSB-first within bytes, identical on any CPU; the
// few multi-byte header fields use the writer's native byte order, so an
// arc-compressed DAWG reads back on a machine of the same endianness):
//   header | popular | escapes | anchors | deltas | records
//
// Like the packed DAWG, child order within a node's list is preserved, so an
// arc-compressed DAWG built from a KWG_MAKER_MERGE_TAIL_REORDER DAWG inherits
// that DAWG's caveat: valid for linear-scan readers and word lookup, NOT for
// the Alpha cross-set path.

enum {
  // On-disk format version (2 = flag-bit + zigzag + bitmap rank; 3 = unified
  // code space + positive-only gaps + block-local escape indices).
  DAWG_ARC_COMPRESSED_VERSION = 3,
  // 4 magic + 1 version + 1 tile_bits + 1 arc_bits + 1 rec_width + 1 field +
  // 1 escape_reserve + 1 block_bits + 1 pad + 4 node_count + 4 root_index +
  // 4 popular_count + 4 escape_count.
  DAWG_ARC_COMPRESSED_HEADER_BYTES = 32,
  // Blocks per superblock in the two-level escape-start directory: a 32-bit
  // anchor per superblock plus a 16-bit delta per block. The largest possible
  // delta (every node in a superblock escaping) is 32 * 128 = 4096, so the
  // 16-bit delta never overflows for any supported block size.
  DAWG_ARC_COMPRESSED_SUPERBLOCK_BLOCKS = 32,
};

#define DAWG_ARC_COMPRESSED_MAGIC "ACDW"

// Build mode: which point on the size/speed curve to target. Both modes produce
// the identical on-disk format and the identical decoder; they differ only in
// the chosen (field, popular-table size K, block size), and therefore in the
// escape rate. An escape costs two directory reads plus a side-array read on
// every *descended* node, so a lower escape rate traverses faster, not just
// larger -- the rate is both a RAM lever and a speed lever. Measured on full
// CSW24 (acdawgbench, word lookup vs the bit-packed packed DAWG's 537.2 KB):
//   MIN_RAM  -- 451.9 KB, ~21% escapes -> ~1.85x word-lookup, ~1.28x
//               enumeration. Smallest possible footprint (the retro goal).
//   BALANCED -- 465.3 KB, <=15% escapes -> ~1.73x word-lookup. A little more
//               RAM for faster traversal; for lookup/move-gen-heavy use.
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
  uint32_t escape_base;   // first escape code: 2^field - escape_reserve
  uint8_t tile_bits;
  uint8_t arc_bits;  // bits to hold any node index (popular/escape targets)
  uint8_t rec_width; // tile_bits + 2 + field
  uint8_t field;     // code width in bits
  uint8_t escape_reserve; // R: codes reserved for block-local escape indices
  uint8_t block_bits;     // log2 of the escape-directory block size in nodes
  // Cached section bit offsets into bytes, derived from the header fields.
  size_t popular_bit_off;
  size_t escape_bit_off;
  size_t anchor_bit_off;
  size_t delta_bit_off;
  size_t record_bit_off;
} DawgArcCompressed;

// escape_start for a node's block: 32-bit superblock anchor + 16-bit delta.
static inline uint32_t
dawg_arc_compressed_escape_start(const DawgArcCompressed *dp,
                                 uint32_t node_index) {
  const uint32_t block = node_index >> dp->block_bits;
  const uint32_t superblock = block / DAWG_ARC_COMPRESSED_SUPERBLOCK_BLOCKS;
  const uint32_t anchor = dawg_packed_bits_read(
      dp->bytes, dp->anchor_bit_off + (size_t)superblock * 32, 32);
  const uint32_t delta = dawg_packed_bits_read(
      dp->bytes, dp->delta_bit_off + (size_t)block * 16, 16);
  return anchor + delta;
}

// Decodes record node_index back into a 32-bit value using the standard KWG
// node layout, so kwg_node_is_end / kwg_node_accepts / kwg_node_arc_index /
// kwg_node_tile all apply.
static inline uint32_t dawg_arc_compressed_get_node(const DawgArcCompressed *dp,
                                                    uint32_t node_index) {
  const uint32_t record = dawg_packed_bits_read(
      dp->bytes, dp->record_bit_off + (size_t)node_index * dp->rec_width,
      dp->rec_width);
  const uint32_t code = record & ((1U << dp->field) - 1U);
  const uint32_t is_end = (record >> dp->field) & 1U;
  const uint32_t accepts = (record >> (dp->field + 1)) & 1U;
  const uint32_t tile = record >> (dp->field + 2);
  uint32_t arc;
  if (code == 0) {
    arc = 0;
  } else if (code <= dp->popular_count) {
    arc = dawg_packed_bits_read(
        dp->bytes, dp->popular_bit_off + (size_t)(code - 1) * dp->arc_bits,
        dp->arc_bits);
  } else if (code < dp->escape_base) {
    arc = node_index + (code - dp->popular_count);
  } else {
    const uint32_t slot = dawg_arc_compressed_escape_start(dp, node_index) +
                          (code - dp->escape_base);
    arc = dawg_packed_bits_read(
        dp->bytes, dp->escape_bit_off + (size_t)slot * dp->arc_bits,
        dp->arc_bits);
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
