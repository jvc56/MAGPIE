// Build the flat lexicon binary file from a KWG. See flat_lex_maker.h for the
// format spec.

#include "flat_lex_maker.h"

#include "../def/kwg_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../ent/bit_rack.h"
#include "../ent/dictionary_word.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FLAT_LEX_MAGIC_0 'F'
#define FLAT_LEX_MAGIC_1 'L'
#define FLAT_LEX_MAGIC_2 'E'
#define FLAT_LEX_MAGIC_3 'X'
#define FLAT_LEX_VERSION 1
#define FLAT_LEX_BITRACK_BYTES 16

// Recursively walk the DAWG, emitting every accepted word into buckets[L]
// where L is the word length.
static void walk_dawg(const KWG *kwg, uint32_t node_index,
                      MachineLetter *prefix, int depth,
                      DictionaryWordList **buckets, int max_len_plus_one) {
  if (node_index == 0 || depth >= max_len_plus_one - 1) {
    return;
  }
  for (uint32_t i = node_index;; i++) {
    const uint32_t node = kwg_node(kwg, i);
    const MachineLetter ml = (MachineLetter)kwg_node_tile(node);
    prefix[depth] = ml;
    if (kwg_node_accepts(node)) {
      const int word_length = depth + 1;
      dictionary_word_list_add_word(buckets[word_length], prefix, word_length);
    }
    const uint32_t child = kwg_node_arc_index(node);
    if (child != 0) {
      walk_dawg(kwg, child, prefix, depth + 1, buckets, max_len_plus_one);
    }
    if (kwg_node_is_end(node)) {
      break;
    }
  }
}

static void write_bitrack_le(uint8_t *dst, const BitRack *br) {
  // BitRack is already stored low-then-high in little-endian builds; on
  // little-endian hosts we can memcpy.
  memcpy(dst, br, FLAT_LEX_BITRACK_BYTES);
}

void flat_lex_build(const KWG *kwg, const LetterDistribution *ld,
                    uint8_t **out_bytes, size_t *out_size,
                    ErrorStack *error_stack) {
  *out_bytes = NULL;
  *out_size = 0;
  if (!bit_rack_is_compatible_with_ld(ld)) {
    error_stack_push(error_stack, ERROR_STATUS_LD_LEXICON_DEFAULT_NOT_FOUND,
                     string_duplicate("BitRack incompatible with this LD"));
    return;
  }
  const int max_len_plus_one = MAX_KWG_STRING_LENGTH;
  const int alphabet_size = ld_get_size(ld);

  DictionaryWordList **buckets = (DictionaryWordList **)malloc_or_die(
      sizeof(DictionaryWordList *) * (size_t)max_len_plus_one);
  for (int len = 0; len < max_len_plus_one; len++) {
    buckets[len] = dictionary_word_list_create();
  }

  MachineLetter prefix[MAX_KWG_STRING_LENGTH];
  const uint32_t root = kwg_get_dawg_root_node_index(kwg);
  walk_dawg(kwg, root, prefix, 0, buckets, max_len_plus_one);

  uint32_t total_words = 0;
  for (int len = 0; len < max_len_plus_one; len++) {
    if (dictionary_word_list_get_count(buckets[len]) > 0) {
      dictionary_word_list_sort(buckets[len]);
    }
    total_words += (uint32_t)dictionary_word_list_get_count(buckets[len]);
  }

  // Build output buffer in memory then write once.
  const size_t header_bytes = 16;
  const size_t counts_bytes = (size_t)max_len_plus_one * 4;
  const size_t bitracks_bytes = (size_t)total_words * FLAT_LEX_BITRACK_BYTES;
  size_t letters_bytes = 0;
  for (int len = 0; len < max_len_plus_one; len++) {
    letters_bytes +=
        (size_t)dictionary_word_list_get_count(buckets[len]) * (size_t)len;
  }
  const size_t total_bytes =
      header_bytes + counts_bytes + bitracks_bytes + letters_bytes;
  uint8_t *buf = (uint8_t *)malloc_or_die(total_bytes);

  // Header
  buf[0] = FLAT_LEX_MAGIC_0;
  buf[1] = FLAT_LEX_MAGIC_1;
  buf[2] = FLAT_LEX_MAGIC_2;
  buf[3] = FLAT_LEX_MAGIC_3;
  buf[4] = FLAT_LEX_VERSION;
  buf[5] = (uint8_t)alphabet_size;
  buf[6] = (uint8_t)max_len_plus_one;
  buf[7] = FLAT_LEX_BITRACK_BYTES;
  buf[8] = (uint8_t)(total_words & 0xff);
  buf[9] = (uint8_t)((total_words >> 8) & 0xff);
  buf[10] = (uint8_t)((total_words >> 16) & 0xff);
  buf[11] = (uint8_t)((total_words >> 24) & 0xff);
  buf[12] = 0;
  buf[13] = 0;
  buf[14] = 0;
  buf[15] = 0;

  // Count table
  uint8_t *p = buf + header_bytes;
  for (int len = 0; len < max_len_plus_one; len++) {
    const uint32_t c = (uint32_t)dictionary_word_list_get_count(buckets[len]);
    p[0] = (uint8_t)(c & 0xff);
    p[1] = (uint8_t)((c >> 8) & 0xff);
    p[2] = (uint8_t)((c >> 16) & 0xff);
    p[3] = (uint8_t)((c >> 24) & 0xff);
    p += 4;
  }

  // BitRacks block (all words, ordered length-major)
  uint8_t *bitracks_dst = p;
  for (int len = 0; len < max_len_plus_one; len++) {
    const int n = dictionary_word_list_get_count(buckets[len]);
    for (int i = 0; i < n; i++) {
      const DictionaryWord *dw = dictionary_word_list_get_word(buckets[len], i);
      const BitRack br = bit_rack_create_from_dictionary_word(dw);
      write_bitrack_le(bitracks_dst, &br);
      bitracks_dst += FLAT_LEX_BITRACK_BYTES;
    }
  }

  // Letters block
  uint8_t *letters_dst = bitracks_dst;
  for (int len = 0; len < max_len_plus_one; len++) {
    const int n = dictionary_word_list_get_count(buckets[len]);
    for (int i = 0; i < n; i++) {
      const DictionaryWord *dw = dictionary_word_list_get_word(buckets[len], i);
      memcpy(letters_dst, dictionary_word_get_word(dw), (size_t)len);
      letters_dst += (size_t)len;
    }
  }

  for (int len = 0; len < max_len_plus_one; len++) {
    dictionary_word_list_destroy(buckets[len]);
  }
  free(buckets);

  *out_bytes = buf;
  *out_size = total_bytes;
}

void flat_lex_make(const KWG *kwg, const LetterDistribution *ld,
                   const char *output_path, ErrorStack *error_stack) {
  uint8_t *buf = NULL;
  size_t total_bytes = 0;
  flat_lex_build(kwg, ld, &buf, &total_bytes, error_stack);
  if (buf == NULL) {
    return;
  }
  FILE *fp = fopen(output_path, "wb");
  if (fp == NULL) {
    free(buf);
    error_stack_push(error_stack, ERROR_STATUS_RW_WRITE_ERROR,
                     string_duplicate(output_path));
    return;
  }
  const size_t written = fwrite(buf, 1, total_bytes, fp);
  fclose(fp);
  free(buf);
  if (written != total_bytes) {
    error_stack_push(error_stack, ERROR_STATUS_RW_WRITE_ERROR,
                     string_duplicate(output_path));
  }
}
