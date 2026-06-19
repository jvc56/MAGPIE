#ifndef DAWG_PACKED_H
#define DAWG_PACKED_H

#include "../def/kwg_defs.h"
#include "../util/io_util.h"
#include "dictionary_word.h"
#include "kwg.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// A "packed DAWG" is a niche, opt-in serialization of a DAWG-only KWG whose
// 32-bit nodes have been re-encoded into the minimum number of bits each node
// actually needs. It is intended for fitting a full word list onto retro
// hardware, NOT for mainline MAGPIE: the regular 32-bit KWG remains the default
// everywhere and is produced unchanged.
//
// Each node carries the same four fields as a KWG node -- tile, accepts,
// is_end, and a first-child arc index -- but the arc index is widened only to
// cover the real node count and the tile only to cover the lexicon's largest
// machine letter. The fields are laid out low-to-high exactly like the 32-bit
// KWG layout (arc, then is_end, then accepts, then tile) so that a decoded
// node can be handed straight to the kwg_node_* accessors.
//
// The serializer picks one of two storage strategies and records the choice in
// the header so the reader stays trivial:
//   * bit-packed: node i begins at bit i * stored_width (smallest output);
//   * byte-aligned: stored_width is rounded up to a whole number of bytes so
//     node i begins at byte i * (stored_width / 8) -- a few wasted bits in
//     exchange for decoding that costs no cross-byte shifting (cheap on 8-bit
//     CPUs).
//
// The node array is a bit-stream packed LSB-first into bytes, so it is
// identical on any CPU. The few multi-byte header fields are written in the
// machine's native byte order: a packed DAWG reads back correctly on a machine
// of the same endianness (the intended use), but is not portable across
// endiannesses -- which is fine, since it never needs to be transported.
//
// Because child order within a node's list is preserved verbatim from the
// source KWG, a packed DAWG built from a KWG_MAKER_MERGE_TAIL_REORDER DAWG
// inherits that DAWG's caveat: it is valid for linear-scan readers and word
// lookup, but NOT for the Alpha cross-set path.

enum {
  DAWG_PACKED_VERSION = 1,
  // 4 magic + 1 version + 1 tile_bits + 1 arc_bits + 1 stored_width + 1 flags +
  // 3 pad + 4 node_count + 4 root_index.
  DAWG_PACKED_HEADER_BYTES = 20,
  DAWG_PACKED_FLAG_BYTE_ALIGNED = 0x1,
};

#define DAWG_PACKED_MAGIC "PDWG"

typedef struct DawgPacked {
  uint8_t *node_bits;
  size_t node_bytes;
  uint32_t node_count;
  uint32_t root_index;
  uint8_t tile_bits;
  uint8_t arc_bits;
  uint8_t stored_width;
  bool byte_aligned;
} DawgPacked;

// Reads nbits (<= 32) starting at bit_off from a little-bit-endian (LSB-first
// within each byte) packed array.
static inline uint32_t dawg_packed_bits_read(const uint8_t *bits,
                                             size_t bit_off, uint8_t nbits) {
  uint32_t result = 0;
  for (uint8_t bit_idx = 0; bit_idx < nbits; bit_idx++) {
    const size_t pos = bit_off + bit_idx;
    const uint32_t bit = (bits[pos >> 3] >> (pos & 7)) & 1U;
    result |= bit << bit_idx;
  }
  return result;
}

static inline void dawg_packed_bits_write(uint8_t *bits, size_t bit_off,
                                          uint8_t nbits, uint32_t value) {
  for (uint8_t bit_idx = 0; bit_idx < nbits; bit_idx++) {
    const size_t pos = bit_off + bit_idx;
    const uint32_t bit = (value >> bit_idx) & 1U;
    bits[pos >> 3] =
        (uint8_t)((bits[pos >> 3] & ~(1U << (pos & 7))) | (bit << (pos & 7)));
  }
}

// Decodes packed node node_index back into a 32-bit value using the standard
// KWG node layout, so kwg_node_is_end / kwg_node_accepts / kwg_node_arc_index /
// kwg_node_tile all apply.
static inline uint32_t dawg_packed_get_node(const DawgPacked *dp,
                                            uint32_t node_index) {
  const size_t bit_off = (size_t)node_index * dp->stored_width;
  const uint8_t raw_width = (uint8_t)(dp->arc_bits + 2 + dp->tile_bits);
  const uint32_t value =
      dawg_packed_bits_read(dp->node_bits, bit_off, raw_width);
  // 64-bit shift keeps the mask well-defined even for an out-of-range
  // arc_bits; dawg_packed_read_from_file additionally rejects such headers.
  const uint32_t arc = value & (uint32_t)(((uint64_t)1 << dp->arc_bits) - 1U);
  const uint32_t is_end = (value >> dp->arc_bits) & 1U;
  const uint32_t accepts = (value >> (dp->arc_bits + 1)) & 1U;
  const uint32_t tile = value >> (dp->arc_bits + 2);
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

static inline uint32_t dawg_packed_get_node_count(const DawgPacked *dp) {
  return dp->node_count;
}

static inline uint32_t dawg_packed_get_root_index(const DawgPacked *dp) {
  return dp->root_index;
}

static inline size_t dawg_packed_get_node_bytes(const DawgPacked *dp) {
  return dp->node_bytes;
}

void dawg_packed_destroy(DawgPacked *dp);

// Builds a packed DAWG from a DAWG-only KWG. When prefer_byte_alignment is
// true, stored_width is rounded up to a whole number of bytes (whole-byte
// nodes, cheaper to decode on 8-bit CPUs); when false, nodes are bit-packed at
// the minimal width.
DawgPacked *dawg_packed_create_from_kwg(const KWG *kwg,
                                        bool prefer_byte_alignment);

void dawg_packed_write_to_file(const DawgPacked *dp, const char *filename,
                               ErrorStack *error_stack);

DawgPacked *dawg_packed_read_from_file(const char *filename,
                                       ErrorStack *error_stack);

// Appends every word reachable from the packed DAWG's root to words.
void dawg_packed_write_words(const DawgPacked *dp, DictionaryWordList *words);

#endif
