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
  // Smallest code width considered by the config search; below this the
  // popular table cannot index a useful number of targets.
  DAWG_ARC_COMPRESSED_MIN_FIELD = 4,
  DAWG_ARC_COMPRESSED_MAX_TILE_BITS = 8,
  DAWG_ARC_COMPRESSED_MAX_ARC_BITS = 22,
  DAWG_ARC_COMPRESSED_MAX_BLOCK_BITS = 7,
  // BALANCED mode picks the smallest config whose escape rate is at or below
  // this percent of nodes (a knee on the size/speed curve, still smaller than
  // the fixed-width DAWG); MIN_RAM ignores it and just minimizes total bytes.
  DAWG_ARC_COMPRESSED_BALANCED_ESCAPE_PCT = 15,
  // The local K refinement examines this many code values on each side of the
  // coarse-grid winner, at this step.
  DAWG_ARC_COMPRESSED_K_REFINE_SPAN = 192,
  DAWG_ARC_COMPRESSED_K_REFINE_STEP = 16,
};

// Coarse popular-table sizes for the config sweep; the winner's K is then
// refined locally at DAWG_ARC_COMPRESSED_K_REFINE_STEP granularity (the code
// space is a value-granular partition, so K is not restricted to powers of
// two).
static const uint32_t popular_candidates[] = {
    0,    64,   128,  192,  256,  384,  512,   768,  1024,
    1536, 2048, 3072, 4096, 6144, 8192, 12288, 16384};
enum { DAWG_ARC_COMPRESSED_NUM_POPULAR_CANDIDATES = 17 };

// Block sizes (log2) tried for the escape directory: smaller blocks need fewer
// reserved escape codes but more directory deltas.
static const uint8_t block_bits_candidates[] = {5, 6, 7};
enum { DAWG_ARC_COMPRESSED_NUM_BLOCK_CANDIDATES = 3 };

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

// One candidate configuration evaluated by the search, and its cost.
typedef struct DawgArcCompressedConfig {
  uint8_t field;
  uint8_t block_bits;
  uint32_t popular_count;
  uint32_t escape_reserve;
  uint32_t escape_count;
  uint64_t total_bits;
  bool feasible;
} DawgArcCompressedConfig;

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

// Directory bits for a node count at a block size: a 32-bit anchor per
// superblock plus a 16-bit delta per block.
static uint64_t dawg_arc_compressed_directory_bits(uint32_t node_count,
                                                   uint8_t block_bits) {
  const uint64_t blocks =
      ((uint64_t)node_count + (1ULL << block_bits) - 1) >> block_bits;
  const uint64_t superblocks =
      (blocks + DAWG_ARC_COMPRESSED_SUPERBLOCK_BLOCKS - 1) /
      DAWG_ARC_COMPRESSED_SUPERBLOCK_BLOCKS;
  return superblocks * 32 + blocks * 16;
}

// Evaluates one (field, K, block_bits) candidate: iterates the reserved escape
// code count R to a fixpoint (a larger R shrinks the gap window, which can only
// add escapes and grow the per-block max, so the iteration is monotone and
// converges within the block size), then prices the whole structure.
static DawgArcCompressedConfig
dawg_arc_compressed_evaluate(const DawgArcCompressedNode *nodes,
                             uint32_t node_count, const uint32_t *rank_pos,
                             uint8_t tile_bits, uint8_t arc_bits, uint8_t field,
                             uint32_t popular_count, uint8_t block_bits) {
  DawgArcCompressedConfig config;
  memset(&config, 0, sizeof(config));
  config.field = field;
  config.block_bits = block_bits;
  config.popular_count = popular_count;
  config.feasible = false;
  config.total_bits = UINT64_MAX;

  const uint32_t codes = 1U << field;
  const uint32_t block_size = 1U << block_bits;
  uint32_t reserve = 0;
  uint32_t escape_count = 0;
  for (;;) {
    if (popular_count + reserve + 1 > codes) {
      return config; // no room for the gap window (or even the code split)
    }
    const uint32_t window = codes - 1 - popular_count - reserve;
    escape_count = 0;
    uint32_t block_max = 0;
    uint32_t cur_block = UINT32_MAX;
    uint32_t cur_count = 0;
    for (uint32_t node_idx = 1; node_idx < node_count; node_idx++) {
      const uint32_t arc = nodes[node_idx].arc;
      if (arc == 0 || rank_pos[arc] < popular_count) {
        continue;
      }
      const int64_t gap = (int64_t)arc - (int64_t)node_idx;
      if (gap >= 1 && gap <= (int64_t)window) {
        continue;
      }
      escape_count++;
      const uint32_t block = node_idx >> block_bits;
      if (block != cur_block) {
        cur_block = block;
        cur_count = 0;
      }
      cur_count++;
      if (cur_count > block_max) {
        block_max = cur_count;
      }
    }
    if (block_max <= reserve) {
      break; // fixpoint: the reserved codes cover every block
    }
    if (block_max > block_size) {
      return config; // cannot happen (a block has block_size nodes), safety
    }
    reserve = block_max;
  }

  config.escape_reserve = reserve;
  config.escape_count = escape_count;
  config.total_bits =
      (uint64_t)node_count * (tile_bits + 2 + field) +
      (uint64_t)popular_count * arc_bits + (uint64_t)escape_count * arc_bits +
      dawg_arc_compressed_directory_bits(node_count, block_bits);
  config.feasible = true;
  return config;
}

