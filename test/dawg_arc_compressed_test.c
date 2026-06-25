#include "../src/def/kwg_defs.h"
#include "../src/ent/dawg_arc_compressed.h"
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

void test_dawg_arc_compressed(void) {
  Config *config = config_create_or_die("set -lex CSW21");
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
  printf("arc-compressed dawg: %u nodes, tile_bits=%u arc_bits=%u field=%u "
         "rec_width=%u, popular=%u escapes=%u -> %zu bytes (vs %d bytes at 32 "
         "bits, %.2f%%)\n",
         dawg_arc_compressed_get_node_count(dp), dp->tile_bits, dp->arc_bits,
         dp->field, dp->rec_width, dp->popular_count, dp->escape_count,
         dawg_arc_compressed_get_num_bytes(dp), reorder_node_count * 4,
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
  assert(dawg_arc_compressed_get_num_bytes(loaded) ==
         dawg_arc_compressed_get_num_bytes(dp));
  assert(memcmp(loaded->bytes, dp->bytes, dp->num_bytes) == 0);

  DictionaryWordList *decoded_loaded = dictionary_word_list_create();
  dawg_arc_compressed_write_words(loaded, decoded_loaded);
  DictionaryWordList *expected_loaded = copy_word_list(words);
  assert_same_word_set(expected_loaded, decoded_loaded);

  assert_rejects_bad_header();

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
