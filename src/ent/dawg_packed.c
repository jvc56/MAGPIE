#include "dawg_packed.h"

#include "../def/board_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../util/fileproxy.h"
#include "../util/io_util.h"
#include "dictionary_word.h"
#include "kwg.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Number of bits needed to represent every value in [0, max_value].
static inline uint8_t bits_needed(uint32_t max_value) {
  uint8_t bits = 1;
  while ((max_value >> bits) != 0) {
    bits++;
  }
  return bits;
}

DawgPacked *dawg_packed_create_from_kwg(const KWG *kwg,
                                        bool prefer_byte_alignment) {
  const int node_count = kwg_get_number_of_nodes(kwg);
  uint32_t max_tile = 0;
  for (int node_idx = 0; node_idx < node_count; node_idx++) {
    const uint32_t tile = kwg_node_tile(kwg_node(kwg, (uint32_t)node_idx));
    if (tile > max_tile) {
      max_tile = tile;
    }
  }
  const uint8_t tile_bits = bits_needed(max_tile);
  // Arc indices and the root index all fall in [0, node_count - 1].
  const uint8_t arc_bits = bits_needed((uint32_t)(node_count - 1));
  const uint8_t raw_width = (uint8_t)(tile_bits + 2 + arc_bits);
  uint8_t stored_width = raw_width;
  if (prefer_byte_alignment) {
    stored_width = (uint8_t)((raw_width + 7U) & ~7U);
  }
  const bool byte_aligned = (stored_width % 8U) == 0U;

  const size_t total_bits = (size_t)node_count * stored_width;
  const size_t node_bytes = (total_bits + 7U) / 8U;
  uint8_t *node_bits = (uint8_t *)calloc_or_die(node_bytes, 1);

  for (int node_idx = 0; node_idx < node_count; node_idx++) {
    const uint32_t node = kwg_node(kwg, (uint32_t)node_idx);
    const uint32_t arc = kwg_node_arc_index(node);
    const uint32_t is_end = kwg_node_is_end(node) ? 1U : 0U;
    const uint32_t accepts = kwg_node_accepts(node) ? 1U : 0U;
    const uint32_t tile = kwg_node_tile(node);
    const uint32_t value = arc | (is_end << arc_bits) |
                           (accepts << (arc_bits + 1)) |
                           (tile << (arc_bits + 2));
    dawg_packed_bits_write(node_bits, (size_t)node_idx * stored_width,
                           raw_width, value);
  }

  DawgPacked *dp = (DawgPacked *)malloc_or_die(sizeof(DawgPacked));
  dp->node_bits = node_bits;
  dp->node_bytes = node_bytes;
  dp->node_count = (uint32_t)node_count;
  dp->root_index = kwg_get_dawg_root_node_index(kwg);
  dp->tile_bits = tile_bits;
  dp->arc_bits = arc_bits;
  dp->stored_width = stored_width;
  dp->byte_aligned = byte_aligned;
  return dp;
}

void dawg_packed_destroy(DawgPacked *dp) {
  if (dp == NULL) {
    return;
  }
  free(dp->node_bits);
  free(dp);
}

void dawg_packed_write_to_file(const DawgPacked *dp, const char *filename,
                               ErrorStack *error_stack) {
  FILE *stream = fopen_safe(filename, "wb", error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
  uint8_t header[DAWG_PACKED_HEADER_BYTES];
  memset(header, 0, sizeof(header));
  memcpy(header, DAWG_PACKED_MAGIC, 4);
  header[4] = DAWG_PACKED_VERSION;
  header[5] = dp->tile_bits;
  header[6] = dp->arc_bits;
  header[7] = dp->stored_width;
  header[8] = dp->byte_aligned ? DAWG_PACKED_FLAG_BYTE_ALIGNED : 0;
  // header[9..11] reserved padding (already zero). Multi-byte fields are
  // stored in the writer's native byte order: a packed DAWG is meant to be
  // read back on the same machine, not transported across endiannesses.
  memcpy(header + 12, &dp->node_count, sizeof(uint32_t));
  memcpy(header + 16, &dp->root_index, sizeof(uint32_t));
  fwrite_or_die(header, 1, sizeof(header), stream, "packed dawg header");
  fwrite_or_die(dp->node_bits, 1, dp->node_bytes, stream, "packed dawg nodes");
  fclose_or_die(stream);
}

DawgPacked *dawg_packed_read_from_file(const char *filename,
                                       ErrorStack *error_stack) {
  FILE *stream = stream_from_filename(filename, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return NULL;
  }
  uint8_t header[DAWG_PACKED_HEADER_BYTES];
  if (fread(header, 1, sizeof(header), stream) != sizeof(header) ||
      memcmp(header, DAWG_PACKED_MAGIC, 4) != 0 ||
      header[4] != DAWG_PACKED_VERSION) {
    fclose_or_die(stream);
    error_stack_push(error_stack, ERROR_STATUS_CONVERT_MALFORMED_KWG,
                     get_formatted_string(
                         "malformed packed dawg header in file: %s", filename));
    return NULL;
  }
  DawgPacked *dp = (DawgPacked *)malloc_or_die(sizeof(DawgPacked));
  dp->tile_bits = header[5];
  dp->arc_bits = header[6];
  dp->stored_width = header[7];
  dp->byte_aligned = (header[8] & DAWG_PACKED_FLAG_BYTE_ALIGNED) != 0;
  memcpy(&dp->node_count, header + 12, sizeof(uint32_t));
  memcpy(&dp->root_index, header + 16, sizeof(uint32_t));

  const size_t total_bits = (size_t)dp->node_count * dp->stored_width;
  dp->node_bytes = (total_bits + 7U) / 8U;
  dp->node_bits = (uint8_t *)malloc_or_die(dp->node_bytes);
  if (fread(dp->node_bits, 1, dp->node_bytes, stream) != dp->node_bytes) {
    fclose_or_die(stream);
    dawg_packed_destroy(dp);
    error_stack_push(
        error_stack, ERROR_STATUS_CONVERT_MALFORMED_KWG,
        get_formatted_string("truncated packed dawg node data in file: %s",
                             filename));
    return NULL;
  }
  fclose_or_die(stream);
  return dp;
}

static void dawg_packed_write_words_aux(const DawgPacked *dp,
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
    const uint32_t node = dawg_packed_get_node(dp, node_idx);
    const MachineLetter ml = (MachineLetter)kwg_node_tile(node);
    const uint32_t arc = kwg_node_arc_index(node);
    const bool node_accepts = kwg_node_accepts(node);
    if (prefix_length < BOARD_DIM) {
      prefix[prefix_length] = ml;
    }
    dawg_packed_write_words_aux(dp, arc, prefix, prefix_length + 1,
                                node_accepts, words);
    if (kwg_node_is_end(node)) {
      break;
    }
  }
}

void dawg_packed_write_words(const DawgPacked *dp, DictionaryWordList *words) {
  MachineLetter prefix[BOARD_DIM];
  dawg_packed_write_words_aux(dp, dp->root_index, prefix, 0, false, words);
}
