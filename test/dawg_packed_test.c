#include "../src/def/kwg_defs.h"
#include "../src/ent/dawg_packed.h"
#include "../src/ent/dictionary_word.h"
#include "../src/ent/game.h"
#include "../src/ent/kwg.h"
#include "../src/ent/player.h"
#include "../src/impl/config.h"
#include "../src/impl/kwg_maker.h"
#include "../src/util/io_util.h"
#include "test_util.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  // NWL23 capped at 8 letters: small enough that the byte-aligned strategy
  // lands on sub-32-bit nodes (the retro target), and a realistic word list
  // for fitting onto constrained hardware.
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

// Verifies that a packed DAWG built in `prefer_byte_alignment` mode encodes the
// reorder DAWG losslessly: every decoded node matches the source KWG, the
// blob survives a file round-trip, and the word set is preserved.
static void check_packed_mode(const KWG *reorder_kwg,
                              const DictionaryWordList *words,
                              bool prefer_byte_alignment) {
  const int node_count = kwg_get_number_of_nodes(reorder_kwg);
  DawgPacked *dp =
      dawg_packed_create_from_kwg(reorder_kwg, prefer_byte_alignment);

  printf("packed dawg (%s): %d nodes, tile_bits=%u arc_bits=%u stored_width=%u "
         "-> %zu bytes (vs %d bytes at 32 bits, %.2f%%)\n",
         prefer_byte_alignment ? "byte-aligned" : "bit-packed", node_count,
         dp->tile_bits, dp->arc_bits, dp->stored_width,
         dawg_packed_get_node_bytes(dp), node_count * 4,
         100.0 * (double)dawg_packed_get_node_bytes(dp) / (node_count * 4));

  // Decoded nodes must be identical to the source 32-bit KWG nodes.
  for (int node_idx = 0; node_idx < node_count; node_idx++) {
    assert(dawg_packed_get_node(dp, (uint32_t)node_idx) ==
           kwg_node(reorder_kwg, (uint32_t)node_idx));
  }

  // File round-trip.
  const char *filename = "dawg_packed_round_trip_test.pdawg";
  ErrorStack *error_stack = error_stack_create();
  dawg_packed_write_to_file(dp, filename, error_stack);
  assert(error_stack_is_empty(error_stack));
  DawgPacked *loaded = dawg_packed_read_from_file(filename, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(loaded != NULL);
  assert(loaded->node_count == dp->node_count);
  assert(loaded->root_index == dp->root_index);
  assert(loaded->tile_bits == dp->tile_bits);
  assert(loaded->arc_bits == dp->arc_bits);
  assert(loaded->stored_width == dp->stored_width);
  assert(loaded->byte_aligned == dp->byte_aligned);
  assert(dawg_packed_get_node_bytes(loaded) == dawg_packed_get_node_bytes(dp));
  assert(memcmp(loaded->node_bits, dp->node_bits, dp->node_bytes) == 0);

  // Word set must round-trip through the loaded packed DAWG.
  DictionaryWordList *decoded = dictionary_word_list_create();
  dawg_packed_write_words(loaded, decoded);
  DictionaryWordList *expected = dictionary_word_list_create();
  for (int word_idx = 0; word_idx < dictionary_word_list_get_count(words);
       word_idx++) {
    const DictionaryWord *word = dictionary_word_list_get_word(words, word_idx);
    dictionary_word_list_add_word(expected, dictionary_word_get_word(word),
                                  dictionary_word_get_length(word));
  }
  assert_same_word_set(expected, decoded);

  (void)remove(filename);
  dictionary_word_list_destroy(expected);
  dictionary_word_list_destroy(decoded);
  dawg_packed_destroy(loaded);
  error_stack_destroy(error_stack);
  dawg_packed_destroy(dp);
}

// A corrupt header (here, an out-of-range arc_bits) must be rejected cleanly
// rather than triggering a huge allocation or an out-of-range shift.
static void assert_rejects_bad_header(void) {
  const char *filename = "dawg_packed_bad_header_test.pdawg";
  uint8_t header[DAWG_PACKED_HEADER_BYTES];
  memset(header, 0, sizeof(header));
  memcpy(header, DAWG_PACKED_MAGIC, 4);
  header[4] = DAWG_PACKED_VERSION;
  header[5] = 5;  // tile_bits
  header[6] = 99; // arc_bits: out of range
  header[7] = 24; // stored_width
  ErrorStack *error_stack = error_stack_create();
  FILE *stream = fopen_safe(filename, "wb", error_stack);
  assert(error_stack_is_empty(error_stack));
  fwrite_or_die(header, 1, sizeof(header), stream, "bad packed dawg header");
  fclose_or_die(stream);

  const DawgPacked *dp = dawg_packed_read_from_file(filename, error_stack);
  assert(dp == NULL);
  assert(!error_stack_is_empty(error_stack));
  error_stack_destroy(error_stack);
  (void)remove(filename);
}

void test_dawg_packed(void) {
  Config *config = config_create_or_die("set -lex NWL23");
  Game *game = config_game_create(config);
  const KWG *nwl_kwg = player_get_kwg(game_get_player(game, 0));
  DictionaryWordList *all_words = dictionary_word_list_create();
  kwg_write_words(nwl_kwg, kwg_get_dawg_root_node_index(nwl_kwg), all_words,
                  NULL);

  // Cap word length to emulate a constrained-hardware word list.
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

  // Both strategies must beat the 32-bit-per-node baseline for this corpus,
  // and the byte-aligned strategy must produce whole-byte nodes.
  const int node_count = kwg_get_number_of_nodes(reorder_kwg);
  DawgPacked *bit_packed = dawg_packed_create_from_kwg(reorder_kwg, false);
  assert(dawg_packed_get_node_bytes(bit_packed) < (size_t)node_count * 4);
  dawg_packed_destroy(bit_packed);
  DawgPacked *byte_aligned = dawg_packed_create_from_kwg(reorder_kwg, true);
  assert(byte_aligned->byte_aligned);
  assert((byte_aligned->stored_width % 8U) == 0U);
  assert(dawg_packed_get_node_bytes(byte_aligned) < (size_t)node_count * 4);
  dawg_packed_destroy(byte_aligned);

  check_packed_mode(reorder_kwg, words, false);
  check_packed_mode(reorder_kwg, words, true);
  assert_rejects_bad_header();

  dictionary_word_list_destroy(words);
  kwg_destroy(reorder_kwg);
  game_destroy(game);
  config_destroy(config);
}
