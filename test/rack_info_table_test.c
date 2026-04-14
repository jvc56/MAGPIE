#include "rack_info_table_test.h"

#include "../src/def/bit_rack_defs.h"
#include "../src/def/board_defs.h"
#include "../src/def/kwg_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/rack_defs.h"
#include "../src/ent/bit_rack.h"
#include "../src/ent/data_filepaths.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/klv.h"
#include "../src/ent/leave_map.h"
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
  assert(a->playthrough_min_played_size == b->playthrough_min_played_size);
  assert(a->num_buckets == b->num_buckets);
  assert(a->num_entries == b->num_entries);
  for (uint32_t bucket_idx = 0; bucket_idx <= a->num_buckets; bucket_idx++) {
    assert(a->bucket_starts[bucket_idx] == b->bucket_starts[bucket_idx]);
  }
  for (uint32_t entry_idx = 0; entry_idx < a->num_entries; entry_idx++) {
    const RackInfoTableEntry *ea = &a->entries[entry_idx];
    const RackInfoTableEntry *eb = &b->entries[entry_idx];
    // Compare packed 24-bit leaves by unpacking both.
    Equity leaves_a[RACK_INFO_TABLE_LEAVES_PER_ENTRY];
    Equity leaves_b[RACK_INFO_TABLE_LEAVES_PER_ENTRY];
    rack_info_table_entry_unpack_leaves(ea, leaves_a);
    rack_info_table_entry_unpack_leaves(eb, leaves_b);
    for (int leaf_idx = 0; leaf_idx < RACK_INFO_TABLE_LEAVES_PER_ENTRY;
         leaf_idx++) {
      assert(leaves_a[leaf_idx] == leaves_b[leaf_idx]);
    }
    for (int union_idx = 0; union_idx < RACK_INFO_TABLE_UNIONS_PER_ENTRY;
         union_idx++) {
      assert(ea->playthrough_union[union_idx] ==
             eb->playthrough_union[union_idx]);
    }
    for (int leave_idx = 0;
         leave_idx < RACK_INFO_TABLE_NONPLAYTHROUGH_BEST_LEAVES_PER_ENTRY;
         leave_idx++) {
      assert(ea->nonplaythrough_best_leave_values[leave_idx] ==
             eb->nonplaythrough_best_leave_values[leave_idx]);
    }
    assert(ea->nonplaythrough_has_word_of_length_bitmask ==
           eb->nonplaythrough_has_word_of_length_bitmask);
    for (int byte_idx = 0; byte_idx < RACK_INFO_TABLE_BITRACK_BYTES;
         byte_idx++) {
      assert(ea->bit_rack_bytes[byte_idx] == eb->bit_rack_bytes[byte_idx]);
    }
  }
}

// Reference implementation for ground-truth verification: recursively
// enumerate canonical subracks of a full rack (same walk as the maker),
// and for each terminal, run per-letter WMP probes and OR the resulting
// bitmask into expected_unions[popcount(leave_map->current_index)].
typedef struct RitRefState {
  const WMP *wmp;
  const LetterDistribution *ld;
  int ld_size;
  int cap;
  Rack *player_rack;
  Rack *leave;
  LeaveMap *leave_map;
  uint32_t expected_unions[RACK_INFO_TABLE_UNIONS_PER_ENTRY];
} RitRefState;

static int rit_test_popcount(unsigned int x) {
#if defined(__has_builtin) && __has_builtin(__builtin_popcount)
  return __builtin_popcount(x);
#else
  int count = 0;
  while (x) {
    x &= x - 1;
    count++;
  }
  return count;
#endif
}

