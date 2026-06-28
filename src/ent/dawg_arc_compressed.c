#include "dawg_arc_compressed.h"

#include "../def/board_defs.h"
#include "../util/fileproxy.h"
#include "../util/io_util.h"
#include "dawg_packed.h"
#include "dictionary_word.h"
#include "kwg.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  // Smallest subfield width considered by the config search; below this the
  // popular table cannot index a useful number of targets.
  DAWG_ARC_COMPRESSED_MIN_FIELD = 4,
  DAWG_ARC_COMPRESSED_MAX_TILE_BITS = 8,
  DAWG_ARC_COMPRESSED_MAX_ARC_BITS = 22,
  // The config-search directory-size estimate charges one 32-bit counter per
  // this many nodes (a rough proxy; the real directory keys on RANK_BLOCK).
  DAWG_ARC_COMPRESSED_CONFIG_DIRECTORY_STRIDE = 256,
  // BALANCED mode picks the smallest config whose escape rate is at or below
  // this percent of nodes (a knee on the size/speed curve, still smaller than
  // the fixed-width DAWG); MIN_RAM ignores it and just minimizes total bytes.
  DAWG_ARC_COMPRESSED_BALANCED_ESCAPE_PCT = 15,
};

// Candidate popular-table sizes; a candidate is usable only when it is both
// indexable by the subfield (K <= 2^field) and backed by enough targets.
static const uint32_t popular_candidates[] = {64,   128,  256,  512,  1024,
                                              2048, 4096, 8192, 16384};
enum { DAWG_ARC_COMPRESSED_NUM_POPULAR_CANDIDATES = 9 };

// A DAWG node after state-DFS renumbering: arc is the renumbered first-child
// index (0 = no child).
typedef struct DawgArcCompressedNode {
  MachineLetter tile;
  bool accepts;
  bool is_end;
  uint32_t arc;
} DawgArcCompressedNode;

// (in-degree, index) pair used to rank targets for the popular table.
typedef struct DawgArcCompressedRankEntry {
  uint32_t in_degree;
  uint32_t index;
} DawgArcCompressedRankEntry;

// Sorts by descending in-degree, breaking ties by ascending node index so the
// ranking is deterministic.
static int dawg_arc_compressed_rank_compare(const void *left,
                                            const void *right) {
  const DawgArcCompressedRankEntry *left_entry =
      (const DawgArcCompressedRankEntry *)left;
  const DawgArcCompressedRankEntry *right_entry =
      (const DawgArcCompressedRankEntry *)right;
  if (left_entry->in_degree != right_entry->in_degree) {
    return (left_entry->in_degree < right_entry->in_degree) ? 1 : -1;
  }
  if (left_entry->index != right_entry->index) {
    return (left_entry->index < right_entry->index) ? -1 : 1;
  }
  return 0;
}

// Number of bits needed to represent every value in [0, max_value].
static inline uint8_t bits_needed(uint32_t max_value) {
  uint8_t bits = 1;
  while ((max_value >> bits) != 0) {
    bits++;
  }
  return bits;
}

// zig-zag signed->unsigned gap map (WebGraph's int2nat; Boldi & Vigna 2004).
static inline uint64_t zigzag(int64_t value) {
  return (value >= 0) ? ((uint64_t)value << 1)
                      : (((uint64_t)(-value) << 1) - 1U);
}

