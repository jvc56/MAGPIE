#include "compact_leaves_test.h"

#include "../src/def/letter_distribution_defs.h"
#include "../src/def/rack_defs.h"
#include "../src/ent/compact_leaves.h"
#include "../src/ent/data_filepaths.h"
#include "../src/ent/equity.h"
#include "../src/ent/klv.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/rack.h"
#include "../src/impl/compact_leaves_maker.h"
#include "../src/impl/config.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Asserts the two models are byte-for-byte equivalent in every field, so a
// file-loaded model is indistinguishable from the in-memory one it was written
// from.
static void assert_clv_fields_equal(const CompactLeaves *a,
                                    const CompactLeaves *b) {
  assert(a->dist_size == b->dist_size);
  assert(a->radix_code == b->radix_code);
  assert(a->radix_millipoints == b->radix_millipoints);
  assert(a->flags == b->flags);
  assert(a->coef_bits == b->coef_bits);
  assert(a->vowel_bits == b->vowel_bits);
  assert(a->base_ticks == b->base_ticks);
  assert(a->num_synergies == b->num_synergies);
  for (int ml = 0; ml < a->dist_size; ml++) {
    assert(a->tile_ticks[ml] == b->tile_ticks[ml]);
  }
  const bool has_dup = (a->flags & COMPACT_LEAVES_FLAG_HAS_DUP) != 0;
  assert(has_dup == ((b->flags & COMPACT_LEAVES_FLAG_HAS_DUP) != 0));
  if (has_dup) {
    assert(a->dup_ticks != NULL && b->dup_ticks != NULL);
    for (int ml = 0; ml < a->dist_size; ml++) {
      assert(a->dup_ticks[ml] == b->dup_ticks[ml]);
    }
  } else {
    assert(a->dup_ticks == NULL && b->dup_ticks == NULL);
  }
  for (int i = 0; i < COMPACT_LEAVES_VC_TABLE_SIZE; i++) {
    assert(a->vc_ticks[i] == b->vc_ticks[i]);
  }
  for (uint32_t syn_idx = 0; syn_idx < a->num_synergies; syn_idx++) {
    const CompactLeavesSynergy *sa = &a->synergies[syn_idx];
    const CompactLeavesSynergy *sb = &b->synergies[syn_idx];
    assert(sa->num_tiles == sb->num_tiles);
    assert(sa->value_ticks == sb->value_ticks);
    assert(memcmp(sa->tiles, sb->tiles, sa->num_tiles) == 0);
  }
}

// The quantized leave value must be identical between two models for a spread of
// sampled leaves (singles, duplicates, and small multi-tile racks). Sampled by
// machine-letter index so it needs no per-lexicon orthography.
static void assert_clv_values_equal(const CompactLeaves *a,
                                    const CompactLeaves *b,
                                    const LetterDistribution *ld) {
  const int size = ld_get_size(ld);
  Rack *leave = rack_create((uint16_t)size);

  // Empty leave is defined to be 0 for both.
  assert(compact_leaves_get_leave_value(a, leave) == 0);
  assert(compact_leaves_get_leave_value(b, leave) == 0);

  // Single tiles (incl. blank, ml 0) and doubled tiles.
  for (int ml = 0; ml < size; ml++) {
    rack_reset(leave);
    rack_add_letter(leave, (MachineLetter)ml);
    assert(compact_leaves_get_leave_value(a, leave) ==
           compact_leaves_get_leave_value(b, leave));
    if (ml + 1 < size) {
      rack_add_letter(leave, (MachineLetter)ml);
      assert(compact_leaves_get_leave_value(a, leave) ==
             compact_leaves_get_leave_value(b, leave));
    }
  }

  // A few full-width (RACK_SIZE-1) leaves built from low machine letters.
  for (int start = 0; start + 1 < size && start < 4; start++) {
    rack_reset(leave);
    for (int offset = 0; offset < RACK_SIZE - 1; offset++) {
      const int ml = 1 + ((start + offset) % (size - 1));
      rack_add_letter(leave, (MachineLetter)ml);
    }
    assert(compact_leaves_get_leave_value(a, leave) ==
           compact_leaves_get_leave_value(b, leave));
  }

  rack_destroy(leave);
}

