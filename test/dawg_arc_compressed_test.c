#include "../src/compat/ctime.h"
#include "../src/def/board_defs.h"
#include "../src/def/kwg_defs.h"
#include "../src/ent/dawg_arc_compressed.h"
#include "../src/ent/dawg_packed.h"
#include "../src/ent/dictionary_word.h"
#include "../src/ent/game.h"
#include "../src/ent/kwg.h"
#include "../src/ent/player.h"
#include "../src/impl/config.h"
#include "../src/impl/kwg_maker.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  // Cap word length to emulate a constrained-hardware word list (and keep the
  // ASAN build fast), matching the packed-DAWG test's corpus shape.
  MAX_TEST_WORD_LENGTH = 8,
};

static void assert_same_word_set(DictionaryWordList *expected,
                                 DictionaryWordList *actual) {
  dictionary_word_list_sort(expected);
  dictionary_word_list_sort(actual);
  assert(dictionary_word_list_get_count(expected) ==
         dictionary_word_list_get_count(actual));
  for (int word_idx = 0; word_idx < dictionary_word_list_get_count(expected);
       word_idx++) {
    const DictionaryWord *expected_word =
        dictionary_word_list_get_word(expected, word_idx);
    const DictionaryWord *actual_word =
        dictionary_word_list_get_word(actual, word_idx);
    assert(dictionary_word_get_length(expected_word) ==
           dictionary_word_get_length(actual_word));
    assert(memcmp(dictionary_word_get_word(expected_word),
                  dictionary_word_get_word(actual_word),
                  dictionary_word_get_length(expected_word)) == 0);
  }
}

// Builds the expected word set (a fresh copy of words) so the in-memory and
// file-loaded decodings can each be checked against it.
static DictionaryWordList *copy_word_list(const DictionaryWordList *words) {
  DictionaryWordList *copy = dictionary_word_list_create();
  for (int word_idx = 0; word_idx < dictionary_word_list_get_count(words);
       word_idx++) {
    const DictionaryWord *word = dictionary_word_list_get_word(words, word_idx);
    dictionary_word_list_add_word(copy, dictionary_word_get_word(word),
                                  dictionary_word_get_length(word));
  }
  return copy;
}

// A corrupt header (here, an out-of-range arc_bits) must be rejected cleanly
// rather than triggering a huge allocation or an out-of-range shift.
static void assert_rejects_bad_header(void) {
  const char *filename = "dawg_arc_compressed_bad_header_test.acdawg";
  uint8_t header[DAWG_ARC_COMPRESSED_HEADER_BYTES];
  memset(header, 0, sizeof(header));
  for (int magic_idx = 0; magic_idx < 4; magic_idx++) {
    header[magic_idx] = (uint8_t)DAWG_ARC_COMPRESSED_MAGIC[magic_idx];
  }
  header[4] = DAWG_ARC_COMPRESSED_VERSION;
  header[5] = 5;  // tile_bits
  header[6] = 99; // arc_bits: out of range
  header[7] = 15; // rec_width
  header[8] = 7;  // field
  ErrorStack *error_stack = error_stack_create();
  FILE *stream = fopen_safe(filename, "wb", error_stack);
  assert(error_stack_is_empty(error_stack));
  fwrite_or_die(header, 1, sizeof(header), stream,
                "bad arc-compressed dawg header");
  fclose_or_die(stream);

  const DawgArcCompressed *dp =
      dawg_arc_compressed_read_from_file(filename, error_stack);
  assert(dp == NULL);
  assert(!error_stack_is_empty(error_stack));
  error_stack_destroy(error_stack);
  (void)remove(filename);
}