// True when `candidate` beats `best` for the given mode: MIN_RAM minimizes
// total bits outright; BALANCED minimizes total bits among configs whose
// escape rate is at or below the ceiling.
static bool
dawg_arc_compressed_config_better(const DawgArcCompressedConfig *candidate,
                                  const DawgArcCompressedConfig *best,
                                  uint32_t node_count, bool balanced) {
  if (!candidate->feasible) {
    return false;
  }
  if (balanced &&
      (uint64_t)candidate->escape_count * 100 >
          (uint64_t)node_count * DAWG_ARC_COMPRESSED_BALANCED_ESCAPE_PCT) {
    return false;
  }
  return candidate->total_bits < best->total_bits;
}

// Chooses the (field, K, block_bits, R) tuple that minimizes the total resident
// size, sweeping a coarse K grid and then refining K locally around the
// winner. rank_pos[node] is the node's position in the in-degree ranking
// (UINT32_MAX if it is never a target).
static DawgArcCompressedConfig dawg_arc_compressed_best_config(
    const DawgArcCompressedNode *nodes, uint32_t node_count,
    const uint32_t *rank_pos, uint32_t ranked_count, uint8_t tile_bits,
    uint8_t arc_bits, dawg_arc_compressed_mode_t mode) {
  const bool balanced = mode == DAWG_ARC_COMPRESSED_MODE_BALANCED;
  DawgArcCompressedConfig best;
  memset(&best, 0, sizeof(best));
  best.total_bits = UINT64_MAX;
  best.feasible = false;
  // BALANCED falls back to the MIN_RAM winner when no config meets the escape
  // ceiling (e.g. a tiny lexicon where every config escapes a lot).
  DawgArcCompressedConfig best_any = best;

  for (uint8_t field = DAWG_ARC_COMPRESSED_MIN_FIELD; field <= arc_bits;
       field++) {
    for (int block_idx = 0;
         block_idx < DAWG_ARC_COMPRESSED_NUM_BLOCK_CANDIDATES; block_idx++) {
      const uint8_t block_bits = block_bits_candidates[block_idx];
      for (int k_idx = 0; k_idx < DAWG_ARC_COMPRESSED_NUM_POPULAR_CANDIDATES;
           k_idx++) {
        const uint32_t popular_count = popular_candidates[k_idx];
        if (popular_count > ranked_count || popular_count >= (1U << field)) {
          continue;
        }
        const DawgArcCompressedConfig candidate = dawg_arc_compressed_evaluate(
            nodes, node_count, rank_pos, tile_bits, arc_bits, field,
            popular_count, block_bits);
        if (dawg_arc_compressed_config_better(&candidate, &best, node_count,
                                              balanced)) {
          best = candidate;
        }
        if (dawg_arc_compressed_config_better(&candidate, &best_any, node_count,
                                              false)) {
          best_any = candidate;
        }
      }
    }
  }
  if (!best.feasible) {
    best = best_any;
  }

  // Local K refinement around the winner (same field/block, finer K).
  const int64_t base_k = best.popular_count;
  for (int64_t k = base_k - DAWG_ARC_COMPRESSED_K_REFINE_SPAN;
       k <= base_k + DAWG_ARC_COMPRESSED_K_REFINE_SPAN;
       k += DAWG_ARC_COMPRESSED_K_REFINE_STEP) {
    if (k < 0 || k == base_k || k > ranked_count ||
        k >= (int64_t)(1U << best.field)) {
      continue;
    }
    const DawgArcCompressedConfig candidate = dawg_arc_compressed_evaluate(
        nodes, node_count, rank_pos, tile_bits, arc_bits, best.field,
        (uint32_t)k, best.block_bits);
    if (dawg_arc_compressed_config_better(&candidate, &best, node_count,
                                          balanced)) {
      best = candidate;
    }
  }
  return best;
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
  const size_t blocks =
      ((size_t)dp->node_count + (1ULL << dp->block_bits) - 1) >> dp->block_bits;
  const size_t superblocks =
      (blocks + DAWG_ARC_COMPRESSED_SUPERBLOCK_BLOCKS - 1) /
      DAWG_ARC_COMPRESSED_SUPERBLOCK_BLOCKS;
  dp->anchor_bit_off = pos;
  pos += superblocks * 32;
  dp->delta_bit_off = pos;
  pos += blocks * 16;
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

  const DawgArcCompressedConfig config = dawg_arc_compressed_best_config(
      nodes, node_count, rank_pos, ranked_count, tile_bits, arc_bits, mode);
  const uint8_t field = config.field;
  const uint32_t popular_count = config.popular_count;
  const uint32_t escape_reserve = config.escape_reserve;
  const uint8_t block_bits = config.block_bits;
  const uint32_t escape_base = (1U << field) - escape_reserve;
  const uint32_t window = (1U << field) - 1 - popular_count - escape_reserve;
  const uint8_t rec_width = (uint8_t)(tile_bits + 2 + field);

  // Classify every arc into the four code ranges, collecting the escape
  // targets (in node order) and the per-block escape-start directory.
  uint32_t *code = (uint32_t *)malloc_or_die(sizeof(uint32_t) * node_count);
  uint32_t *escapes = (uint32_t *)malloc_or_die(sizeof(uint32_t) * node_count);
  const uint32_t num_blocks =
      (node_count + (1U << block_bits) - 1) >> block_bits;
  uint32_t *block_start =
      (uint32_t *)calloc_or_die(num_blocks + 1, sizeof(uint32_t));
  uint32_t escape_count = 0;
  uint32_t cur_block = 0;
  uint32_t cur_local = 0;
  for (uint32_t node_idx = 0; node_idx < node_count; node_idx++) {
    const uint32_t block = node_idx >> block_bits;
    while (cur_block < block) {
      cur_block++;
      block_start[cur_block] = escape_count;
      cur_local = 0;
    }
    const uint32_t arc = nodes[node_idx].arc;
    if (arc == 0) {
      code[node_idx] = 0;
      continue;
    }
    if (rank_pos[arc] < popular_count) {
      code[node_idx] = 1 + rank_pos[arc];
      continue;
    }
    const int64_t gap = (int64_t)arc - (int64_t)node_idx;
    if (gap >= 1 && gap <= (int64_t)window) {
      code[node_idx] = popular_count + (uint32_t)gap;
      continue;
    }
    code[node_idx] = escape_base + cur_local;
    cur_local++;
    escapes[escape_count++] = arc;
  }
  while (cur_block < num_blocks) {
    cur_block++;
    block_start[cur_block] = escape_count;
  }

  DawgArcCompressed *dp =
      (DawgArcCompressed *)malloc_or_die(sizeof(DawgArcCompressed));
  dp->node_count = node_count;
  dp->root_index = root;
  dp->popular_count = popular_count;
  dp->escape_count = escape_count;
  dp->escape_base = escape_base;
  dp->tile_bits = tile_bits;
  dp->arc_bits = arc_bits;
  dp->rec_width = rec_width;
  dp->field = field;
  dp->escape_reserve = (uint8_t)escape_reserve;
  dp->block_bits = block_bits;
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
  bytes[9] = (uint8_t)escape_reserve;
  bytes[10] = block_bits;
  // bytes[11] reserved padding (already zero). Multi-byte fields use native
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
  // Two-level escape-start directory: 32-bit anchors per superblock, 16-bit
  // per-block deltas from the covering anchor.
  const uint32_t num_superblocks =
      (num_blocks + DAWG_ARC_COMPRESSED_SUPERBLOCK_BLOCKS - 1) /
      DAWG_ARC_COMPRESSED_SUPERBLOCK_BLOCKS;
  for (uint32_t super_idx = 0; super_idx < num_superblocks; super_idx++) {
    dawg_packed_bits_write(
        bytes, dp->anchor_bit_off + (size_t)super_idx * 32, 32,
        block_start[super_idx * DAWG_ARC_COMPRESSED_SUPERBLOCK_BLOCKS]);
  }
  for (uint32_t block_idx = 0; block_idx < num_blocks; block_idx++) {
    const uint32_t anchor =
        block_start[(block_idx / DAWG_ARC_COMPRESSED_SUPERBLOCK_BLOCKS) *
                    DAWG_ARC_COMPRESSED_SUPERBLOCK_BLOCKS];
    dawg_packed_bits_write(bytes, dp->delta_bit_off + (size_t)block_idx * 16,
                           16, block_start[block_idx] - anchor);
  }
  // Records.
  for (uint32_t node_idx = 0; node_idx < node_count; node_idx++) {
    const uint32_t value =
        code[node_idx] |
        ((uint32_t)(nodes[node_idx].is_end ? 1U : 0U) << field) |
        ((uint32_t)(nodes[node_idx].accepts ? 1U : 0U) << (field + 1)) |
        ((uint32_t)nodes[node_idx].tile << (field + 2));
    dawg_packed_bits_write(bytes,
                           dp->record_bit_off + (size_t)node_idx * rec_width,
                           rec_width, value);
  }
  dp->bytes = bytes;

  free(nodes);
  free(in_degree);
  free(ranked);
  free(rank_pos);
  free(code);
  free(escapes);
  free(block_start);
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
  dp->escape_reserve = header[9];
  dp->block_bits = header[10];
  memcpy(&dp->node_count, header + 12, sizeof(uint32_t));
  memcpy(&dp->root_index, header + 16, sizeof(uint32_t));
  memcpy(&dp->popular_count, header + 20, sizeof(uint32_t));
  memcpy(&dp->escape_count, header + 24, sizeof(uint32_t));

  // Reject corrupt/malicious headers before trusting the fields for sizing,
  // allocation, and bit-shift widths. escape_reserve == 0 is only coherent
  // with no escapes at all (there is no code range to address any).
  const uint8_t expected_rec_width = (uint8_t)(dp->tile_bits + 2 + dp->field);
  const bool field_ok = dp->field >= 1 && dp->field < 32;
  const bool code_space_ok =
      field_ok && (uint64_t)dp->popular_count + dp->escape_reserve + 1 <=
                      (1ULL << dp->field);
  if (dp->tile_bits < 1 || dp->tile_bits > DAWG_ARC_COMPRESSED_MAX_TILE_BITS ||
      dp->arc_bits < 1 || dp->arc_bits > DAWG_ARC_COMPRESSED_MAX_ARC_BITS ||
      !field_ok || dp->field >= dp->arc_bits + 2 ||
      dp->rec_width != expected_rec_width || dp->node_count == 0 ||
      dp->node_count > (1U << dp->arc_bits) ||
      dp->root_index >= dp->node_count || !code_space_ok ||
      dp->block_bits < 1 ||
      dp->block_bits > DAWG_ARC_COMPRESSED_MAX_BLOCK_BITS ||
      dp->escape_reserve > (1U << dp->block_bits) ||
      dp->escape_count > dp->node_count ||
      (dp->escape_reserve == 0 && dp->escape_count != 0)) {
    fclose_or_die(stream);
    free(dp);
    error_stack_push(
        error_stack, ERROR_STATUS_CONVERT_MALFORMED_KWG,
        get_formatted_string(
            "invalid arc-compressed dawg header fields in file: %s", filename));
    return NULL;
  }
  dp->escape_base = (1U << dp->field) - dp->escape_reserve;

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