// Renumbers the DAWG reachable from the KWG's dawg root into state-DFS order
// (sibling runs kept contiguous so most gaps are small), reserving renumbered
// index 0 as a no-child sentinel. Returns a freshly allocated node array of
// length *out_count with the root at *out_root. The KWG is read only.
static DawgArcCompressedNode *dawg_arc_compressed_renumber(const KWG *kwg,
                                                           uint32_t *out_count,
                                                           uint32_t *out_root) {
  const uint32_t old_count = (uint32_t)kwg_get_number_of_nodes(kwg);
  const uint32_t root = kwg_get_dawg_root_node_index(kwg);

  // run_start[old_idx] = first index of the sibling run containing old_idx.
  uint32_t *run_start = (uint32_t *)malloc_or_die(sizeof(uint32_t) * old_count);
  uint32_t current_run = 0;
  for (uint32_t old_idx = 0; old_idx < old_count; old_idx++) {
    if (old_idx == 0 || kwg_node_is_end(kwg_node(kwg, old_idx - 1))) {
      current_run = old_idx;
    }
    run_start[old_idx] = current_run;
  }

  uint32_t *new_of = (uint32_t *)calloc_or_die(old_count, sizeof(uint32_t));
  uint32_t *order = (uint32_t *)malloc_or_die(sizeof(uint32_t) * old_count);
  uint32_t order_count = 0;
  uint8_t *visited = (uint8_t *)calloc_or_die((old_count + 7) / 8, 1);
  // Total pushes over the whole DFS are bounded by the arc count (<=
  // old_count).
  uint32_t *stack =
      (uint32_t *)malloc_or_die(sizeof(uint32_t) * (old_count + 1));
  uint32_t stack_top = 0;
  uint32_t *kids = (uint32_t *)malloc_or_die(sizeof(uint32_t) * old_count);

  stack[stack_top++] = run_start[root];
  while (stack_top > 0) {
    const uint32_t run = stack[--stack_top];
    if (((visited[run >> 3] >> (run & 7)) & 1U) != 0) {
      continue;
    }
    visited[run >> 3] |= (uint8_t)(1U << (run & 7));
    uint32_t kids_count = 0;
    uint32_t node_idx = run;
    for (;;) {
      new_of[node_idx] = order_count + 1;
      order[order_count++] = node_idx;
      const uint32_t node = kwg_node(kwg, node_idx);
      const uint32_t arc = kwg_node_arc_index(node);
      if (arc != 0) {
        const uint32_t child_run = run_start[arc];
        if (((visited[child_run >> 3] >> (child_run & 7)) & 1U) == 0) {
          kids[kids_count++] = child_run;
        }
      }
      if (kwg_node_is_end(node)) {
        break;
      }
      node_idx++;
    }
    // Push in reverse so the first child's run is processed first (DFS order).
    for (uint32_t kid_idx = kids_count; kid_idx-- > 0;) {
      stack[stack_top++] = kids[kid_idx];
    }
  }

  const uint32_t new_count = order_count + 1;
  DawgArcCompressedNode *nodes = (DawgArcCompressedNode *)malloc_or_die(
      sizeof(DawgArcCompressedNode) * new_count);
  nodes[0].tile = 0;
  nodes[0].accepts = false;
  nodes[0].is_end = true;
  nodes[0].arc = 0;
  for (uint32_t new_idx = 1; new_idx < new_count; new_idx++) {
    const uint32_t old_idx = order[new_idx - 1];
    const uint32_t node = kwg_node(kwg, old_idx);
    const uint32_t arc = kwg_node_arc_index(node);
    nodes[new_idx].tile = (MachineLetter)kwg_node_tile(node);
    nodes[new_idx].accepts = kwg_node_accepts(node);
    nodes[new_idx].is_end = kwg_node_is_end(node);
    nodes[new_idx].arc = (arc != 0) ? new_of[arc] : 0;
  }
  *out_root = new_of[root];
  *out_count = new_count;

  free(run_start);
  free(new_of);
  free(order);
  free(visited);
  free(stack);
  free(kids);
  return nodes;
}

