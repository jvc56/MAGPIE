#include "rack_info_table_test.h"

#include "../src/def/players_data_defs.h"
#include "../src/def/rack_defs.h"
#include "../src/ent/autoplay_results.h"
#include "../src/ent/bit_rack.h"
#include "../src/ent/data_filepaths.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/klv.h"
#include "../src/ent/leave_map.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/player.h"
#include "../src/ent/players_data.h"
#include "../src/ent/rack.h"
#include "../src/ent/rack_info_table.h"
#include "../src/ent/wmp.h"
#include "../src/impl/config.h"
#include "../src/impl/rack_info_table_maker.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
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

static void
verify_canonical_subracks_recursive(RitRefState *state, MachineLetter ml) {
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
static void
compute_expected_unions(const WMP *wmp, const LetterDistribution *ld,
                        const BitRack *rack_bit_rack,
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
      .cap = ld_size < (int)BIT_RACK_MAX_ALPHABET_SIZE
                 ? ld_size
                 : (int)BIT_RACK_MAX_ALPHABET_SIZE,
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
    const BitRack rack_bit_rack =
        rack_info_table_entry_read_bit_rack(entry);
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
  Config *config = config_create_or_die(
      "set -lex CSW21 -ld english_ab -k1 CSW21 -k2 CSW21");
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
  const RackInfoTableEntry *entry =
      rack_info_table_lookup(rit, &test_bit_rack);
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
    const BitRack entry_bit_rack =
        rack_info_table_entry_read_bit_rack(stored);
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

// On-demand test: build the full CSW24 RIT at POC coverage (min=7) and
// cross-check every entry's playthrough slot against an independent WMP
// query, plus pin-check the specific "APNOEAL + J -> JALAPENO" case.
// Invoked via ./bin/magpie_test rit_csw24_full.
void test_rack_info_table_csw24_full(void) {
  Config *config = config_create_or_die(
      "set -lex CSW24 -ld english -k1 CSW24 -k2 CSW24");
  const LetterDistribution *ld = config_get_ld(config);
  Game *game = config_game_create(config);
  const Player *player = game_get_player(game, 0);
  const KLV *klv = player_get_klv(player);
  const WMP *wmp = player_get_wmp(player);
  assert(klv != NULL);
  assert(wmp != NULL);

  // Build at min=RACK_SIZE — only the full-rack (leave_size=0) union is
  // populated. Verifying just the fullrack slot keeps the test fast, and
  // the per-letter WMP cross-check is the same correctness bar regardless
  // of how many other unions we'd otherwise be filling.
  const uint8_t min_played = (uint8_t)RACK_SIZE;
  RackInfoTable *rit = make_rack_info_table(klv, wmp, ld, 0, min_played);
  assert(rit != NULL);
  assert(rit->version == RIT_VERSION);
  assert(rit->playthrough_min_played_size == min_played);
  assert(rit->num_entries > 1000000); // CSW24 has millions of distinct racks

  // Whole-table consistency: every covered union matches an independent
  // per-letter WMP probe over every canonical subrack. With min=7 only
  // leave_size=0 is checked as populated; all other leave_sizes must be
  // zero.
  verify_unions_against_wmp(rit, wmp, ld);

  // Pin-check: APNOEAL + J -> JALAPENO. That's the only 8-letter word
  // composed of {A, A, E, L, N, O, P} + one additional letter in CSW24,
  // so the leave_size=0 union should be exactly (1 << ml_J) and nothing
  // else.
  Rack apnoeal;
  rack_set_dist_size(&apnoeal, ld_get_size(ld));
  rack_reset(&apnoeal);
  rack_add_letter(&apnoeal, ld_hl_to_ml(ld, "A"));
  rack_add_letter(&apnoeal, ld_hl_to_ml(ld, "A"));
  rack_add_letter(&apnoeal, ld_hl_to_ml(ld, "E"));
  rack_add_letter(&apnoeal, ld_hl_to_ml(ld, "L"));
  rack_add_letter(&apnoeal, ld_hl_to_ml(ld, "N"));
  rack_add_letter(&apnoeal, ld_hl_to_ml(ld, "O"));
  rack_add_letter(&apnoeal, ld_hl_to_ml(ld, "P"));
  assert(rack_get_total_letters(&apnoeal) == RACK_SIZE);

  const BitRack apnoeal_bit_rack = bit_rack_create_from_rack(ld, &apnoeal);
  const RackInfoTableEntry *apnoeal_entry =
      rack_info_table_lookup(rit, &apnoeal_bit_rack);
  assert(apnoeal_entry != NULL);
  const uint32_t apnoeal_bitmask =
      rack_info_table_entry_get_playthrough_union(apnoeal_entry,
                                                   /*leave_size=*/0);
  const MachineLetter ml_j = ld_hl_to_ml(ld, "J");
  const uint32_t expected_apnoeal = 1U << ml_j;
  assert(apnoeal_bitmask == expected_apnoeal);

  rack_info_table_destroy(rit);
  game_destroy(game);
  config_destroy(config);
}

// ============================================================================
// RIT coverage sweep
//
// On-demand benchmark that builds several CSW24 RackInfoTables in-process
// with different playthrough_min_played_size values, runs autoplay with
// each, and prints a summary table so we can pick the coverage that
// maximizes movegen performance. Invoked via ./bin/magpie_test rit_sweep.
// ============================================================================

typedef struct RitSweepTiming {
  double real_sec;
  double user_sec;
  double sys_sec;
} RitSweepTiming;

static RitSweepTiming rit_sweep_run_once(Config *config, const char *cmd) {
  struct timespec ts_start;
  struct timespec ts_end;
  struct rusage ru_start;
  struct rusage ru_end;
  clock_gettime(CLOCK_MONOTONIC, &ts_start);
  getrusage(RUSAGE_SELF, &ru_start);
  load_and_exec_config_or_die(config, cmd);
  getrusage(RUSAGE_SELF, &ru_end);
  clock_gettime(CLOCK_MONOTONIC, &ts_end);
  RitSweepTiming t;
  t.real_sec = (double)(ts_end.tv_sec - ts_start.tv_sec) +
               (double)(ts_end.tv_nsec - ts_start.tv_nsec) / 1e9;
  t.user_sec = (double)(ru_end.ru_utime.tv_sec - ru_start.ru_utime.tv_sec) +
               (double)(ru_end.ru_utime.tv_usec - ru_start.ru_utime.tv_usec) /
                   1e6;
  t.sys_sec = (double)(ru_end.ru_stime.tv_sec - ru_start.ru_stime.tv_sec) +
              (double)(ru_end.ru_stime.tv_usec - ru_start.ru_stime.tv_usec) /
                  1e6;
  return t;
}

// Use min == RACK_SIZE + 1 to represent "RIT with no playthrough coverage",
// and min == -1 to represent "no RIT at all".
typedef struct RitSweepConfig {
  const char *label;
  int min_played_size;
} RitSweepConfig;

void test_rack_info_table_csw24_sweep(void) {
  Config *config = config_create_or_die(
      "set -lex CSW24 -ld english -k1 CSW24 -k2 CSW24 -threads 8 "
      "-numplays 1 -rit false -mtmode pgp -gp false -s1 equity -s2 equity "
      "-r1 best -r2 best");
  const LetterDistribution *ld = config_get_ld(config);
  PlayersData *players_data = config_get_players_data(config);
  const KLV *klv = players_data_get_klv(players_data, 0);
  const WMP *wmp = players_data_get_wmp(players_data, 0);
  assert(klv != NULL);
  assert(wmp != NULL);
  assert(players_data_get_is_shared(players_data, PLAYERS_DATA_TYPE_KLV));
  assert(players_data_get_is_shared(players_data, PLAYERS_DATA_TYPE_WMP));

  const RitSweepConfig sweep_configs[] = {
      {"no RIT", -1},
      {"RIT leaves only", RACK_SIZE + 1},
      {"RIT min=7", 7},
      {"RIT min=6", 6},
      {"RIT min=5", 5},
      {"RIT min=4", 4},
      {"RIT min=3", 3},
      {"RIT min=2", 2},
      {"RIT min=1", 1},
  };
  const int num_configs =
      (int)(sizeof(sweep_configs) / sizeof(sweep_configs[0]));

  RitSweepTiming timings[sizeof(sweep_configs) / sizeof(sweep_configs[0])];
  char *result_strs[sizeof(sweep_configs) / sizeof(sweep_configs[0])];
  uint32_t slots_per_entry[sizeof(sweep_configs) /
                           sizeof(sweep_configs[0])];

  // 100k games gives enough signal to discriminate per-config differences
  // without making the whole sweep take forever. Same seed for every
  // iteration so results are apples-to-apples.
  const char *autoplay_cmd = "autoplay games 100000 -seed 42";

  for (int cfg_idx = 0; cfg_idx < num_configs; cfg_idx++) {
    const RitSweepConfig *cfg = &sweep_configs[cfg_idx];
    printf("[rit_sweep] ==== iteration %d/%d: %s ====\n", cfg_idx + 1,
           num_configs, cfg->label);
    (void)fflush(stdout);

    // Mark the RIT slot empty before building, so no stale pointer lingers
    // if the build aborts.
    players_data_set_data(players_data, PLAYERS_DATA_TYPE_RIT, 0, NULL);
    players_data_set_data(players_data, PLAYERS_DATA_TYPE_RIT, 1, NULL);

    RackInfoTable *rit = NULL;
    if (cfg->min_played_size >= 0) {
      printf("[rit_sweep] building %s...\n", cfg->label);
      (void)fflush(stdout);
      rit = make_rack_info_table(klv, wmp, ld, 8,
                                 (uint8_t)cfg->min_played_size);
      assert(rit != NULL);
      // Count covered leave_size unions: played_size in [min, RACK_SIZE]
      // maps to leave_size in [0, RACK_SIZE - min], which is
      // (RACK_SIZE - min + 1) unions. Zero if min > RACK_SIZE.
      slots_per_entry[cfg_idx] =
          (rit->playthrough_min_played_size > RACK_SIZE)
              ? 0
              : (uint32_t)(RACK_SIZE + 1 - rit->playthrough_min_played_size);
      // Install the freshly built RIT into both player slots. The KLV and
      // WMP are shared across players in this config, so one RIT
      // allocation is safe to alias for both. Ownership stays with us;
      // we'll clear the slots before destroying the table.
      //
      // Then force use_when_available back to false. players_data_set_data
      // flips it to true (!!data), which would trigger
      // config_load_lexicon_dependent_data's next call to
      // players_data_set(RIT, ..., "CSW24", "CSW24") -- a file-based load
      // path that would overwrite our in-memory RIT with one loaded from
      // CSW24.rit and double-free the aliased pointer in the process.
      // With use_when_available=false, the load path passes NULL names
      // and the null-name fast path in players_data_set leaves our
      // installed pointer alone because it "matches" the existing NULL
      // name of our in-memory RIT. MoveGen still picks up the RIT via
      // player_get_rack_info_table, which reads players_data->data[...]
      // directly and ignores use_when_available.
      players_data_set_data(players_data, PLAYERS_DATA_TYPE_RIT, 0, rit);
      players_data_set_data(players_data, PLAYERS_DATA_TYPE_RIT, 1, rit);
      players_data_set_use_when_available(players_data,
                                          PLAYERS_DATA_TYPE_RIT, 0, false);
      players_data_set_use_when_available(players_data,
                                          PLAYERS_DATA_TYPE_RIT, 1, false);
    } else {
      slots_per_entry[cfg_idx] = 0;
    }

    printf("[rit_sweep] running autoplay for %s...\n", cfg->label);
    (void)fflush(stdout);
    timings[cfg_idx] = rit_sweep_run_once(config, autoplay_cmd);
    result_strs[cfg_idx] = autoplay_results_to_string(
        config_get_autoplay_results(config), false, false);

    // Clear the RIT slots before destroying, otherwise players_data would
    // free the table on the next config reload.
    if (rit != NULL) {
      players_data_set_data(players_data, PLAYERS_DATA_TYPE_RIT, 0, NULL);
      players_data_set_data(players_data, PLAYERS_DATA_TYPE_RIT, 1, NULL);
      rack_info_table_destroy(rit);
    }
  }

  // Correctness gate: every config must produce byte-identical autoplay
  // results at the same seed, regardless of which RIT (if any) was used.
  // Any divergence means a fast path silently changed which play was
  // picked.
  for (int cfg_idx = 1; cfg_idx < num_configs; cfg_idx++) {
    if (strcmp(result_strs[0], result_strs[cfg_idx]) != 0) {
      printf("[rit_sweep] DIVERGENCE: '%s' differs from '%s'\n",
             sweep_configs[cfg_idx].label, sweep_configs[0].label);
      printf("  baseline: %s\n", result_strs[0]);
      printf("  this run: %s\n", result_strs[cfg_idx]);
    }
    assert(strcmp(result_strs[0], result_strs[cfg_idx]) == 0);
  }

  const double base_user = timings[0].user_sec;
  const double base_real = timings[0].real_sec;

  printf("\n");
  printf("RIT coverage sweep (CSW24 english, 100k games, 8 threads, seed 42)\n");
  printf("--\n");
  printf("%-20s %6s %10s %10s %10s %14s %14s\n", "config", "unions",
         "real (s)", "user (s)", "sys (s)", "Δuser vs base", "Δreal vs base");
  for (int cfg_idx = 0; cfg_idx < num_configs; cfg_idx++) {
    printf("%-20s %6u %10.2f %10.2f %10.2f %+14.2f %+14.2f\n",
           sweep_configs[cfg_idx].label, slots_per_entry[cfg_idx],
           timings[cfg_idx].real_sec, timings[cfg_idx].user_sec,
           timings[cfg_idx].sys_sec, timings[cfg_idx].user_sec - base_user,
           timings[cfg_idx].real_sec - base_real);
    free(result_strs[cfg_idx]);
  }
  printf("--\n");

  config_destroy(config);
}