static void verify_canonical_subracks_recursive(RitRefState *state,
                                                MachineLetter ml) {
  const int ld_size = state->ld_size;
  while (ml < ld_size && rack_get_letter(state->player_rack, ml) == 0) {
    ml++;
  }
  if (ml == ld_size) {
    const int played_size = rack_get_total_letters(state->player_rack);
    if (played_size > 0) {
      const int word_length = played_size + 1;
      if (word_length >= MINIMUM_WORD_LENGTH && word_length <= BOARD_DIM) {
        const BitRack rack_bit_rack =
            bit_rack_create_from_rack(state->ld, state->player_rack);
        uint32_t bitmask = 0;
        for (int letter_ml = 1; letter_ml < state->cap; letter_ml++) {
          BitRack query = rack_bit_rack;
          bit_rack_add_letter(&query, (MachineLetter)letter_ml);
          const WMPEntry *wmp_entry =
              wmp_get_word_entry(state->wmp, &query, word_length);
          if (wmp_entry != NULL) {
            bitmask |= (1U << letter_ml);
          }
        }
        const int leave_size =
            rit_test_popcount((unsigned int)state->leave_map->current_index);
        state->expected_unions[leave_size] |= bitmask;
      }
    }
    return;
  }

  verify_canonical_subracks_recursive(state, ml + 1);

  const uint16_t num_this = rack_get_letter(state->player_rack, ml);
  for (uint16_t tile_idx = 0; tile_idx < num_this; tile_idx++) {
    rack_add_letter(state->leave, ml);
    leave_map_take_letter_and_update_complement_index(state->leave_map,
                                                      state->player_rack, ml);
    verify_canonical_subracks_recursive(state, ml + 1);
  }

  rack_take_letters(state->leave, ml, num_this);
  for (int tile_idx = 0; tile_idx < num_this; tile_idx++) {
    leave_map_add_letter_and_update_complement_index(state->leave_map,
                                                     state->player_rack, ml);
  }
}

// Compute the expected per-leave-size union bitmasks for a single rack by
// replaying the same canonical enumeration the maker does and running
// per-letter WMP probes as the ground truth at each terminal.
static void compute_expected_unions(
    const WMP *wmp, const LetterDistribution *ld, const BitRack *rack_bit_rack,
    uint32_t expected_unions[RACK_INFO_TABLE_UNIONS_PER_ENTRY]) {
  const int ld_size = ld_get_size(ld);

  Rack player_rack;
  rack_set_dist_size(&player_rack, ld_size);
  rack_reset(&player_rack);
  for (int ml = 0; ml < ld_size; ml++) {
    const int count = bit_rack_get_letter(rack_bit_rack, ml);
    player_rack.array[ml] = (uint16_t)count;
    player_rack.number_of_letters += (uint16_t)count;
  }

  LeaveMap leave_map;
  leave_map.rack_array_size = ld_size;
  leave_map_init(&player_rack, &leave_map);
  leave_map_set_current_index(&leave_map, 0);

  Rack leave;
  rack_set_dist_size(&leave, ld_size);
  rack_reset(&leave);

  for (int i = 0; i < RACK_INFO_TABLE_UNIONS_PER_ENTRY; i++) {
    expected_unions[i] = 0;
  }

  RitRefState state = {
      .wmp = wmp,
      .ld = ld,
      .ld_size = ld_size,
      .cap = ld_size < BIT_RACK_MAX_ALPHABET_SIZE ? ld_size
                                                  : BIT_RACK_MAX_ALPHABET_SIZE,
      .player_rack = &player_rack,
      .leave = &leave,
      .leave_map = &leave_map,
  };
  for (int i = 0; i < RACK_INFO_TABLE_UNIONS_PER_ENTRY; i++) {
    state.expected_unions[i] = 0;
  }

  verify_canonical_subracks_recursive(&state, 0);

  for (int i = 0; i < RACK_INFO_TABLE_UNIONS_PER_ENTRY; i++) {
    expected_unions[i] = state.expected_unions[i];
  }
}