// Chooses the (field, popular_count) pair that minimizes the total resident
// size: fixed-width records + popular table + escape side array + an estimate
// of the rank directory. rank_pos[node] is the node's position in the
// in-degree ranking (UINT32_MAX if it is never a target).
static void dawg_arc_compressed_best_config(
    const DawgArcCompressedNode *nodes, uint32_t node_count,
    const uint32_t *rank_pos, uint32_t ranked_count, uint8_t tile_bits,
    uint8_t arc_bits, dawg_arc_compressed_mode_t mode, uint8_t *out_field,
    uint32_t *out_popular_count) {
  const uint32_t directory_estimate =
      ((node_count + DAWG_ARC_COMPRESSED_CONFIG_DIRECTORY_STRIDE - 1) /
       DAWG_ARC_COMPRESSED_CONFIG_DIRECTORY_STRIDE) *
      32;
  const bool balanced = mode == DAWG_ARC_COMPRESSED_MODE_BALANCED;
  uint64_t best_total = UINT64_MAX;
  uint8_t best_field = DAWG_ARC_COMPRESSED_MIN_FIELD;
  uint32_t best_popular_count = 0;
  // BALANCED tracks the smallest config whose escape rate is within the
  // ceiling.
  uint64_t best_balanced_total = UINT64_MAX;
  uint8_t balanced_field = DAWG_ARC_COMPRESSED_MIN_FIELD;
  uint32_t balanced_popular_count = 0;
  for (uint8_t field = DAWG_ARC_COMPRESSED_MIN_FIELD; field <= arc_bits;
       field++) {
    const uint32_t sentinel = (1U << field) - 1U;
    for (int cand_idx = 0;
         cand_idx < DAWG_ARC_COMPRESSED_NUM_POPULAR_CANDIDATES; cand_idx++) {
      const uint32_t popular_count = popular_candidates[cand_idx];
      if (popular_count > (1U << field) || popular_count > ranked_count) {
        continue;
      }
      uint32_t escape_count = 0;
      for (uint32_t node_idx = 0; node_idx < node_count; node_idx++) {
        const uint32_t arc = nodes[node_idx].arc;
        if (arc == 0 || rank_pos[arc] < popular_count) {
          continue;
        }
        const uint64_t zigzagged = zigzag((int64_t)arc - (int64_t)node_idx);
        if (zigzagged < 1 || zigzagged > sentinel - 1) {
          escape_count++;
        }
      }
      const uint64_t record_bits =
          (uint64_t)node_count * (tile_bits + 3 + field);
      const uint64_t total = record_bits + (uint64_t)popular_count * arc_bits +
                             (uint64_t)escape_count * arc_bits +
                             directory_estimate;
      if (total < best_total) {
        best_total = total;
        best_field = field;
        best_popular_count = popular_count;
      }
      if ((uint64_t)escape_count * 100 <=
              (uint64_t)node_count * DAWG_ARC_COMPRESSED_BALANCED_ESCAPE_PCT &&
          total < best_balanced_total) {
        best_balanced_total = total;
        balanced_field = field;
        balanced_popular_count = popular_count;
      }
    }
  }
  // BALANCED uses the within-ceiling minimum when one exists; otherwise (e.g. a
  // tiny lexicon where even the widest config escapes a lot) falls back to the
  // overall MIN_RAM minimum.
  if (balanced && best_balanced_total != UINT64_MAX) {
    *out_field = balanced_field;
    *out_popular_count = balanced_popular_count;
  } else {
    *out_field = best_field;
    *out_popular_count = best_popular_count;
  }
}

// Fills the cached section offsets and num_bytes from the header fields, which
// must already be set on dp. Shared by create and read so the layout has a
// single source of truth.
static void dawg_arc_compressed_compute_offsets(DawgArcCompressed *dp) {
  const size_t header_bits = (size_t)DAWG_ARC_COMPRESSED_HEADER_BYTES * 8;
  dp->popular_bit_off = header_bits;
  size_t pos = header_bits + (size_t)dp->popular_count * dp->arc_bits;
  pos = (pos + 7) & ~(size_t)7;
  dp->escape_bit_off = pos;
  pos += (size_t)dp->escape_count * dp->arc_bits;
  pos = (pos + 7) & ~(size_t)7;
  dp->bitmap_byte_off = pos >> 3;
  const size_t bitmap_bytes = ((size_t)dp->node_count + 7) / 8;
  pos += bitmap_bytes * 8;
  dp->directory_bit_off = pos;
  const size_t num_blocks =
      ((size_t)dp->node_count + DAWG_ARC_COMPRESSED_RANK_BLOCK - 1) /
      DAWG_ARC_COMPRESSED_RANK_BLOCK;
  pos += (num_blocks + 1) * 32;
  dp->record_bit_off = pos;
  pos += (size_t)dp->node_count * dp->rec_width;
  dp->num_bytes = (pos + 7) / 8;
}

