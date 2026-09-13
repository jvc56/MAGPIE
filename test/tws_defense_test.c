#include "tws_defense_test.h"

#include "../src/def/tws_defense_defs.h"
#include "../src/ent/tws_defense.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// Creates a temporary data directory with a strategy/ subdirectory and
// returns the path (owned by the caller).
static char *create_temp_twd_data_dir(void) {
  char tmp_template[] = "/tmp/magpie_twd_XXXXXX";
  const char *tmp_dir = mkdtemp(tmp_template);
  assert(tmp_dir);
  char *strategy_dir = get_formatted_string("%s/strategy", tmp_dir);
  assert(mkdir(strategy_dir, 0755) == 0);
  free(strategy_dir);
  return string_duplicate(tmp_dir);
}

static void write_twd_file_contents(const char *data_dir, const char *twd_name,
                                    const char *contents) {
  ErrorStack *error_stack = error_stack_create();
  char *filename =
      get_formatted_string("%s/strategy/%s.twd", data_dir, twd_name);
  write_string_to_file(filename, "w", contents, error_stack);
  assert(error_stack_is_empty(error_stack));
  free(filename);
  error_stack_destroy(error_stack);
}

static void assert_twd_create_fails(const char *data_dir, const char *twd_name,
                                    const char *contents) {
  write_twd_file_contents(data_dir, twd_name, contents);
  ErrorStack *error_stack = error_stack_create();
  const TWDWeights *twd = twd_create(data_dir, twd_name, error_stack);
  assert(!twd);
  assert(!error_stack_is_empty(error_stack));
  error_stack_destroy(error_stack);
}

static void test_twd_feature_names(void) {
  char name_buffer[64];
  twd_feature_name(TWD_FEATURE_HOOK_START, name_buffer, sizeof(name_buffer));
  assert(strings_equal(name_buffer, "hook_d1"));
  twd_feature_name(TWD_FEATURE_FLOAT_FLEX_START, name_buffer,
                   sizeof(name_buffer));
  assert(strings_equal(name_buffer, "float_flex_d1"));
  twd_feature_name(TWD_FEATURE_FLOAT_SCORE_START + 1, name_buffer,
                   sizeof(name_buffer));
  assert(strings_equal(name_buffer, "float_score_d2"));
  twd_feature_name(TWD_FEATURE_TT_FLOATER, name_buffer, sizeof(name_buffer));
  assert(strings_equal(name_buffer, "tt_floater"));
  twd_feature_name(TWD_FEATURE_TT_HOOK_ONLY, name_buffer, sizeof(name_buffer));
  assert(strings_equal(name_buffer, "tt_hook_only"));
}

static void test_twd_round_trip(const char *data_dir) {
  TWDWeights *twd = twd_create_zeroed("round_trip");
  assert(strings_equal(twd_get_name(twd), "round_trip"));
  assert(twd_get_mutation_counter(twd) == 0);
  for (int feature_index = 0; feature_index < TWD_NUM_FEATURES;
       feature_index++) {
    assert(twd_get_weight(twd, feature_index) == 0);
    twd_set_weight(twd, feature_index, -100 * (feature_index + 1));
  }
  twd_bump_mutation_counter(twd);
  assert(twd_get_mutation_counter(twd) == 1);

  ErrorStack *error_stack = error_stack_create();
  twd_write(twd, data_dir, "round_trip", error_stack);
  assert(error_stack_is_empty(error_stack));

  TWDWeights *loaded = twd_create(data_dir, "round_trip", error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(loaded);
  assert(strings_equal(twd_get_name(loaded), "round_trip"));
  for (int feature_index = 0; feature_index < TWD_NUM_FEATURES;
       feature_index++) {
    assert(twd_get_weight(loaded, feature_index) ==
           twd_get_weight(twd, feature_index));
  }
  error_stack_destroy(error_stack);
  twd_destroy(loaded);
  twd_destroy(twd);
}

static void test_twd_invalid_files(const char *data_dir) {
  // Wrong header
  assert_twd_create_fails(data_dir, "bad_header",
                          "magpie_twd_v0\nhook_d2,-1\n");
  // Positive weight
  char *contents = get_formatted_string("%s\nhook_d1,1\n", TWD_MAGIC_HEADER);
  assert_twd_create_fails(data_dir, "positive_weight", contents);
  free(contents);
  // Missing comma
  contents = get_formatted_string("%s\nhook_d1 -1\n", TWD_MAGIC_HEADER);
  assert_twd_create_fails(data_dir, "missing_comma", contents);
  free(contents);
  // Wrong feature name
  contents = get_formatted_string("%s\nnot_a_feature,-1\n", TWD_MAGIC_HEADER);
  assert_twd_create_fails(data_dir, "wrong_name", contents);
  free(contents);
  // Too few rows
  contents =
      get_formatted_string("%s\nhook_d1,-1\n# a comment\n\n", TWD_MAGIC_HEADER);
  assert_twd_create_fails(data_dir, "too_few_rows", contents);
  free(contents);
}

static void test_twd_comments_and_blank_lines(const char *data_dir) {
  StringBuilder *sb = string_builder_create();
  string_builder_add_formatted_string(sb, "%s\n# leading comment\n\n",
                                      TWD_MAGIC_HEADER);
  char feature_name[64];
  for (int feature_index = 0; feature_index < TWD_NUM_FEATURES;
       feature_index++) {
    twd_feature_name(feature_index, feature_name, sizeof(feature_name));
    string_builder_add_formatted_string(sb, "%s,%d\n# comment %d\n",
                                        feature_name, -feature_index,
                                        feature_index);
  }
  write_twd_file_contents(data_dir, "commented", string_builder_peek(sb));
  string_builder_destroy(sb);

  ErrorStack *error_stack = error_stack_create();
  TWDWeights *loaded = twd_create(data_dir, "commented", error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(loaded);
  for (int feature_index = 0; feature_index < TWD_NUM_FEATURES;
       feature_index++) {
    assert(twd_get_weight(loaded, feature_index) == -feature_index);
  }
  error_stack_destroy(error_stack);
  twd_destroy(loaded);
}

void test_tws_defense(void) {
  char *data_dir = create_temp_twd_data_dir();
  test_twd_feature_names();
  test_twd_round_trip(data_dir);
  test_twd_invalid_files(data_dir);
  test_twd_comments_and_blank_lines(data_dir);
  free(data_dir);
}