// Builds, compresses, decodes and round-trips the (length-capped) word list of
// `lexicon`. Run for English (CSW21) and Polish (OSPS49); the larger Polish
// alphabet flexes the wider tile_bits encoding path.
static void test_dawg_arc_compressed_for_lexicon(const char *lexicon) {
  char *set_cmd = get_formatted_string("set -lex %s -wmp false", lexicon);
  Config *config = config_create_or_die(set_cmd);
  free(set_cmd);
  Game *game = config_game_create(config);
  const KWG *csw_kwg = player_get_kwg(game_get_player(game, 0));
  DictionaryWordList *all_words = dictionary_word_list_create();
  kwg_write_words(csw_kwg, kwg_get_dawg_root_node_index(csw_kwg), all_words,
                  NULL);

  DictionaryWordList *words = dictionary_word_list_create();
  for (int word_idx = 0; word_idx < dictionary_word_list_get_count(all_words);
       word_idx++) {
    const DictionaryWord *word =
        dictionary_word_list_get_word(all_words, word_idx);
    if (dictionary_word_get_length(word) <= MAX_TEST_WORD_LENGTH) {
      dictionary_word_list_add_word(words, dictionary_word_get_word(word),
                                    dictionary_word_get_length(word));
    }
  }
  dictionary_word_list_destroy(all_words);

  KWG *reorder_kwg = make_kwg_from_words(words, KWG_MAKER_OUTPUT_DAWG,
                                         KWG_MAKER_MERGE_TAIL_REORDER);
  const int reorder_node_count = kwg_get_number_of_nodes(reorder_kwg);

  DawgArcCompressed *dp = dawg_arc_compressed_create_from_kwg(
      reorder_kwg, DAWG_ARC_COMPRESSED_MODE_MIN_RAM);
  printf(
      "[%s] arc-compressed dawg: %u nodes, tile_bits=%u arc_bits=%u field=%u "
      "rec_width=%u, popular=%u escapes=%u -> %zu bytes (vs %d bytes at 32 "
      "bits, %.2f%%)\n",
      lexicon, dawg_arc_compressed_get_node_count(dp), dp->tile_bits,
      dp->arc_bits, dp->field, dp->rec_width, dp->popular_count,
      dp->escape_count, dawg_arc_compressed_get_num_bytes(dp),
      reorder_node_count * 4,
      100.0 * (double)dawg_arc_compressed_get_num_bytes(dp) /
          (reorder_node_count * 4));

  // Must beat the 32-bit-per-node baseline.
  assert(dawg_arc_compressed_get_num_bytes(dp) <
         (size_t)reorder_node_count * 4);

  // The decoded word set must match the source word list.
  DictionaryWordList *decoded = dictionary_word_list_create();
  dawg_arc_compressed_write_words(dp, decoded);
  DictionaryWordList *expected = copy_word_list(words);
  assert_same_word_set(expected, decoded);

  // BALANCED mode trades RAM for fewer escapes (and thus faster traversal): no
  // more escapes than MIN_RAM, at least as large, and the same word set.
  DawgArcCompressed *bal = dawg_arc_compressed_create_from_kwg(
      reorder_kwg, DAWG_ARC_COMPRESSED_MODE_BALANCED);
  printf("arc-compressed dawg (balanced): field=%u K=%u escapes=%u -> %zu "
         "bytes\n",
         bal->field, bal->popular_count, bal->escape_count,
         dawg_arc_compressed_get_num_bytes(bal));
  assert(bal->escape_count <= dp->escape_count);
  assert(dawg_arc_compressed_get_num_bytes(bal) >=
         dawg_arc_compressed_get_num_bytes(dp));
  DictionaryWordList *bal_decoded = dictionary_word_list_create();
  dawg_arc_compressed_write_words(bal, bal_decoded);
  DictionaryWordList *bal_expected = copy_word_list(words);
  assert_same_word_set(bal_expected, bal_decoded);
  dictionary_word_list_destroy(bal_decoded);
  dictionary_word_list_destroy(bal_expected);
  dawg_arc_compressed_destroy(bal);

  // File round-trip: the word set must survive write + read.
  const char *filename = "dawg_arc_compressed_round_trip_test.acdawg";
  ErrorStack *error_stack = error_stack_create();
  dawg_arc_compressed_write_to_file(dp, filename, error_stack);
  assert(error_stack_is_empty(error_stack));
  DawgArcCompressed *loaded =
      dawg_arc_compressed_read_from_file(filename, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(loaded != NULL);
  assert(loaded->node_count == dp->node_count);
  assert(loaded->root_index == dp->root_index);
  assert(loaded->popular_count == dp->popular_count);
  assert(loaded->escape_count == dp->escape_count);
  assert(loaded->tile_bits == dp->tile_bits);
  assert(loaded->arc_bits == dp->arc_bits);
  assert(loaded->rec_width == dp->rec_width);
  assert(loaded->field == dp->field);
  assert(loaded->escape_reserve == dp->escape_reserve);
  assert(loaded->block_bits == dp->block_bits);
  assert(loaded->escape_base == dp->escape_base);
  assert(dawg_arc_compressed_get_num_bytes(loaded) ==
         dawg_arc_compressed_get_num_bytes(dp));
  assert(memcmp(loaded->bytes, dp->bytes, dp->num_bytes) == 0);

  DictionaryWordList *decoded_loaded = dictionary_word_list_create();
  dawg_arc_compressed_write_words(loaded, decoded_loaded);
  DictionaryWordList *expected_loaded = copy_word_list(words);
  assert_same_word_set(expected_loaded, decoded_loaded);

  (void)remove(filename);
  dictionary_word_list_destroy(decoded);
  dictionary_word_list_destroy(expected);
  dictionary_word_list_destroy(decoded_loaded);
  dictionary_word_list_destroy(expected_loaded);
  dawg_arc_compressed_destroy(loaded);
  error_stack_destroy(error_stack);
  dawg_arc_compressed_destroy(dp);
  dictionary_word_list_destroy(words);
  kwg_destroy(reorder_kwg);
  game_destroy(game);
  config_destroy(config);
}

void test_dawg_arc_compressed(void) {
  test_dawg_arc_compressed_for_lexicon("CSW21");
  // Polish: a larger alphabet (no 64-bit BitRack) that flexes the wider
  // tile_bits arc encoding.
  test_dawg_arc_compressed_for_lexicon("OSPS49");
  assert_rejects_bad_header();
}

// ---------------------------------------------------------------------------
// On-demand encoding study (acdawgstats): where do the bytes go, and how much
// would candidate re-encodings save? Reconstructs the renumbered graph through
// the public decoder (each record IS the renumbered node), recomputes the
// in-degree ranking the builder used, then sizes what-if variants analytically
// from the arc distributions -- no src/ changes and no new encoders needed to
// compare formats. Variants sized:
//   v2      -- the shipped format (model cross-checked against actual bytes).
//   A1      -- unified code space: no flag bit; subfield values partition into
//              none / popular / gap window / escape sentinel (continuous K).
//   A2      -- A1 + block-local escape indices: top R codes give the escape's
//              index within its rank-block, so the bitmap AND the decode-time
//              popcount rank disappear; a per-block escape-start directory
//              (two-level: 32-bit superblock anchors + 16-bit block deltas)
//              replaces them. R must cover the max escapes in any block.
//   +split  -- for A1/A2, an asymmetric positive/negative gap split chosen
//              from the data instead of zigzag's fixed 50/50.
// ---------------------------------------------------------------------------

enum {
  ACDAWG_STATS_GAP_BUCKETS = 24, // log2 magnitude buckets for the histogram
  ACDAWG_STATS_BLOCK = 64,       // what-if models price 64-node blocks
  ACDAWG_STATS_SUPERBLOCK_BLOCKS = DAWG_ARC_COMPRESSED_SUPERBLOCK_BLOCKS,
};

// One outgoing arc of the renumbered graph: its source node, the target's
// position in the in-degree ranking, and the signed source-to-target gap.
typedef struct AcdawgStatsArc {
  uint32_t source;
  uint32_t rank;
  int64_t gap;
} AcdawgStatsArc;

// (in-degree, index) pair mirroring the builder's private ranking entry, with
// the same ordering (descending in-degree, ties by ascending index) so the
// recomputed ranking matches the one the builder used.
typedef struct AcdawgStatsRankEntry {
  uint32_t in_degree;
  uint32_t index;
} AcdawgStatsRankEntry;

static int acdawg_stats_rank_compare(const void *left, const void *right) {
  const AcdawgStatsRankEntry *left_entry = (const AcdawgStatsRankEntry *)left;
  const AcdawgStatsRankEntry *right_entry = (const AcdawgStatsRankEntry *)right;
  if (left_entry->in_degree != right_entry->in_degree) {
    return (left_entry->in_degree < right_entry->in_degree) ? 1 : -1;
  }
  if (left_entry->index != right_entry->index) {
    return (left_entry->index < right_entry->index) ? -1 : 1;
  }
  return 0;
}

typedef struct AcdawgStatsGraph {
  AcdawgStatsArc *arcs;
  uint32_t arc_count;
  uint32_t node_count;
  uint32_t no_child_count;
  uint8_t tile_bits;
  uint8_t arc_bits;
} AcdawgStatsGraph;

// Builds the arc table (rank + gap per arc) from a first-child index array
// (arcs_of[node] = renumbered first-child index, 0 = none), recomputing the
// same descending in-degree ranking the builder uses. Shared by the built
// graph and the reordered what-if graphs.
static AcdawgStatsGraph acdawg_stats_graph_from_arcs(const uint32_t *arcs_of,
                                                     uint32_t node_count,
                                                     uint8_t tile_bits,
                                                     uint8_t arc_bits) {
  uint32_t *in_degree = (uint32_t *)calloc_or_die(node_count, sizeof(uint32_t));
  uint32_t arc_count = 0;
  uint32_t no_child_count = 0;
  for (uint32_t node_idx = 1; node_idx < node_count; node_idx++) {
    const uint32_t arc = arcs_of[node_idx];
    if (arc == 0) {
      no_child_count++;
    } else {
      in_degree[arc]++;
      arc_count++;
    }
  }

  // Descending in-degree, ties by ascending index (the builder's ordering).
  AcdawgStatsRankEntry *ranked = (AcdawgStatsRankEntry *)malloc_or_die(
      sizeof(AcdawgStatsRankEntry) * node_count);
  uint32_t ranked_count = 0;
  for (uint32_t node_idx = 0; node_idx < node_count; node_idx++) {
    if (in_degree[node_idx] > 0) {
      ranked[ranked_count].in_degree = in_degree[node_idx];
      ranked[ranked_count].index = node_idx;
      ranked_count++;
    }
  }
  qsort(ranked, ranked_count, sizeof(AcdawgStatsRankEntry),
        acdawg_stats_rank_compare);
  uint32_t *rank_pos = (uint32_t *)malloc_or_die(sizeof(uint32_t) * node_count);
  for (uint32_t node_idx = 0; node_idx < node_count; node_idx++) {
    rank_pos[node_idx] = UINT32_MAX;
  }
  for (uint32_t rank_idx = 0; rank_idx < ranked_count; rank_idx++) {
    rank_pos[ranked[rank_idx].index] = rank_idx;
  }

  AcdawgStatsArc *arcs =
      (AcdawgStatsArc *)malloc_or_die(sizeof(AcdawgStatsArc) * arc_count);
  uint32_t out_idx = 0;
  for (uint32_t node_idx = 1; node_idx < node_count; node_idx++) {
    const uint32_t arc = arcs_of[node_idx];
    if (arc == 0) {
      continue;
    }
    arcs[out_idx].source = node_idx;
    arcs[out_idx].rank = rank_pos[arc];
    arcs[out_idx].gap = (int64_t)arc - (int64_t)node_idx;
    out_idx++;
  }

  AcdawgStatsGraph graph;
  graph.arcs = arcs;
  graph.arc_count = arc_count;
  graph.node_count = node_count;
  graph.no_child_count = no_child_count;
  graph.tile_bits = tile_bits;
  graph.arc_bits = arc_bits;
  free(in_degree);
  free(ranked);
  free(rank_pos);
  return graph;
}

// Number of bits needed to represent every value in [0, max_value].
static uint8_t acdawg_stats_bits_needed(uint32_t max_value) {
  uint8_t bits = 1;
  while ((max_value >> bits) != 0) {
    bits++;
  }
  return bits;
}

// Run-addressing what-if: a copy of the arc table whose gaps are rewritten
// into RUN-ordinal units (target run's layout ordinal minus the source run's).
// An arc into the middle of a run (tail-merge suffix sharing) cannot be
// gap-coded in run units at all; it gets a never-in-window sentinel so the
// sizing model sends it to the popular table or an escape. Reports the run
// count and the mid-run arc count through the out params.
static AcdawgStatsGraph
acdawg_stats_run_gap_graph(const uint32_t *arcs_of, const uint8_t *is_end,
                           uint32_t node_count, uint8_t tile_bits,
                           uint8_t arc_bits, uint32_t *out_run_count,
                           uint32_t *out_mid_run_arcs) {
  AcdawgStatsGraph graph =
      acdawg_stats_graph_from_arcs(arcs_of, node_count, tile_bits, arc_bits);

  // run_of[node] = its run's start node; ordinal[node] = its run's 1-based
  // position in layout order. Node 0 is the sentinel (is_end set), so node 1
  // starts the first real run.
  uint32_t *run_of = (uint32_t *)malloc_or_die(sizeof(uint32_t) * node_count);
  uint32_t *ordinal = (uint32_t *)malloc_or_die(sizeof(uint32_t) * node_count);
  run_of[0] = 0;
  ordinal[0] = 0;
  uint32_t current_run = 0;
  uint32_t run_count = 0;
  for (uint32_t node_idx = 1; node_idx < node_count; node_idx++) {
    if (is_end[node_idx - 1] != 0) {
      current_run = node_idx;
      run_count++;
    }
    run_of[node_idx] = current_run;
    ordinal[node_idx] = run_count;
  }

  uint32_t mid_run_arcs = 0;
  for (uint32_t arc_idx = 0; arc_idx < graph.arc_count; arc_idx++) {
    AcdawgStatsArc *arc = &graph.arcs[arc_idx];
    const uint32_t target = (uint32_t)((int64_t)arc->source + arc->gap);
    if (run_of[target] == target) {
      arc->gap = (int64_t)ordinal[target] - (int64_t)ordinal[arc->source];
    } else {
      arc->gap = INT64_MIN; // mid-run: not expressible as a run gap
      mid_run_arcs++;
    }
  }
  *out_run_count = run_count;
  *out_mid_run_arcs = mid_run_arcs;
  free(run_of);
  free(ordinal);
  return graph;
}

// Builds the arc table from a built acdawg by decoding every record.
static AcdawgStatsGraph acdawg_stats_graph_build(const DawgArcCompressed *dp) {
  const uint32_t node_count = dawg_arc_compressed_get_node_count(dp);
  uint32_t *arcs_of = (uint32_t *)calloc_or_die(node_count, sizeof(uint32_t));
  for (uint32_t node_idx = 1; node_idx < node_count; node_idx++) {
    const uint32_t node = dawg_arc_compressed_get_node(dp, node_idx);
    arcs_of[node_idx] = kwg_node_arc_index(node);
  }
  AcdawgStatsGraph graph = acdawg_stats_graph_from_arcs(
      arcs_of, node_count, dp->tile_bits, dp->arc_bits);
  free(arcs_of);
  return graph;
}

// Escape count for a gap window of `pos_codes` positive and `neg_codes`
// negative codes over the arcs not absorbed by a top-K popular table. When
// out_block_max is non-NULL also reports the max escapes in any rank-block
// (the R a block-local escape encoding must reserve).
static uint32_t acdawg_stats_escapes(const AcdawgStatsGraph *graph, uint32_t k,
                                     int64_t pos_codes, int64_t neg_codes,
                                     uint32_t *out_block_max) {
  uint32_t escapes = 0;
  uint32_t block_max = 0;
  uint32_t cur_block = UINT32_MAX;
  uint32_t cur_count = 0;
  for (uint32_t arc_idx = 0; arc_idx < graph->arc_count; arc_idx++) {
    const AcdawgStatsArc *arc = &graph->arcs[arc_idx];
    if (arc->rank < k) {
      continue;
    }
    const bool in_window = (arc->gap > 0 && arc->gap <= pos_codes) ||
                           (arc->gap < 0 && -arc->gap <= neg_codes);
    if (in_window) {
      continue;
    }
    escapes++;
    if (out_block_max != NULL) {
      const uint32_t block = arc->source / ACDAWG_STATS_BLOCK;
      if (block != cur_block) {
        cur_block = block;
        cur_count = 0;
      }
      cur_count++;
      if (cur_count > block_max) {
        block_max = cur_count;
      }
    }
  }
  if (out_block_max != NULL) {
    *out_block_max = block_max;
  }
  return escapes;
}

// Best asymmetric split of `window` gap codes into positive + negative counts
// for the arcs not absorbed by a top-K popular table: sweeps the negative
// share over powers of two (plus 0) and keeps the split with fewest escapes.
static void acdawg_stats_best_split(const AcdawgStatsGraph *graph, uint32_t k,
                                    int64_t window, int64_t *out_pos,
                                    int64_t *out_neg, uint32_t *out_escapes) {
  int64_t best_pos = window;
  int64_t best_neg = 0;
  uint32_t best_escapes = UINT32_MAX;
  for (int64_t neg = 0; neg <= window; neg = (neg == 0) ? 1 : neg * 2) {
    const int64_t pos = window - neg;
    const uint32_t escapes = acdawg_stats_escapes(graph, k, pos, neg, NULL);
    if (escapes < best_escapes) {
      best_escapes = escapes;
      best_pos = pos;
      best_neg = neg;
    }
  }
  *out_pos = best_pos;
  *out_neg = best_neg;
  *out_escapes = best_escapes;
}

// Two-level escape-start directory bytes for the A2 (bitmap-free) variant:
// a 32-bit anchor per superblock plus a 16-bit delta per block.
static uint64_t acdawg_stats_a2_directory_bytes(uint32_t node_count) {
  const uint64_t blocks =
      ((uint64_t)node_count + ACDAWG_STATS_BLOCK - 1) / ACDAWG_STATS_BLOCK;
  const uint64_t superblocks = (blocks + ACDAWG_STATS_SUPERBLOCK_BLOCKS - 1) /
                               ACDAWG_STATS_SUPERBLOCK_BLOCKS;
  return superblocks * 4 + blocks * 2;
}

// v2's non-record overhead: popular table + escape array + bitmap + directory.
static uint64_t acdawg_stats_v2_overhead_bits(const AcdawgStatsGraph *graph,
                                              uint32_t k, uint32_t escapes) {
  const uint64_t blocks =
      ((uint64_t)graph->node_count + ACDAWG_STATS_BLOCK - 1) /
      ACDAWG_STATS_BLOCK;
  return (uint64_t)k * graph->arc_bits + (uint64_t)escapes * graph->arc_bits +
         ((uint64_t)graph->node_count + 7) / 8 * 8 + (blocks + 1) * 32;
}

// ---- Ordering experiments: does a different DFS child order shrink the
// best-config total? The renumbering is a builder choice, not a format
// feature, so a winning order is a src change to the renumberer only.

// The best v3-model configuration found for one arc table.
typedef struct AcdawgStatsV3Best {
  uint64_t bytes;
  uint32_t k;
  uint32_t reserve;
  uint32_t escapes;
  uint8_t field;
  uint8_t block_bits;
} AcdawgStatsV3Best;

// Models the v3 builder's search on an arc table: positive-only gap window, R
// iterated to a fixpoint, coarse K grid, block sizes 32/64/128 (no local K
// refinement -- comparisons across orderings use the same model).
static AcdawgStatsV3Best acdawg_stats_best_v3(const AcdawgStatsGraph *graph) {
  static const uint32_t k_grid[] = {0,    64,   128,  192,   256,  384,
                                    512,  768,  1024, 1536,  2048, 3072,
                                    4096, 6144, 8192, 12288, 16384};
  enum { ACDAWG_STATS_V3_K_GRID = 17 };
  AcdawgStatsV3Best best;
  memset(&best, 0, sizeof(best));
  best.bytes = UINT64_MAX;
  for (uint8_t field = 4; field <= graph->arc_bits; field++) {
    const uint64_t codes = 1ULL << field;
    for (uint8_t block_bits = 5; block_bits <= 7; block_bits++) {
      for (int k_idx = 0; k_idx < ACDAWG_STATS_V3_K_GRID; k_idx++) {
        const uint32_t k = k_grid[k_idx];
        if (k + 1 >= codes) {
          continue;
        }
        uint32_t reserve = 0;
        uint32_t escapes = 0;
        bool feasible = true;
        // reserve strictly increases per round and is bounded by the block
        // size, so this converges.
        for (;;) {
          if ((uint64_t)k + reserve + 1 > codes) {
            feasible = false;
            break;
          }
          const int64_t window = (int64_t)(codes - 1 - k - reserve);
          escapes = 0;
          uint32_t block_max = 0;
          uint32_t cur_block = UINT32_MAX;
          uint32_t cur_count = 0;
          for (uint32_t arc_idx = 0; arc_idx < graph->arc_count; arc_idx++) {
            const AcdawgStatsArc *arc = &graph->arcs[arc_idx];
            if (arc->rank < k || (arc->gap >= 1 && arc->gap <= window)) {
              continue;
            }
            escapes++;
            const uint32_t block = arc->source >> block_bits;
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
            break;
          }
          reserve = block_max;
        }
        if (!feasible) {
          continue;
        }
        const uint64_t blocks =
            ((uint64_t)graph->node_count + (1ULL << block_bits) - 1) >>
            block_bits;
        const uint64_t superblocks =
            (blocks + ACDAWG_STATS_SUPERBLOCK_BLOCKS - 1) /
            ACDAWG_STATS_SUPERBLOCK_BLOCKS;
        const uint64_t total_bits =
            (uint64_t)graph->node_count * (graph->tile_bits + 2 + field) +
            (uint64_t)k * graph->arc_bits +
            (uint64_t)escapes * graph->arc_bits + superblocks * 32 +
            blocks * 16;
        const uint64_t total = (total_bits + 7) / 8;
        if (total < best.bytes) {
          best.bytes = total;
          best.k = k;
          best.reserve = reserve;
          best.escapes = escapes;
          best.field = field;
          best.block_bits = block_bits;
        }
      }
    }
  }
  return best;
}

// (weight, position) pair for ordering a run's child runs.
typedef struct AcdawgStatsKidEntry {
  uint64_t weight;
  uint32_t position; // collection order, the tie-break for determinism
  uint32_t run;
} AcdawgStatsKidEntry;

static int acdawg_stats_kid_compare_asc(const void *left, const void *right) {
  const AcdawgStatsKidEntry *left_kid = (const AcdawgStatsKidEntry *)left;
  const AcdawgStatsKidEntry *right_kid = (const AcdawgStatsKidEntry *)right;
  if (left_kid->weight != right_kid->weight) {
    return (left_kid->weight < right_kid->weight) ? -1 : 1;
  }
  return (left_kid->position < right_kid->position) ? -1 : 1;
}

// One frame of the iterative weight computation: the run being walked, the
// next member to process, the child run whose finished weight to fold in on
// resume, and the weight accumulated so far.
typedef struct AcdawgStatsWeightFrame {
  uint32_t run;
  uint32_t cursor;
  uint32_t pending;
  uint64_t weight;
} AcdawgStatsWeightFrame;

// Subtree weight of every run reachable from root_run: run length plus the
// weights of every member's child run -- the standard placement-cost proxy.
// With first_visit_only false this is the tree-expanded weight (a shared
// subtree counts once per completed reference); with it true a run's weight
// counts only runs not yet reached anywhere in the traversal, approximating
// the number of NEW nodes its placement would add. Tail-merged arcs can point
// INTO other runs, so run-to-run edges are not depth-monotone and can even
// form cycles; this therefore uses an explicit post-order stack (no
// recursion) and counts a back-edge to an in-progress run as zero.
static void
acdawg_stats_compute_weights(uint32_t root_run, const uint32_t *arcs_of,
                             const uint8_t *is_end, const uint32_t *run_of,
                             uint32_t node_count, bool first_visit_only,
                             uint64_t *memo) {
  // 0 = unvisited, 1 = in progress (on the stack), 2 = done.
  uint8_t *state = (uint8_t *)calloc_or_die(node_count, 1);
  AcdawgStatsWeightFrame *stack = (AcdawgStatsWeightFrame *)malloc_or_die(
      sizeof(AcdawgStatsWeightFrame) * (node_count + 1));
  uint32_t top = 0;
  stack[top].run = root_run;
  stack[top].cursor = root_run;
  stack[top].pending = 0;
  stack[top].weight = 0;
  top++;
  state[root_run] = 1;
  while (top > 0) {
    AcdawgStatsWeightFrame *frame = &stack[top - 1];
    if (frame->pending != 0) {
      if (state[frame->pending] == 2) {
        frame->weight += memo[frame->pending];
      }
      frame->pending = 0;
    }
    bool descended = false;
    for (;;) {
      // Complete when the previously processed member was the run's end.
      if (frame->cursor > frame->run && is_end[frame->cursor - 1] != 0) {
        break;
      }
      const uint32_t node_idx = frame->cursor;
      frame->cursor++;
      frame->weight++;
      const uint32_t arc = arcs_of[node_idx];
      if (arc != 0) {
        const uint32_t child_run = run_of[arc];
        if (state[child_run] == 0) {
          frame->pending = child_run;
          state[child_run] = 1;
          stack[top].run = child_run;
          stack[top].cursor = child_run;
          stack[top].pending = 0;
          stack[top].weight = 0;
          top++;
          descended = true;
          break;
        }
        if (state[child_run] == 2 && !first_visit_only) {
          frame->weight += memo[child_run];
        }
        // state 1 (a back-edge into an in-progress run) contributes zero, as
        // does any revisited run when only first visits count.
      }
    }
    if (!descended) {
      memo[frame->run] = frame->weight;
      state[frame->run] = 2;
      top--;
    }
  }
  free(state);
  free(stack);
}

// Sort-key bias that pushes shared child runs (in-degree > 1) after unique
// ones in the shared-last ordering; well above any real subtree weight.
#define ACDAWG_STATS_SHARED_KID_BIAS (1ULL << 40)

// Renumbers the graph with the builder's run-DFS, but ordering each run's
// child runs by subtree weight: order_mode 0 keeps sibling order (must
// reproduce the input numbering -- a self-check), 1 processes the smallest
// subtree first, 2 the largest first, 3 the smallest first with shared
// (in-degree > 1) child runs pushed last (only one parent can get locality
// to a shared run, so unique-parent runs take the near slots), 4 the
// smallest first by first-visit size (only the nodes a placement would
// newly add). Returns a freshly allocated first-child array in the new
// numbering.
static uint32_t *acdawg_stats_reorder(const uint32_t *arcs_of,
                                      const uint8_t *is_end,
                                      uint32_t node_count, uint32_t root,
                                      int order_mode) {
  // run_of[node] = first node of its sibling run. Node 0 is the sentinel (its
  // is_end is set), so node 1 starts the first real run.
  uint32_t *run_of = (uint32_t *)malloc_or_die(sizeof(uint32_t) * node_count);
  run_of[0] = 0;
  uint32_t current_run = 0;
  for (uint32_t node_idx = 1; node_idx < node_count; node_idx++) {
    if (is_end[node_idx - 1] != 0) {
      current_run = node_idx;
    }
    run_of[node_idx] = current_run;
  }

  // Weights are only consulted when reordering; mode 0's kid entries read the
  // zeroed memo (their order is position-preserving anyway).
  uint64_t *weight_memo =
      (uint64_t *)calloc_or_die(node_count, sizeof(uint64_t));
  if (order_mode != 0) {
    acdawg_stats_compute_weights(
        run_of[root], arcs_of, is_end, run_of, node_count,
        /*first_visit_only=*/order_mode == 4, weight_memo);
  }
  // Shared-last mode biases the sort key of any run referenced by more than
  // one arc.
  if (order_mode == 3) {
    uint32_t *run_in_degree =
        (uint32_t *)calloc_or_die(node_count, sizeof(uint32_t));
    for (uint32_t node_idx = 1; node_idx < node_count; node_idx++) {
      if (arcs_of[node_idx] != 0) {
        run_in_degree[run_of[arcs_of[node_idx]]]++;
      }
    }
    for (uint32_t node_idx = 0; node_idx < node_count; node_idx++) {
      if (run_of[node_idx] == node_idx && run_in_degree[node_idx] > 1) {
        weight_memo[node_idx] += ACDAWG_STATS_SHARED_KID_BIAS;
      }
    }
    free(run_in_degree);
  }

  uint32_t *new_of = (uint32_t *)calloc_or_die(node_count, sizeof(uint32_t));
  uint32_t *order = (uint32_t *)malloc_or_die(sizeof(uint32_t) * node_count);
  uint32_t order_count = 0;
  uint8_t *visited = (uint8_t *)calloc_or_die((node_count + 7) / 8, 1);
  // Total pushes are bounded by the arc count plus the root (<= node_count+1).
  uint32_t *stack =
      (uint32_t *)malloc_or_die(sizeof(uint32_t) * (node_count + 1));
  uint32_t stack_top = 0;
  AcdawgStatsKidEntry *kids = (AcdawgStatsKidEntry *)malloc_or_die(
      sizeof(AcdawgStatsKidEntry) * node_count);

  stack[stack_top++] = run_of[root];
  while (stack_top > 0) {
    const uint32_t run = stack[--stack_top];
    if (((visited[run >> 3] >> (run & 7)) & 1U) != 0) {
      continue;
    }
    visited[run >> 3] |= (uint8_t)(1U << (run & 7));
    uint32_t kids_count = 0;
    for (uint32_t node_idx = run;; node_idx++) {
      new_of[node_idx] = order_count + 1;
      order[order_count++] = node_idx;
      const uint32_t arc = arcs_of[node_idx];
      if (arc != 0) {
        const uint32_t child_run = run_of[arc];
        if (((visited[child_run >> 3] >> (child_run & 7)) & 1U) == 0) {
          kids[kids_count].run = child_run;
          kids[kids_count].position = kids_count;
          kids[kids_count].weight = weight_memo[child_run];
          kids_count++;
        }
      }
      if (is_end[node_idx] != 0) {
        break;
      }
    }
    if (order_mode != 0) {
      qsort(kids, kids_count, sizeof(AcdawgStatsKidEntry),
            acdawg_stats_kid_compare_asc);
    }
    // The stack pops in reverse push order. Mode 0/1 want the first entry
    // (sibling order / smallest weight) processed first, so push back-to-
    // front; mode 2 wants the largest first, so push front-to-back.
    if (order_mode == 2) {
      for (uint32_t kid_idx = 0; kid_idx < kids_count; kid_idx++) {
        stack[stack_top++] = kids[kid_idx].run;
      }
    } else {
      for (uint32_t kid_idx = kids_count; kid_idx-- > 0;) {
        stack[stack_top++] = kids[kid_idx].run;
      }
    }
  }

  uint32_t *new_arcs_of =
      (uint32_t *)calloc_or_die(node_count, sizeof(uint32_t));
  for (uint32_t new_idx = 1; new_idx <= order_count; new_idx++) {
    const uint32_t old_idx = order[new_idx - 1];
    const uint32_t old_arc = arcs_of[old_idx];
    new_arcs_of[new_idx] = (old_arc != 0) ? new_of[old_arc] : 0;
  }

  free(run_of);
  free(weight_memo);
  free(new_of);
  free(order);
  free(visited);
  free(stack);
  free(kids);
  return new_arcs_of;
}

// Sizes one lexicon's acdawg and prints the per-section byte breakdown plus
// the best config found for each what-if variant.
static void acdawg_stats_for_lexicon(const char *lexicon, int max_length) {
  char *set_cmd = get_formatted_string("set -lex %s -wmp false", lexicon);
  Config *config = config_create_or_die(set_cmd);
  free(set_cmd);
  Game *game = config_game_create(config);
  const KWG *lex_kwg = player_get_kwg(game_get_player(game, 0));
  DictionaryWordList *all_words = dictionary_word_list_create();
  kwg_write_words(lex_kwg, kwg_get_dawg_root_node_index(lex_kwg), all_words,
                  NULL);
  DictionaryWordList *words = dictionary_word_list_create();
  for (int word_idx = 0; word_idx < dictionary_word_list_get_count(all_words);
       word_idx++) {
    const DictionaryWord *word =
        dictionary_word_list_get_word(all_words, word_idx);
    if (dictionary_word_get_length(word) <= max_length) {
      dictionary_word_list_add_word(words, dictionary_word_get_word(word),
                                    dictionary_word_get_length(word));
    }
  }
  const int word_count = dictionary_word_list_get_count(words);
  dictionary_word_list_destroy(all_words);

  KWG *reorder_kwg = make_kwg_from_words(words, KWG_MAKER_OUTPUT_DAWG,
                                         KWG_MAKER_MERGE_TAIL_REORDER);
  DawgArcCompressed *dp = dawg_arc_compressed_create_from_kwg(
      reorder_kwg, DAWG_ARC_COMPRESSED_MODE_MIN_RAM);

  printf("\n=== acdawgstats %s (max_length %d): %d words ===\n", lexicon,
         max_length, word_count);
  const uint32_t node_count = dawg_arc_compressed_get_node_count(dp);
  const uint64_t dir_blocks =
      ((uint64_t)node_count + (1ULL << dp->block_bits) - 1) >> dp->block_bits;
  const uint64_t dir_superblocks =
      (dir_blocks + ACDAWG_STATS_SUPERBLOCK_BLOCKS - 1) /
      ACDAWG_STATS_SUPERBLOCK_BLOCKS;
  const uint64_t record_bytes = ((uint64_t)node_count * dp->rec_width + 7) / 8;
  const uint64_t popular_bytes =
      ((uint64_t)dp->popular_count * dp->arc_bits + 7) / 8;
  const uint64_t escape_bytes =
      ((uint64_t)dp->escape_count * dp->arc_bits + 7) / 8;
  const uint64_t anchor_bytes = dir_superblocks * 4;
  const uint64_t delta_bytes = dir_blocks * 2;
  printf("built (MIN_RAM): field=%u K=%u R=%u block=%u escapes=%u (%.1f%% of "
         "nodes) rec_width=%u -> %zu bytes total (%.3f bytes/word)\n",
         dp->field, dp->popular_count, dp->escape_reserve, 1U << dp->block_bits,
         dp->escape_count, 100.0 * dp->escape_count / node_count, dp->rec_width,
         dawg_arc_compressed_get_num_bytes(dp),
         (double)dawg_arc_compressed_get_num_bytes(dp) / word_count);
  printf("  sections: records %llu | popular %llu | escapes %llu | anchors "
         "%llu | deltas %llu (bytes)\n",
         (unsigned long long)record_bytes, (unsigned long long)popular_bytes,
         (unsigned long long)escape_bytes, (unsigned long long)anchor_bytes,
         (unsigned long long)delta_bytes);

  AcdawgStatsGraph graph = acdawg_stats_graph_build(dp);
  printf("  graph: %u nodes, %u arcs, %u no-child; tile_bits=%u arc_bits=%u\n",
         graph.node_count, graph.arc_count, graph.no_child_count,
         graph.tile_bits, graph.arc_bits);

  // Signed-gap histogram over all arcs (before any popular absorption).
  uint64_t pos_hist[ACDAWG_STATS_GAP_BUCKETS] = {0};
  uint64_t neg_hist[ACDAWG_STATS_GAP_BUCKETS] = {0};
  for (uint32_t arc_idx = 0; arc_idx < graph.arc_count; arc_idx++) {
    const int64_t gap = graph.arcs[arc_idx].gap;
    const uint64_t magnitude = (uint64_t)(gap < 0 ? -gap : gap);
    int bucket = 0;
    while ((magnitude >> (bucket + 1)) != 0 &&
           bucket < ACDAWG_STATS_GAP_BUCKETS - 1) {
      bucket++;
    }
    if (gap > 0) {
      pos_hist[bucket]++;
    } else {
      neg_hist[bucket]++;
    }
  }
  printf("  gap log2-magnitude histogram (all arcs; +count / -count):\n");
  for (int bucket = 0; bucket < ACDAWG_STATS_GAP_BUCKETS; bucket++) {
    if (pos_hist[bucket] == 0 && neg_hist[bucket] == 0) {
      continue;
    }
    printf("    2^%-2d %10llu / %llu\n", bucket,
           (unsigned long long)pos_hist[bucket],
           (unsigned long long)neg_hist[bucket]);
  }

  // What-if sweep. K over a K-grid, field over sensible widths; for each
  // variant keep the smallest total. Records: v2 = tile+3+field bits (flag
  // bit), A1/A2 = tile+2+field bits (unified space).
  static const uint32_t k_grid[] = {0,    64,   128,  192,   256,  384,
                                    512,  768,  1024, 1536,  2048, 3072,
                                    4096, 6144, 8192, 12288, 16384};
  enum { ACDAWG_STATS_K_GRID = 17 };
  uint64_t best_bytes_v2 = UINT64_MAX;
  uint64_t best_bytes_a1 = UINT64_MAX;
  uint64_t best_bytes_a1s = UINT64_MAX;
  uint64_t best_bytes_a2 = UINT64_MAX;
  uint64_t best_bytes_a2s = UINT64_MAX;
  char best_desc_v2[128] = "";
  char best_desc_a1[128] = "";
  char best_desc_a1s[128] = "";
  char best_desc_a2[128] = "";
  char best_desc_a2s[128] = "";
  for (uint8_t field = 4; field <= graph.arc_bits; field++) {
    const uint64_t codes = 1ULL << field;
    for (int k_idx = 0; k_idx < ACDAWG_STATS_K_GRID; k_idx++) {
      const uint32_t k = k_grid[k_idx];
      // ---- v2 model: flag bit; gap window = codes - 2 zigzag codes.
      if (k <= codes) {
        const int64_t zig_half = (int64_t)(codes - 2) / 2;
        const uint32_t escapes =
            acdawg_stats_escapes(&graph, k, zig_half, zig_half, NULL);
        const uint64_t total_bits =
            (uint64_t)graph.node_count * (graph.tile_bits + 3 + field) +
            acdawg_stats_v2_overhead_bits(&graph, k, escapes);
        const uint64_t total = (total_bits + 7) / 8;
        if (total < best_bytes_v2) {
          best_bytes_v2 = total;
          (void)snprintf(best_desc_v2, sizeof(best_desc_v2),
                         "field=%u K=%u escapes=%u", field, k, escapes);
        }
      }
      // ---- A1: unified; codes = none + K popular + window + 1 sentinel.
      if ((uint64_t)k + 2 < codes) {
        const int64_t window = (int64_t)(codes - 2 - k);
        const int64_t zig_pos = window / 2;
        const int64_t zig_neg = window - zig_pos;
        const uint32_t escapes =
            acdawg_stats_escapes(&graph, k, zig_pos, zig_neg, NULL);
        const uint64_t total_bits =
            (uint64_t)graph.node_count * (graph.tile_bits + 2 + field) +
            acdawg_stats_v2_overhead_bits(&graph, k, escapes);
        const uint64_t total = (total_bits + 7) / 8;
        if (total < best_bytes_a1) {
          best_bytes_a1 = total;
          (void)snprintf(best_desc_a1, sizeof(best_desc_a1),
                         "field=%u K=%u escapes=%u", field, k, escapes);
        }
        // A1 + asymmetric split.
        int64_t split_pos = 0;
        int64_t split_neg = 0;
        uint32_t split_escapes = 0;
        acdawg_stats_best_split(&graph, k, window, &split_pos, &split_neg,
                                &split_escapes);
        const uint64_t split_bits =
            (uint64_t)graph.node_count * (graph.tile_bits + 2 + field) +
            acdawg_stats_v2_overhead_bits(&graph, k, split_escapes);
        const uint64_t split_total = (split_bits + 7) / 8;
        if (split_total < best_bytes_a1s) {
          best_bytes_a1s = split_total;
          (void)snprintf(best_desc_a1s, sizeof(best_desc_a1s),
                         "field=%u K=%u +%lld/-%lld escapes=%u", field, k,
                         (long long)split_pos, (long long)split_neg,
                         split_escapes);
        }
        // ---- A2: unified + block-local escapes; iterate to a consistent R
        // (window shrinks by R, which can add escapes and grow R). Shares
        // A1's code-space guard, so it lives in the same block.
        uint32_t a2_reserve = 1;
        uint32_t a2_escapes = 0;
        int64_t a2_window = 0;
        bool a2_feasible = false;
        for (int iter = 0; iter < 8; iter++) {
          a2_window = (int64_t)codes - 1 - k - a2_reserve;
          if (a2_window < 0) {
            a2_feasible = false;
            break;
          }
          const int64_t a2_zig_pos = a2_window / 2;
          const int64_t a2_zig_neg = a2_window - a2_zig_pos;
          uint32_t a2_block_max = 0;
          a2_escapes = acdawg_stats_escapes(&graph, k, a2_zig_pos, a2_zig_neg,
                                            &a2_block_max);
          if (a2_block_max <= a2_reserve) {
            a2_feasible = true;
            break;
          }
          a2_reserve = a2_block_max;
        }
        if (a2_feasible) {
          const uint64_t a2_total_bits =
              (uint64_t)graph.node_count * (graph.tile_bits + 2 + field) +
              (uint64_t)k * graph.arc_bits +
              (uint64_t)a2_escapes * graph.arc_bits +
              acdawg_stats_a2_directory_bytes(graph.node_count) * 8;
          const uint64_t a2_total = (a2_total_bits + 7) / 8;
          if (a2_total < best_bytes_a2) {
            best_bytes_a2 = a2_total;
            (void)snprintf(best_desc_a2, sizeof(best_desc_a2),
                           "field=%u K=%u R=%u escapes=%u", field, k,
                           a2_reserve, a2_escapes);
          }
          // A2 + asymmetric split over the same (already-consistent) window.
          int64_t a2_split_pos = 0;
          int64_t a2_split_neg = 0;
          uint32_t a2_split_escapes = 0;
          acdawg_stats_best_split(&graph, k, a2_window, &a2_split_pos,
                                  &a2_split_neg, &a2_split_escapes);
          uint32_t split_block_max = 0;
          (void)acdawg_stats_escapes(&graph, k, a2_split_pos, a2_split_neg,
                                     &split_block_max);
          if (split_block_max <= a2_reserve) {
            const uint64_t a2_split_bits =
                (uint64_t)graph.node_count * (graph.tile_bits + 2 + field) +
                (uint64_t)k * graph.arc_bits +
                (uint64_t)a2_split_escapes * graph.arc_bits +
                acdawg_stats_a2_directory_bytes(graph.node_count) * 8;
            const uint64_t a2_split_total = (a2_split_bits + 7) / 8;
            if (a2_split_total < best_bytes_a2s) {
              best_bytes_a2s = a2_split_total;
              (void)snprintf(best_desc_a2s, sizeof(best_desc_a2s),
                             "field=%u K=%u R=%u +%lld/-%lld escapes=%u", field,
                             k, a2_reserve, (long long)a2_split_pos,
                             (long long)a2_split_neg, a2_split_escapes);
            }
          }
        }
      }
    }
  }
  const uint64_t actual = dawg_arc_compressed_get_num_bytes(dp);
  printf("  what-if best configs (model bytes; v2 is the historical flag-bit "
         "format; the actual build above is v3 ~= A2 positive-only):\n");
  printf("    v2 model : %8llu  %s\n", (unsigned long long)best_bytes_v2,
         best_desc_v2);
  printf("    A1       : %8llu  %s\n", (unsigned long long)best_bytes_a1,
         best_desc_a1);
  printf("    A1+split : %8llu  %s\n", (unsigned long long)best_bytes_a1s,
         best_desc_a1s);
  printf("    A2       : %8llu  %s\n", (unsigned long long)best_bytes_a2,
         best_desc_a2);
  printf("    A2+split : %8llu  %s\n", (unsigned long long)best_bytes_a2s,
         best_desc_a2s);
  printf("    actual v3 build %llu vs v2 model: %+.2f%%\n",
         (unsigned long long)actual,
         100.0 * ((double)actual - (double)best_bytes_v2) /
             (double)best_bytes_v2);

  // ---- Escape-target clustering: could escape entries be narrower than
  // arc_bits with a base offset? Reads the built escape side array directly.
  if (dp->escape_count > 0) {
    uint32_t min_target = UINT32_MAX;
    uint32_t max_target = 0;
    for (uint32_t escape_idx = 0; escape_idx < dp->escape_count; escape_idx++) {
      const uint32_t target = dawg_packed_bits_read(
          dp->bytes, dp->escape_bit_off + (size_t)escape_idx * dp->arc_bits,
          dp->arc_bits);
      if (target < min_target) {
        min_target = target;
      }
      if (target > max_target) {
        max_target = target;
      }
    }
    uint8_t span_bits = 1;
    while (((max_target - min_target) >> span_bits) != 0) {
      span_bits++;
    }
    printf("  escape targets: [%u, %u] span_bits=%u vs arc_bits=%u -> "
           "base-offset entries would save %lld bytes\n",
           min_target, max_target, span_bits, dp->arc_bits,
           (long long)((int64_t)dp->escape_count *
                       ((int64_t)dp->arc_bits - span_bits) / 8));
  }

  // ---- Ordering experiments: rebuild the arc table under weight-ordered
  // DFS numberings and re-run the v3-model search. Mode 0 re-walks the
  // builder's own order and must reproduce it (a self-check of the re-walk).
  {
    const uint32_t node_count_local = graph.node_count;
    uint32_t *arcs_of =
        (uint32_t *)calloc_or_die(node_count_local, sizeof(uint32_t));
    uint8_t *is_end = (uint8_t *)calloc_or_die(node_count_local, 1);
    is_end[0] = 1;
    for (uint32_t node_idx = 1; node_idx < node_count_local; node_idx++) {
      const uint32_t node = dawg_arc_compressed_get_node(dp, node_idx);
      arcs_of[node_idx] = kwg_node_arc_index(node);
      is_end[node_idx] = kwg_node_is_end(node) ? 1 : 0;
    }
    const uint32_t root = dawg_arc_compressed_get_root_index(dp);
    const AcdawgStatsV3Best base = acdawg_stats_best_v3(&graph);
    printf("  ordering (v3 model bytes, coarse K):\n");
    printf("    builder order    : %8llu  field=%u K=%u R=%u block=%u "
           "escapes=%u\n",
           (unsigned long long)base.bytes, base.field, base.k, base.reserve,
           1U << base.block_bits, base.escapes);
    // Mode 0 re-walks in plain sibling order -- the pre-weight-ordering
    // baseline (the builder now orders kids by first-visit weight, so this
    // shows what the ordering is worth rather than reproducing the input).
    static const char *const order_names[] = {
        "sibling-order dfs", "smallest-first   ", "largest-first    ",
        "shared-last      ", "first-visit-size "};
    for (int order_mode = 0; order_mode <= 4; order_mode++) {
      uint32_t *reordered = acdawg_stats_reorder(
          arcs_of, is_end, node_count_local, root, order_mode);
      AcdawgStatsGraph regraph = acdawg_stats_graph_from_arcs(
          reordered, node_count_local, graph.tile_bits, graph.arc_bits);
      const AcdawgStatsV3Best best = acdawg_stats_best_v3(&regraph);
      printf("    %s: %8llu  field=%u K=%u R=%u block=%u escapes=%u "
             "(%+.2f%%)\n",
             order_names[order_mode], (unsigned long long)best.bytes,
             best.field, best.k, best.reserve, 1U << best.block_bits,
             best.escapes,
             100.0 * ((double)best.bytes - (double)base.bytes) /
                 (double)base.bytes);
      free(regraph.arcs);
      free(reordered);
    }

    // ---- Run-based addressing what-if: gaps in run-ordinal units. Denser
    // gaps shrink the field / escape count, but (a) mid-run targets
    // (tail-merge suffix sharing) cannot gap-code at all, and (b) decoding a
    // run gap needs select(run -> node): with the records split into planes
    // the is_end plane doubles as the run bitmap for free, leaving a sampled
    // select table (one node index per 32 runs) as the real overhead -- plus
    // a popcount scan on every gap decode (the common path), so this is a
    // size-only model with a known speed downside.
    {
      uint32_t run_count = 0;
      uint32_t mid_run_arcs = 0;
      AcdawgStatsGraph run_graph = acdawg_stats_run_gap_graph(
          arcs_of, is_end, node_count_local, graph.tile_bits, graph.arc_bits,
          &run_count, &mid_run_arcs);
      const AcdawgStatsV3Best run_best = acdawg_stats_best_v3(&run_graph);
      const uint64_t select_bytes =
          (((uint64_t)run_count + 31) / 32 * graph.arc_bits + 7) / 8;
      printf("  run addressing: %u runs (%.2f nodes/run, run_bits=%u), "
             "mid-run arc targets %u/%u (%.1f%%)\n",
             run_count, (double)node_count_local / run_count,
             acdawg_stats_bits_needed(run_count), mid_run_arcs,
             run_graph.arc_count, 100.0 * mid_run_arcs / run_graph.arc_count);
      printf(
          "    best run-gap config: %llu + select %llu = %llu bytes "
          "(%+.2f%% vs node-gap %llu)  field=%u K=%u R=%u escapes=%u\n",
          (unsigned long long)run_best.bytes, (unsigned long long)select_bytes,
          (unsigned long long)(run_best.bytes + select_bytes),
          100.0 *
              ((double)(run_best.bytes + select_bytes) - (double)base.bytes) /
              (double)base.bytes,
          (unsigned long long)base.bytes, run_best.field, run_best.k,
          run_best.reserve, run_best.escapes);
      free(run_graph.arcs);
    }
    free(arcs_of);
    free(is_end);
  }
  (void)fflush(stdout);

  free(graph.arcs);
  dawg_arc_compressed_destroy(dp);
  kwg_destroy(reorder_kwg);
  dictionary_word_list_destroy(words);
  game_destroy(game);
  config_destroy(config);
}

void test_dawg_arc_compressed_stats(void) {
  // Smallest corpus first so a failure surfaces fast.
  acdawg_stats_for_lexicon("CSW24", 8);
  acdawg_stats_for_lexicon("OSPS49", 8);
  acdawg_stats_for_lexicon("CSW24", BOARD_DIM);
}

// ---------------------------------------------------------------------------
// On-demand traversal benchmark (acdawgbench): time word lookups and a full
// DFS enumeration on the packed DAWG vs the arc-compressed DAWG (both modes),
// so a size change to the acdawg encoding can be checked for speed regressions
// against the fixed-width baseline on the same corpus.
// ---------------------------------------------------------------------------

enum {
  ACDAWG_BENCH_LOOKUP_ROUNDS = 20, // repeat lookups so timings are stable
};

// Walks one word from the packed DAWG's root; true if accepted.
static bool acdawg_bench_packed_lookup(const DawgPacked *dp,
                                       const DictionaryWord *word) {
  const uint8_t *letters = dictionary_word_get_word(word);
  const int length = dictionary_word_get_length(word);
  uint32_t node_index = dawg_packed_get_root_index(dp);
  bool accepts = false;
  for (int letter_idx = 0; letter_idx < length; letter_idx++) {
    if (node_index == 0) {
      return false;
    }
    const MachineLetter ml = letters[letter_idx];
    bool found = false;
    for (uint32_t scan_idx = node_index;; scan_idx++) {
      const uint32_t node = dawg_packed_get_node(dp, scan_idx);
      if ((MachineLetter)kwg_node_tile(node) == ml) {
        node_index = kwg_node_arc_index(node);
        accepts = kwg_node_accepts(node);
        found = true;
        break;
      }
      if (kwg_node_is_end(node)) {
        break;
      }
    }
    if (!found) {
      return false;
    }
  }
  return accepts;
}

// Walks one word from the arc-compressed DAWG's root; true if accepted.
static bool acdawg_bench_compressed_lookup(const DawgArcCompressed *dp,
                                           const DictionaryWord *word) {
  const uint8_t *letters = dictionary_word_get_word(word);
  const int length = dictionary_word_get_length(word);
  uint32_t node_index = dawg_arc_compressed_get_root_index(dp);
  bool accepts = false;
  for (int letter_idx = 0; letter_idx < length; letter_idx++) {
    if (node_index == 0) {
      return false;
    }
    const MachineLetter ml = letters[letter_idx];
    bool found = false;
    for (uint32_t scan_idx = node_index;; scan_idx++) {
      const uint32_t node = dawg_arc_compressed_get_node(dp, scan_idx);
      if ((MachineLetter)kwg_node_tile(node) == ml) {
        node_index = kwg_node_arc_index(node);
        accepts = kwg_node_accepts(node);
        found = true;
        break;
      }
      if (kwg_node_is_end(node)) {
        break;
      }
    }
    if (!found) {
      return false;
    }
  }
  return accepts;
}

void test_dawg_arc_compressed_bench(void) {
  const char *lexicon = "CSW24";
  char *set_cmd = get_formatted_string("set -lex %s -wmp false", lexicon);
  Config *config = config_create_or_die(set_cmd);
  free(set_cmd);
  Game *game = config_game_create(config);
  const KWG *lex_kwg = player_get_kwg(game_get_player(game, 0));
  DictionaryWordList *words = dictionary_word_list_create();
  kwg_write_words(lex_kwg, kwg_get_dawg_root_node_index(lex_kwg), words, NULL);
  const int word_count = dictionary_word_list_get_count(words);

  KWG *reorder_kwg = make_kwg_from_words(words, KWG_MAKER_OUTPUT_DAWG,
                                         KWG_MAKER_MERGE_TAIL_REORDER);
  DawgPacked *packed =
      dawg_packed_create_from_kwg(reorder_kwg, /*prefer_byte_alignment=*/false);
  DawgArcCompressed *min_ram = dawg_arc_compressed_create_from_kwg(
      reorder_kwg, DAWG_ARC_COMPRESSED_MODE_MIN_RAM);
  DawgArcCompressed *balanced = dawg_arc_compressed_create_from_kwg(
      reorder_kwg, DAWG_ARC_COMPRESSED_MODE_BALANCED);

  printf("\n=== acdawgbench %s: %d words, %d lookup rounds ===\n", lexicon,
         word_count, ACDAWG_BENCH_LOOKUP_ROUNDS);
  printf("sizes: packed %zu | acdawg MIN_RAM %zu | acdawg BALANCED %zu bytes\n",
         dawg_packed_get_node_bytes(packed),
         dawg_arc_compressed_get_num_bytes(min_ram),
         dawg_arc_compressed_get_num_bytes(balanced));

  // Word lookup: every word, ACDAWG_BENCH_LOOKUP_ROUNDS times per format.
  Timer timer;
  ctimer_start(&timer);
  uint64_t packed_found = 0;
  for (int round = 0; round < ACDAWG_BENCH_LOOKUP_ROUNDS; round++) {
    for (int word_idx = 0; word_idx < word_count; word_idx++) {
      packed_found += acdawg_bench_packed_lookup(
          packed, dictionary_word_list_get_word(words, word_idx));
    }
  }
  const double packed_lookup = ctimer_elapsed_seconds(&timer);

  ctimer_start(&timer);
  uint64_t min_ram_found = 0;
  for (int round = 0; round < ACDAWG_BENCH_LOOKUP_ROUNDS; round++) {
    for (int word_idx = 0; word_idx < word_count; word_idx++) {
      min_ram_found += acdawg_bench_compressed_lookup(
          min_ram, dictionary_word_list_get_word(words, word_idx));
    }
  }
  const double min_ram_lookup = ctimer_elapsed_seconds(&timer);

  ctimer_start(&timer);
  uint64_t balanced_found = 0;
  for (int round = 0; round < ACDAWG_BENCH_LOOKUP_ROUNDS; round++) {
    for (int word_idx = 0; word_idx < word_count; word_idx++) {
      balanced_found += acdawg_bench_compressed_lookup(
          balanced, dictionary_word_list_get_word(words, word_idx));
    }
  }
  const double balanced_lookup = ctimer_elapsed_seconds(&timer);

  // Every word must be found by every format.
  const uint64_t expected_found =
      (uint64_t)word_count * ACDAWG_BENCH_LOOKUP_ROUNDS;
  assert(packed_found == expected_found);
  assert(min_ram_found == expected_found);
  assert(balanced_found == expected_found);

  const double lookups = (double)word_count * ACDAWG_BENCH_LOOKUP_ROUNDS;
  printf("word lookup ns/word: packed %.1f | MIN_RAM %.1f (%.2fx) | BALANCED "
         "%.1f (%.2fx)\n",
         1e9 * packed_lookup / lookups, 1e9 * min_ram_lookup / lookups,
         min_ram_lookup / packed_lookup, 1e9 * balanced_lookup / lookups,
         balanced_lookup / packed_lookup);

  // Full DFS enumeration (the rack-generation-shaped access pattern).
  ctimer_start(&timer);
  DictionaryWordList *packed_words = dictionary_word_list_create();
  dawg_packed_write_words(packed, packed_words);
  const double packed_enum = ctimer_elapsed_seconds(&timer);
  ctimer_start(&timer);
  DictionaryWordList *min_ram_words = dictionary_word_list_create();
  dawg_arc_compressed_write_words(min_ram, min_ram_words);
  const double min_ram_enum = ctimer_elapsed_seconds(&timer);
  ctimer_start(&timer);
  DictionaryWordList *balanced_words = dictionary_word_list_create();
  dawg_arc_compressed_write_words(balanced, balanced_words);
  const double balanced_enum = ctimer_elapsed_seconds(&timer);
  assert(dictionary_word_list_get_count(packed_words) == word_count);
  assert(dictionary_word_list_get_count(min_ram_words) == word_count);
  assert(dictionary_word_list_get_count(balanced_words) == word_count);
  printf("full enumeration ms: packed %.1f | MIN_RAM %.1f (%.2fx) | BALANCED "
         "%.1f (%.2fx)\n",
         1e3 * packed_enum, 1e3 * min_ram_enum, min_ram_enum / packed_enum,
         1e3 * balanced_enum, balanced_enum / packed_enum);
  (void)fflush(stdout);

  dictionary_word_list_destroy(packed_words);
  dictionary_word_list_destroy(min_ram_words);
  dictionary_word_list_destroy(balanced_words);
  dawg_packed_destroy(packed);
  dawg_arc_compressed_destroy(min_ram);
  dawg_arc_compressed_destroy(balanced);
  kwg_destroy(reorder_kwg);
  dictionary_word_list_destroy(words);
  game_destroy(game);
  config_destroy(config);
}