DawgArcCompressed *
dawg_arc_compressed_create_from_kwg(const KWG *kwg,
                                    dawg_arc_compressed_mode_t mode) {
  uint32_t node_count = 0;
  uint32_t root = 0;
  DawgArcCompressedNode *nodes =
      dawg_arc_compressed_renumber(kwg, &node_count, &root);

  uint32_t max_tile = 0;
  for (uint32_t node_idx = 0; node_idx < node_count; node_idx++) {
    if (nodes[node_idx].tile > max_tile) {
      max_tile = nodes[node_idx].tile;
    }
  }
  const uint8_t tile_bits = bits_needed(max_tile);
  const uint8_t arc_bits = bits_needed(node_count - 1);

  // Rank targets by in-degree; rank_pos[node] is its position in that ranking.
  uint32_t *in_degree = (uint32_t *)calloc_or_die(node_count, sizeof(uint32_t));
  for (uint32_t node_idx = 0; node_idx < node_count; node_idx++) {
    const uint32_t arc = nodes[node_idx].arc;
    if (arc != 0) {
      in_degree[arc]++;
    }
  }
  DawgArcCompressedRankEntry *ranked =
      (DawgArcCompressedRankEntry *)malloc_or_die(
          sizeof(DawgArcCompressedRankEntry) * node_count);
  uint32_t ranked_count = 0;
  for (uint32_t node_idx = 0; node_idx < node_count; node_idx++) {
    if (in_degree[node_idx] > 0) {
      ranked[ranked_count].in_degree = in_degree[node_idx];
      ranked[ranked_count].index = node_idx;
      ranked_count++;
    }
  }
  qsort(ranked, ranked_count, sizeof(DawgArcCompressedRankEntry),
        dawg_arc_compressed_rank_compare);
  uint32_t *rank_pos = (uint32_t *)malloc_or_die(sizeof(uint32_t) * node_count);
  for (uint32_t node_idx = 0; node_idx < node_count; node_idx++) {
    rank_pos[node_idx] = UINT32_MAX;
  }
  for (uint32_t rank_idx = 0; rank_idx < ranked_count; rank_idx++) {
    rank_pos[ranked[rank_idx].index] = rank_idx;
  }

  uint8_t field = DAWG_ARC_COMPRESSED_MIN_FIELD;
  uint32_t popular_count = 0;
  dawg_arc_compressed_best_config(nodes, node_count, rank_pos, ranked_count,
                                  tile_bits, arc_bits, mode, &field,
                                  &popular_count);
  const uint32_t sentinel = (1U << field) - 1U;
  const uint8_t rec_width = (uint8_t)(tile_bits + 3 + field);

  // Classify every arc into the four encodings, collecting the escape targets
  // (in node order) and the escape bit-map.
  uint32_t *subfield = (uint32_t *)malloc_or_die(sizeof(uint32_t) * node_count);
  uint8_t *flag = (uint8_t *)calloc_or_die(node_count, 1);
  uint8_t *escape_bitmap = (uint8_t *)calloc_or_die((node_count + 7) / 8, 1);
  uint32_t *escapes = (uint32_t *)malloc_or_die(sizeof(uint32_t) * node_count);
  uint32_t escape_count = 0;
  for (uint32_t node_idx = 0; node_idx < node_count; node_idx++) {
    const uint32_t arc = nodes[node_idx].arc;
    if (arc == 0) {
      subfield[node_idx] = 0;
    } else if (rank_pos[arc] < popular_count) {
      subfield[node_idx] = rank_pos[arc];
      flag[node_idx] = 1;
    } else {
      const uint64_t zigzagged = zigzag((int64_t)arc - (int64_t)node_idx);
      if (zigzagged >= 1 && zigzagged <= sentinel - 1) {
        subfield[node_idx] = (uint32_t)zigzagged;
      } else {
        subfield[node_idx] = sentinel;
        escape_bitmap[node_idx >> 3] |= (uint8_t)(1U << (node_idx & 7));
        escapes[escape_count++] = arc;
      }
    }
  }

  DawgArcCompressed *dp =
      (DawgArcCompressed *)malloc_or_die(sizeof(DawgArcCompressed));
  dp->node_count = node_count;
  dp->root_index = root;
  dp->popular_count = popular_count;
  dp->escape_count = escape_count;
  dp->tile_bits = tile_bits;
  dp->arc_bits = arc_bits;
  dp->rec_width = rec_width;
  dp->field = field;
  dawg_arc_compressed_compute_offsets(dp);

  uint8_t *bytes = (uint8_t *)calloc_or_die(dp->num_bytes, 1);
  // Header. Write the magic byte-by-byte (memcpy from a string literal trips
  // bugprone-not-null-terminated-result on this binary tag).
  for (int magic_idx = 0; magic_idx < 4; magic_idx++) {
    bytes[magic_idx] = (uint8_t)DAWG_ARC_COMPRESSED_MAGIC[magic_idx];
  }
  bytes[4] = DAWG_ARC_COMPRESSED_VERSION;
  bytes[5] = tile_bits;
  bytes[6] = arc_bits;
  bytes[7] = rec_width;
  bytes[8] = field;
  // bytes[9..11] reserved padding (already zero). Multi-byte fields use native
  // byte order: an arc-compressed DAWG is read back on the same machine.
  memcpy(bytes + 12, &node_count, sizeof(uint32_t));
  memcpy(bytes + 16, &root, sizeof(uint32_t));
  memcpy(bytes + 20, &popular_count, sizeof(uint32_t));
  memcpy(bytes + 24, &escape_count, sizeof(uint32_t));

  // Popular table.
  for (uint32_t pop_idx = 0; pop_idx < popular_count; pop_idx++) {
    dawg_packed_bits_write(bytes,
                           dp->popular_bit_off + (size_t)pop_idx * arc_bits,
                           arc_bits, ranked[pop_idx].index);
  }
  // Escape side array.
  for (uint32_t escape_idx = 0; escape_idx < escape_count; escape_idx++) {
    dawg_packed_bits_write(bytes,
                           dp->escape_bit_off + (size_t)escape_idx * arc_bits,
                           arc_bits, escapes[escape_idx]);
  }
  // Escape bit-map.
  memcpy(bytes + dp->bitmap_byte_off, escape_bitmap, (node_count + 7) / 8);
  // Rank directory: directory[block] = cumulative escape count before the
  // block; directory[num_blocks] = total.
  const uint32_t num_blocks =
      (node_count + DAWG_ARC_COMPRESSED_RANK_BLOCK - 1) /
      DAWG_ARC_COMPRESSED_RANK_BLOCK;
  uint32_t cumulative = 0;
  for (uint32_t block_idx = 0; block_idx < num_blocks; block_idx++) {
    dawg_packed_bits_write(
        bytes, dp->directory_bit_off + (size_t)block_idx * 32, 32, cumulative);
    uint32_t block_end = (block_idx + 1) * DAWG_ARC_COMPRESSED_RANK_BLOCK;
    if (block_end > node_count) {
      block_end = node_count;
    }
    for (uint32_t node_idx = block_idx * DAWG_ARC_COMPRESSED_RANK_BLOCK;
         node_idx < block_end; node_idx++) {
      if (((escape_bitmap[node_idx >> 3] >> (node_idx & 7)) & 1U) != 0) {
        cumulative++;
      }
    }
  }
  dawg_packed_bits_write(bytes, dp->directory_bit_off + (size_t)num_blocks * 32,
                         32, cumulative);
  // Records.
  for (uint32_t node_idx = 0; node_idx < node_count; node_idx++) {
    const uint32_t value =
        subfield[node_idx] | ((uint32_t)flag[node_idx] << field) |
        ((uint32_t)(nodes[node_idx].is_end ? 1U : 0U) << (field + 1)) |
        ((uint32_t)(nodes[node_idx].accepts ? 1U : 0U) << (field + 2)) |
        ((uint32_t)nodes[node_idx].tile << (field + 3));
    dawg_packed_bits_write(bytes,
                           dp->record_bit_off + (size_t)node_idx * rec_width,
                           rec_width, value);
  }
  dp->bytes = bytes;

  free(nodes);
  free(in_degree);
  free(ranked);
  free(rank_pos);
  free(subfield);
  free(flag);
  free(escape_bitmap);
  free(escapes);
  return dp;
}

