#include "rack_info_table_test.h"

#include "../src/def/rack_defs.h"
#include "../src/ent/bit_rack.h"
#include "../src/ent/data_filepaths.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/klv.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/rack_info_table.h"
#include "../src/ent/wmp.h"
#include "../src/impl/config.h"
#include "../src/impl/rack_info_table_maker.h"
#include "../src/util/io_util.h"
#include "test_util.h"
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>

// Count the number of distinct RACK_SIZE-tile multisets drawable from ld.
static uint32_t count_racks_for_ld(const LetterDistribution *ld, int ml,
                                   int remaining) {
  if (remaining == 0) {
    return 1;
  }
  if (ml >= ld_get_size(ld)) {
    return 0;
  }
  int max_this = ld_get_dist(ld, ml);
  if (max_this > remaining) {
    max_this = remaining;
  }
  uint32_t total = 0;
  for (int num = 0; num <= max_this; num++) {
    total += count_racks_for_ld(ld, ml + 1, remaining - num);
  }
  return total;
}

static void assert_rits_equal(const RackInfoTable *a, const RackInfoTable *b) {
  assert(a->version == b->version);
  assert(a->rack_size == b->rack_size);
  assert(a->num_buckets == b->num_buckets);
  assert(a->num_entries == b->num_entries);
  for (uint32_t bucket_idx = 0; bucket_idx <= a->num_buckets; bucket_idx++) {
    assert(a->bucket_starts[bucket_idx] == b->bucket_starts[bucket_idx]);
  }
  for (uint32_t entry_idx = 0; entry_idx < a->num_entries; entry_idx++) {
    const RackInfoTableEntry *ea = &a->entries[entry_idx];
    const RackInfoTableEntry *eb = &b->entries[entry_idx];
    for (int leaf_idx = 0; leaf_idx < RACK_INFO_TABLE_LEAVES_PER_ENTRY;
         leaf_idx++) {
      assert(ea->leaves[leaf_idx] == eb->leaves[leaf_idx]);
    }
    for (int byte_idx = 0; byte_idx < RACK_INFO_TABLE_BITRACK_BYTES;
         byte_idx++) {
      assert(ea->bit_rack_bytes[byte_idx] == eb->bit_rack_bytes[byte_idx]);
    }
  }
}

void test_rack_info_table(void) {
  // Use CSW21 with the english_ab distribution (50 A, 50 B, 0 blank).
  // This keeps the number of full racks small (8, for 0A+7B through 7A+0B)
  // while still exercising the full build path with a real KLV.
  Config *config = config_create_or_die(
      "set -lex CSW21 -ld english_ab -k1 CSW21 -k2 CSW21");
  const LetterDistribution *ld = config_get_ld(config);
  Game *game = config_game_create(config);
  const Player *player = game_get_player(game, 0);
  const KLV *klv = player_get_klv(player);
  const WMP *wmp = player_get_wmp(player);
  assert(klv != NULL);

  // Build the rack info table.
  RackInfoTable *rit = make_rack_info_table(klv, wmp, ld, 1);
  assert(rit != NULL);
  assert(rit->version == RIT_VERSION);
  assert(rit->rack_size == (uint8_t)RACK_SIZE);

  // The number of entries should equal the number of possible
  // RACK_SIZE-tile multisets from the distribution.
  const uint32_t expected_count = count_racks_for_ld(ld, 0, RACK_SIZE);
  assert(rit->num_entries == expected_count);

  // Num buckets should be a power of 2 >= num_entries.
  assert((rit->num_buckets & (rit->num_buckets - 1)) == 0);
  assert(rit->num_buckets >= rit->num_entries);

  // Build an arbitrary RACK_SIZE-tile rack that exists in the distribution.
  Rack test_rack;
  rack_set_dist_size(&test_rack, ld_get_size(ld));
  rack_reset(&test_rack);
  int remaining = RACK_SIZE;
  for (int ml = 0; ml < ld_get_size(ld) && remaining > 0; ml++) {
    int max_this = ld_get_dist(ld, ml);
    int take = (max_this < remaining) ? max_this : remaining;
    for (int i = 0; i < take; i++) {
      rack_add_letter(&test_rack, ml);
    }
    remaining -= take;
  }
  assert(remaining == 0);

  // Look up the rack in the rack info table; it must exist.
  const BitRack test_bit_rack = bit_rack_create_from_rack(ld, &test_rack);
  const RackInfoTableEntry *entry =
      rack_info_table_lookup(rit, &test_bit_rack);
  assert(entry != NULL);
  // Index 0 (empty leave) must be 0.
  assert(entry->leaves[0] == 0);

  // Convenience accessor should return the same array.
  const Equity *leaves_via_accessor =
      rack_info_table_lookup_leaves(rit, &test_bit_rack);
  assert(leaves_via_accessor == entry->leaves);

  // Verify every entry we enumerate can be looked up.
  for (uint32_t entry_idx = 0; entry_idx < rit->num_entries; entry_idx++) {
    const RackInfoTableEntry *stored = &rit->entries[entry_idx];
    const BitRack entry_bit_rack =
        rack_info_table_entry_read_bit_rack(stored);
    const RackInfoTableEntry *found =
        rack_info_table_lookup(rit, &entry_bit_rack);
    assert(found == stored);
    assert(found->leaves[0] == 0);
  }

  // ---- Roundtrip file I/O ----
  const char *data_paths = DEFAULT_TEST_DATA_PATH;
  const char *rit_name = "rit_test_csw21_ab";
  ErrorStack *error_stack = error_stack_create();
  char *rit_filename = data_filepaths_get_writable_filename(
      data_paths, rit_name, DATA_FILEPATH_TYPE_RACK_INFO_TABLE, error_stack);
  assert(error_stack_is_empty(error_stack));

  rack_info_table_write_to_file(rit, rit_filename, error_stack);
  assert(error_stack_is_empty(error_stack));

  RackInfoTable *rit_loaded =
      rack_info_table_create(data_paths, rit_name, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(rit_loaded != NULL);
  assert_rits_equal(rit, rit_loaded);

  rack_info_table_destroy(rit_loaded);
  rack_info_table_destroy(rit);
  free(rit_filename);
  error_stack_destroy(error_stack);
  game_destroy(game);
  config_destroy(config);
}
