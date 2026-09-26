#include "pat_file_test.h"

#include "../src/def/pat_defs.h"
#include "../src/def/rack_defs.h"
#include "../src/ent/pat.h"
#include "../src/ent/pat_file.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "pat_test_util.h"
#include "test_util.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Every row the writer emits reads back as written: the weights and every
// flag, scale and adjustment, each set away from its default. The file is
// the current version and carries none of the retired rows.
static void test_pat_file_round_trip_all_rows(const char *data_dir) {
  PATWeights *pat = pat_create_zeroed("all_rows");
  for (int feature_index = 0; feature_index < PAT_NUM_FEATURES;
       feature_index++) {
    pat_set_weight(pat, feature_index, -1 - ((feature_index * 37) % 997));
  }
  pat_set_combine_gamma(pat, 0.25);
  pat_set_own_asset_discount(pat, 0.5);
  pat_set_lexicon_floaters(pat, !PAT_DEFAULT_LEXICON_FLOATERS);
  pat_set_signed_through(pat, !PAT_DEFAULT_SIGNED_THROUGH);
  pat_set_fit_residual(pat, PAT_FIT_THROUGH);
  pat_set_exact_created_hooks(pat, !PAT_DEFAULT_EXACT_CREATED_HOOKS);
  pat_set_run_through(pat, !PAT_DEFAULT_RUN_THROUGH);
  // Written whenever positive; its tables are built only when the weights
  // meet a letter distribution (see pat_prepare_utility).
  pat->utility_adjust = 1500.0;
  pat_set_stage_scale(pat, PAT_STAGE_EARLY, 0.5);
  pat_set_stage_scale(pat, PAT_STAGE_MID, 1.25);
  pat_set_stage_scale(pat, PAT_STAGE_LATE, 0.0);
  for (int tiles = 1; tiles <= RACK_SIZE; tiles++) {
    pat_set_opening_tiles_adjustment(pat, tiles, -100 * tiles);
  }
  pat_set_opening_exchange_adjustment(pat, -250);

  ErrorStack *error_stack = error_stack_create();
  pat_write(pat, data_dir, "all_rows", error_stack);
  assert(error_stack_is_empty(error_stack));
  PATWeights *loaded = pat_create(data_dir, "all_rows", error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(loaded);
  for (int feature_index = 0; feature_index < PAT_NUM_FEATURES;
       feature_index++) {
    assert(pat_get_weight(loaded, feature_index) ==
           pat_get_weight(pat, feature_index));
  }
  assert(fabs(loaded->combine_gamma - 0.25) < 1e-9);
  assert(fabs(pat_get_own_asset_discount(loaded) - 0.5) < 1e-9);
  assert(pat_get_lexicon_floaters(loaded) == !PAT_DEFAULT_LEXICON_FLOATERS);
  assert(pat_get_signed_through(loaded) == !PAT_DEFAULT_SIGNED_THROUGH);
  assert(pat_get_fit_residual(loaded) == PAT_FIT_THROUGH);
  assert(loaded->exact_created_hooks == !PAT_DEFAULT_EXACT_CREATED_HOOKS);
  assert(loaded->run_through == !PAT_DEFAULT_RUN_THROUGH);
  assert(fabs(loaded->utility_adjust - 1500.0) < 1e-9);
  assert(loaded->utility_table == NULL);
  assert(fabs(pat_get_stage_scale(loaded, PAT_STAGE_EARLY) - 0.5) < 1e-9);
  assert(fabs(pat_get_stage_scale(loaded, PAT_STAGE_MID) - 1.25) < 1e-9);
  assert(pat_get_stage_scale(loaded, PAT_STAGE_LATE) == 0.0);
  for (int tiles = 1; tiles <= RACK_SIZE; tiles++) {
    assert(pat_get_opening_tiles_adjustment(loaded, tiles) == -100 * tiles);
  }
  assert(pat_get_opening_exchange_adjustment(loaded) == -250);
  assert(loaded->version == PAT_VERSION);

  char *filename = get_formatted_string("%s/strategy/all_rows.pat", data_dir);
  char *contents = get_string_from_file_or_die(filename);
  char *header = get_formatted_string("%s%d\n", PAT_MAGIC_PREFIX, PAT_VERSION);
  assert(has_prefix(header, contents));
  static const char *const retired[] = {
      "fit_scaled",  "train_overlay", "fit_shrink", "hook_scaled",
      "flex_scaled", "lm_span",       "lm_ext"};
  for (size_t retired_idx = 0;
       retired_idx < sizeof(retired) / sizeof(retired[0]); retired_idx++) {
    assert(strstr(contents, retired[retired_idx]) == NULL);
  }
  free(header);
  free(contents);
  free(filename);
  error_stack_destroy(error_stack);
  pat_destroy(loaded);
  pat_destroy(pat);
}

// A file with every weight zero, with rows placed between header_line and
// the weight rows.
static char *pat_file_with_rows(const char *header_line, const char *rows) {
  StringBuilder *sb = string_builder_create();
  string_builder_add_formatted_string(sb, "%s\n%s", header_line, rows);
  char feature_name[64];
  for (int feature_index = 0; feature_index < PAT_NUM_FEATURES;
       feature_index++) {
    pat_feature_name(feature_index, feature_name, sizeof(feature_name));
    string_builder_add_formatted_string(sb, "%s,0\n", feature_name);
  }
  char *contents = string_builder_dump(sb, NULL);
  string_builder_destroy(sb);
  return contents;
}

static void assert_pat_file_status(const char *data_dir, const char *name,
                                   const char *contents,
                                   error_code_t expected_status) {
  write_pat_file_contents(data_dir, name, contents);
  ErrorStack *error_stack = error_stack_create();
  PATWeights *pat = pat_create(data_dir, name, error_stack);
  if (expected_status == ERROR_STATUS_SUCCESS) {
    assert(pat);
    assert(error_stack_is_empty(error_stack));
  } else {
    assert(!pat);
    assert(error_stack_top(error_stack) == expected_status);
  }
  pat_destroy(pat);
  error_stack_destroy(error_stack);
}

// Rows the parser does not know, values out of range, and versions this
// build cannot read are rejected with the status naming the problem; the
// same file with a valid row loads.
static void test_pat_file_rejections(const char *data_dir) {
  char current[32];
  (void)snprintf(current, sizeof(current), "%s%d", PAT_MAGIC_PREFIX,
                 PAT_VERSION);
  char newer[32];
  (void)snprintf(newer, sizeof(newer), "%s%d", PAT_MAGIC_PREFIX,
                 PAT_VERSION + 1);
  const struct {
    const char *header;
    const char *rows;
    error_code_t status;
  } cases[] = {
      {current, "", ERROR_STATUS_SUCCESS},
      {current, "fit_residual,5\n", ERROR_STATUS_SUCCESS},
      {newer, "", ERROR_STATUS_PAT_UNSUPPORTED_VERSION},
      {"not_a_pat_file", "", ERROR_STATUS_PAT_INVALID_HEADER},
      {current, "unknown_flag,1\n", ERROR_STATUS_PAT_INVALID_ROW},
      {current, "fit_residual,1\n", ERROR_STATUS_PAT_INVALID_ROW},
      {current, "fit_residual,4\n", ERROR_STATUS_PAT_INVALID_ROW},
      {current, "gamma,1.5\n", ERROR_STATUS_PAT_INVALID_ROW},
      {current, "own_asset_discount,-0.1\n", ERROR_STATUS_PAT_INVALID_ROW},
      {current, "run_through,2\n", ERROR_STATUS_PAT_INVALID_ROW},
      {current, "utility_adjust,-1\n", ERROR_STATUS_PAT_INVALID_ROW},
      {current, "stage_scale_mid,-1\n", ERROR_STATUS_PAT_INVALID_ROW},
      {current, "fit_scaled,0\n", ERROR_STATUS_PAT_INVALID_ROW},
  };
  for (size_t case_idx = 0; case_idx < sizeof(cases) / sizeof(cases[0]);
       case_idx++) {
    char *contents =
        pat_file_with_rows(cases[case_idx].header, cases[case_idx].rows);
    char *name = get_formatted_string("rejection_%d", (int)case_idx);
    assert_pat_file_status(data_dir, name, contents, cases[case_idx].status);
    free(name);
    free(contents);
  }
  // One weight row too many.
  char *contents = pat_file_with_rows(current, "");
  char *extra = get_formatted_string("%shook_d1,0\n", contents);
  assert_pat_file_status(data_dir, "extra_row", extra,
                         ERROR_STATUS_PAT_WRONG_NUMBER_OF_ROWS);
  free(extra);
  free(contents);
}

void test_pat_file(void) {
  char *data_dir = create_temp_pat_data_dir();
  test_pat_file_round_trip_all_rows(data_dir);
  test_pat_file_rejections(data_dir);
  free(data_dir);
}