void dawg_arc_compressed_destroy(DawgArcCompressed *dp) {
  if (dp == NULL) {
    return;
  }
  free(dp->bytes);
  free(dp);
}

void dawg_arc_compressed_write_to_file(const DawgArcCompressed *dp,
                                       const char *filename,
                                       ErrorStack *error_stack) {
  FILE *stream = fopen_safe(filename, "wb", error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
  fwrite_or_die(dp->bytes, 1, dp->num_bytes, stream, "arc-compressed dawg");
  fclose_or_die(stream);
}

DawgArcCompressed *dawg_arc_compressed_read_from_file(const char *filename,
                                                      ErrorStack *error_stack) {
  FILE *stream = stream_from_filename(filename, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return NULL;
  }
  uint8_t header[DAWG_ARC_COMPRESSED_HEADER_BYTES];
  if (fread(header, 1, sizeof(header), stream) != sizeof(header) ||
      memcmp(header, DAWG_ARC_COMPRESSED_MAGIC, 4) != 0 ||
      header[4] != DAWG_ARC_COMPRESSED_VERSION) {
    fclose_or_die(stream);
    error_stack_push(
        error_stack, ERROR_STATUS_CONVERT_MALFORMED_KWG,
        get_formatted_string("malformed arc-compressed dawg header in file: %s",
                             filename));
    return NULL;
  }
  DawgArcCompressed *dp =
      (DawgArcCompressed *)malloc_or_die(sizeof(DawgArcCompressed));
  dp->tile_bits = header[5];
  dp->arc_bits = header[6];
  dp->rec_width = header[7];
  dp->field = header[8];
  memcpy(&dp->node_count, header + 12, sizeof(uint32_t));
  memcpy(&dp->root_index, header + 16, sizeof(uint32_t));
  memcpy(&dp->popular_count, header + 20, sizeof(uint32_t));
  memcpy(&dp->escape_count, header + 24, sizeof(uint32_t));

  // Reject corrupt/malicious headers before trusting the fields for sizing,
  // allocation, and bit-shift widths.
  const uint8_t expected_rec_width = (uint8_t)(dp->tile_bits + 3 + dp->field);
  if (dp->tile_bits < 1 || dp->tile_bits > DAWG_ARC_COMPRESSED_MAX_TILE_BITS ||
      dp->arc_bits < 1 || dp->arc_bits > DAWG_ARC_COMPRESSED_MAX_ARC_BITS ||
      dp->field < 1 || dp->field >= dp->arc_bits ||
      dp->rec_width != expected_rec_width || dp->node_count == 0 ||
      dp->node_count > (1U << dp->arc_bits) ||
      dp->root_index >= dp->node_count ||
      dp->popular_count > (1U << dp->field) ||
      dp->escape_count > dp->node_count) {
    fclose_or_die(stream);
    free(dp);
    error_stack_push(
        error_stack, ERROR_STATUS_CONVERT_MALFORMED_KWG,
        get_formatted_string(
            "invalid arc-compressed dawg header fields in file: %s", filename));
    return NULL;
  }

  dawg_arc_compressed_compute_offsets(dp);
  dp->bytes = (uint8_t *)malloc_or_die(dp->num_bytes);
  memcpy(dp->bytes, header, sizeof(header));
  const size_t body_bytes = dp->num_bytes - sizeof(header);
  if (fread(dp->bytes + sizeof(header), 1, body_bytes, stream) != body_bytes) {
    fclose_or_die(stream);
    dawg_arc_compressed_destroy(dp);
    error_stack_push(
        error_stack, ERROR_STATUS_CONVERT_MALFORMED_KWG,
        get_formatted_string("truncated arc-compressed dawg data in file: %s",
                             filename));
    return NULL;
  }
  fclose_or_die(stream);
  return dp;
}

static void dawg_arc_compressed_write_words_aux(const DawgArcCompressed *dp,
                                                uint32_t node_index,
                                                MachineLetter *prefix,
                                                int prefix_length, bool accepts,
                                                DictionaryWordList *words) {
  if (accepts) {
    dictionary_word_list_add_word(words, prefix, prefix_length);
  }
  if (node_index == 0) {
    return;
  }
  for (uint32_t node_idx = node_index;; node_idx++) {
    const uint32_t node = dawg_arc_compressed_get_node(dp, node_idx);
    const MachineLetter ml = (MachineLetter)kwg_node_tile(node);
    const uint32_t arc = kwg_node_arc_index(node);
    const bool node_accepts = kwg_node_accepts(node);
    if (prefix_length < BOARD_DIM) {
      prefix[prefix_length] = ml;
    }
    dawg_arc_compressed_write_words_aux(dp, arc, prefix, prefix_length + 1,
                                        node_accepts, words);
    if (kwg_node_is_end(node)) {
      break;
    }
  }
}

void dawg_arc_compressed_write_words(const DawgArcCompressed *dp,
                                     DictionaryWordList *words) {
  MachineLetter prefix[BOARD_DIM];
  dawg_arc_compressed_write_words_aux(dp, dp->root_index, prefix, 0, false,
                                      words);
}