// Walk every RIT entry and, for each, recompute the per-leave-size union
// bitmasks via an independent per-letter WMP probe over every canonical
// subrack. Checks that the RIT's populated unions (inside the coverage
// interval) match the ground truth, and that unions outside the interval
// are zero.
static void verify_unions_against_wmp(const RackInfoTable *rit, const WMP *wmp,
                                      const LetterDistribution *ld) {
  const int min_played = (int)rit->playthrough_min_played_size;
  const int max_covered_leave_size =
      min_played > RACK_SIZE ? -1 : RACK_SIZE - min_played;

  uint32_t expected_unions[RACK_INFO_TABLE_UNIONS_PER_ENTRY];
  for (uint32_t entry_idx = 0; entry_idx < rit->num_entries; entry_idx++) {
    const RackInfoTableEntry *entry = &rit->entries[entry_idx];
    const BitRack rack_bit_rack = rack_info_table_entry_read_bit_rack(entry);
    compute_expected_unions(wmp, ld, &rack_bit_rack, expected_unions);

    for (int leave_size = 0; leave_size < RACK_INFO_TABLE_UNIONS_PER_ENTRY;
         leave_size++) {
      const uint32_t got = entry->playthrough_union[leave_size];
      if (leave_size <= max_covered_leave_size) {
        assert(got == expected_unions[leave_size]);
      } else {
        assert(got == 0);
      }
    }
  }
}

void test_rack_info_table(void) {
  // Use CSW21 with the english_ab distribution (50 A, 50 B, 0 blank).
  // This keeps the number of full racks small (8, for 0A+7B through 7A+0B)
  // while still exercising the full build path with a real KLV and WMP.
  Config *config =
      config_create_or_die("set -lex CSW21 -ld english_ab -k1 CSW21 -k2 CSW21");
  const LetterDistribution *ld = config_get_ld(config);
  Game *game = config_game_create(config);
  const Player *player = game_get_player(game, 0);
  const KLV *klv = player_get_klv(player);
  const WMP *wmp = player_get_wmp(player);
  assert(klv != NULL);
  assert(wmp != NULL);

  // Build with full coverage (min=1) so every leave_size union gets
  // populated and verified. This is cheap for english_ab because there
  // are only a handful of full racks.
  const uint8_t min_played = 1;
  RackInfoTable *rit = make_rack_info_table(klv, wmp, ld, 1, min_played);
  assert(rit != NULL);
  assert(rit->version == RIT_VERSION);
  assert(rit->rack_size == (uint8_t)RACK_SIZE);
  assert(rit->playthrough_min_played_size == min_played);

  // The number of entries should equal the number of possible
  // RACK_SIZE-tile multisets from the distribution.
  const uint32_t expected_count = count_racks_for_ld(ld, 0, RACK_SIZE);
  assert(rit->num_entries == expected_count);

  // Num buckets should be a power of 2 >= num_entries.
  assert((rit->num_buckets & (rit->num_buckets - 1)) == 0);
  assert(rit->num_buckets >= rit->num_entries);

  // Coverage sanity checks: min=1 means every played_size in [1, 7] is
  // covered, and played_size 0 is out of range.
  for (int played_size = 1; played_size <= RACK_SIZE; played_size++) {
    assert(rack_info_table_has_playthrough_coverage(rit, played_size));
  }
  assert(!rack_info_table_has_playthrough_coverage(rit, 0));

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
  const RackInfoTableEntry *entry = rack_info_table_lookup(rit, &test_bit_rack);
  assert(entry != NULL);
  // Index 0 (empty leave) must be 0.
  {
    Equity unpacked[RACK_INFO_TABLE_LEAVES_PER_ENTRY];
    rack_info_table_entry_unpack_leaves(entry, unpacked);
    assert(unpacked[0] == 0);
  }

  // Verify every entry we enumerate can be looked up, and that the
  // RIT's playthrough slot 0 matches an independent WMP query for the
  // full rack.
  for (uint32_t entry_idx = 0; entry_idx < rit->num_entries; entry_idx++) {
    const RackInfoTableEntry *stored = &rit->entries[entry_idx];
    const BitRack entry_bit_rack = rack_info_table_entry_read_bit_rack(stored);
    const RackInfoTableEntry *found =
        rack_info_table_lookup(rit, &entry_bit_rack);
    assert(found == stored);
    Equity unpacked_leaves[RACK_INFO_TABLE_LEAVES_PER_ENTRY];
    rack_info_table_entry_unpack_leaves(found, unpacked_leaves);
    assert(unpacked_leaves[0] == 0);
  }
  verify_unions_against_wmp(rit, wmp, ld);

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
      rack_info_table_create(data_paths, rit_name, false, error_stack);
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