// Builds a CompactLeaves from `lexicon`'s KLV, asserts the budget contract, and
// checks a write/read round-trip preserves both the fields and the evaluated
// values. Run for English (CSW21) and Polish (OSPS49); the larger Polish
// alphabet flexes the per-tile arrays and the vowel bitset.
static void test_compact_leaves_for_lexicon(const char *lexicon,
                                            size_t target_bytes,
                                            bool bit_packed) {
  char *set_cmd = get_formatted_string("set -lex %s -wmp false", lexicon);
  Config *config = config_create_or_die(set_cmd);
  free(set_cmd);
  const LetterDistribution *ld = config_get_ld(config);

  ErrorStack *error_stack = error_stack_create();
  KLV *klv = klv_create(config_get_data_paths(config), lexicon, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(klv != NULL);

  CompactLeaves *cl = compact_leaves_create_from_klv(
      klv, ld, NULL, target_bytes, COMPACT_LEAVES_RADIX_EIGHTH, bit_packed);
  assert(cl != NULL);
  assert(cl->dist_size == ld_get_size(ld));
  assert(cl->radix_code == COMPACT_LEAVES_RADIX_EIGHTH);
  assert(((cl->flags & COMPACT_LEAVES_FLAG_BITPACKED) != 0) == bit_packed);

  const char *filename = "compact_leaves_round_trip_test.clv";
  compact_leaves_write_to_file(cl, filename, error_stack);
  assert(error_stack_is_empty(error_stack));

  // target_bytes here is comfortably above the base-model floor, so the file
  // must honor the budget.
  FILE *sized = fopen(filename, "rb");
  assert(sized != NULL);
  fseek(sized, 0, SEEK_END);
  const long file_size = ftell(sized);
  fclose(sized);
  assert(file_size > 0);
  assert((size_t)file_size <= target_bytes);

  CompactLeaves *loaded = compact_leaves_read_from_file(filename, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(loaded != NULL);
  assert_clv_fields_equal(cl, loaded);
  assert_clv_values_equal(cl, loaded, ld);

  printf("[%s] compact leaves: dist_size=%u synergies=%u flags=0x%x -> %ld "
         "bytes (budget %zu)\n",
         lexicon, cl->dist_size, cl->num_synergies, cl->flags, file_size,
         target_bytes);

  (void)remove(filename);
  compact_leaves_destroy(loaded);
  compact_leaves_destroy(cl);
  klv_destroy(klv);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

// Exercises the klv2clv convert command end-to-end: convert a small testdata
// KLV to a .clv on disk and confirm it loads and evaluates.
static void test_compact_leaves_convert_command(void) {
  Config *config = config_create_or_die("set -lex CSW21 -wmp false");
  ErrorStack *error_stack = error_stack_create();

  load_and_exec_config_or_die(
      config, "convert klv2clv CSW21_small english_small -clvsize 2048");

  char *clv_path = data_filepaths_get_readable_filename(
      DEFAULT_TEST_DATA_PATH, "CSW21_small", DATA_FILEPATH_TYPE_COMPACT_LEAVES,
      error_stack);
  assert(error_stack_is_empty(error_stack));

  CompactLeaves *cl = compact_leaves_read_from_file(clv_path, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(cl != NULL);
  // english_small has blank + A + B.
  assert(cl->dist_size == 3);

  // The empty leave evaluates to 0; a one-tile leave is a finite Equity.
  Rack *leave = rack_create(cl->dist_size);
  assert(compact_leaves_get_leave_value(cl, leave) == 0);
  rack_add_letter(leave, 1);
  const Equity single = compact_leaves_get_leave_value(cl, leave);
  assert(single > EQUITY_MIN_VALUE && single < EQUITY_MAX_VALUE);
  rack_destroy(leave);

  (void)remove(clv_path);
  free(clv_path);
  compact_leaves_destroy(cl);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

void test_compact_leaves(void) {
  // Bit-packed (the default the maker/convert ship) and byte-granular bodies.
  test_compact_leaves_for_lexicon("CSW21", 4096, true);
  test_compact_leaves_for_lexicon("CSW21", 8192, false);
  // Polish: large alphabet (no 64-bit BitRack) flexing the per-tile arrays.
  test_compact_leaves_for_lexicon("OSPS49", 4096, true);
  test_compact_leaves_convert_command();
}
