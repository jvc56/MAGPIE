#include "tws_defense.h"

#include "../def/tws_defense_defs.h"
#include "../util/fileproxy.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "data_filepaths.h"
#include "equity.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct TWDWeights {
  char *name;
  Equity weights[TWD_NUM_FEATURES];
  uint64_t mutation_counter;
};

const char *twd_get_name(const TWDWeights *twd) { return twd->name; }

Equity twd_get_weight(const TWDWeights *twd, int feature_index) {
  return twd->weights[feature_index];
}

void twd_set_weight(TWDWeights *twd, int feature_index, Equity weight) {
  if (weight > 0) {
    log_fatal("TWS defense weight for feature %d must be <= 0, got %d",
              feature_index, weight);
  }
  twd->weights[feature_index] = weight;
}

uint64_t twd_get_mutation_counter(const TWDWeights *twd) {
  return twd->mutation_counter;
}

void twd_bump_mutation_counter(TWDWeights *twd) { twd->mutation_counter++; }

void twd_feature_name(int feature_index, char *buf, size_t buf_size) {
  if (feature_index >= TWD_FEATURE_HOOK_START &&
      feature_index < TWD_FEATURE_FLOAT_FLEX_START) {
    snprintf(buf, buf_size, "hook_d%d",
             feature_index - TWD_FEATURE_HOOK_START + 1);
  } else if (feature_index < TWD_FEATURE_FLOAT_SCORE_START) {
    snprintf(buf, buf_size, "float_flex_d%d",
             feature_index - TWD_FEATURE_FLOAT_FLEX_START + 1);
  } else if (feature_index < TWD_FEATURE_TT_FLOATER) {
    snprintf(buf, buf_size, "float_score_d%d",
             feature_index - TWD_FEATURE_FLOAT_SCORE_START + 1);
  } else if (feature_index == TWD_FEATURE_TT_FLOATER) {
    snprintf(buf, buf_size, "tt_floater");
  } else if (feature_index == TWD_FEATURE_TT_HOOK_ONLY) {
    snprintf(buf, buf_size, "tt_hook_only");
  } else {
    log_fatal("invalid TWS defense feature index: %d", feature_index);
  }
}

TWDWeights *twd_create_zeroed(const char *twd_name) {
  TWDWeights *twd = calloc_or_die(1, sizeof(TWDWeights));
  twd->name = string_duplicate(twd_name);
  return twd;
}

void twd_destroy(TWDWeights *twd) {
  if (!twd) {
    return;
  }
  free(twd->name);
  free(twd);
}

// Parses the weights file contents into twd. The format is:
//   line 1: the magic header (TWD_MAGIC_HEADER)
//   then, ignoring empty lines and lines starting with '#', exactly
//   TWD_NUM_FEATURES lines of "<feature_name>,<millipoints>", in canonical
//   feature order, every value <= 0.
static void twd_parse_contents(TWDWeights *twd, const char *twd_name,
                               const StringSplitter *split_contents,
                               ErrorStack *error_stack) {
  const int num_lines = string_splitter_get_number_of_items(split_contents);
  if (num_lines < 1 ||
      !strings_equal(string_splitter_get_item(split_contents, 0),
                     TWD_MAGIC_HEADER)) {
    error_stack_push(
        error_stack, ERROR_STATUS_TWD_INVALID_HEADER,
        get_formatted_string(
            "TWS defense file '%s' does not start with the header '%s'",
            twd_name, TWD_MAGIC_HEADER));
    return;
  }
  int feature_index = 0;
  char expected_name[64];
  for (int line_index = 1; line_index < num_lines; line_index++) {
    const char *line = string_splitter_get_item(split_contents, line_index);
    if (is_string_empty_or_whitespace(line) || line[0] == '#') {
      continue;
    }
    if (feature_index >= TWD_NUM_FEATURES) {
      error_stack_push(error_stack, ERROR_STATUS_TWD_WRONG_NUMBER_OF_ROWS,
                       get_formatted_string(
                           "TWS defense file '%s' has more than %d weight rows",
                           twd_name, TWD_NUM_FEATURES));
      return;
    }
    const char *comma = strchr(line, ',');
    if (!comma) {
      error_stack_push(
          error_stack, ERROR_STATUS_TWD_INVALID_ROW,
          get_formatted_string(
              "TWS defense file '%s' line %d is not '<name>,<value>': %s",
              twd_name, line_index + 1, line));
      return;
    }
    twd_feature_name(feature_index, expected_name, sizeof(expected_name));
    const size_t name_length = (size_t)(comma - line);
    if (strlen(expected_name) != name_length ||
        strncmp(line, expected_name, name_length) != 0) {
      error_stack_push(
          error_stack, ERROR_STATUS_TWD_INVALID_ROW,
          get_formatted_string("TWS defense file '%s' line %d names feature "
                               "'%.*s' but '%s' was expected",
                               twd_name, line_index + 1, (int)name_length, line,
                               expected_name));
      return;
    }
    const int weight = string_to_int(comma + 1, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      error_stack_push(
          error_stack, ERROR_STATUS_TWD_INVALID_ROW,
          get_formatted_string(
              "TWS defense file '%s' line %d has an invalid weight: %s",
              twd_name, line_index + 1, comma + 1));
      return;
    }
    if (weight > 0) {
      error_stack_push(
          error_stack, ERROR_STATUS_TWD_POSITIVE_WEIGHT,
          get_formatted_string(
              "TWS defense file '%s' line %d has a positive weight (%d); "
              "applied weights must be <= 0 so the defense term can never "
              "increase a move's equity",
              twd_name, line_index + 1, weight));
      return;
    }
    twd->weights[feature_index] = weight;
    feature_index++;
  }
  if (feature_index != TWD_NUM_FEATURES) {
    error_stack_push(
        error_stack, ERROR_STATUS_TWD_WRONG_NUMBER_OF_ROWS,
        get_formatted_string(
            "TWS defense file '%s' has %d weight rows but %d were expected",
            twd_name, feature_index, TWD_NUM_FEATURES));
  }
}

TWDWeights *twd_create(const char *data_paths, const char *twd_name,
                       ErrorStack *error_stack) {
  char *twd_filename = data_filepaths_get_readable_filename(
      data_paths, twd_name, DATA_FILEPATH_TYPE_TWS_DEFENSE, error_stack);
  TWDWeights *twd = NULL;
  if (error_stack_is_empty(error_stack)) {
    char *file_contents =
        fileproxy_get_string_from_filename(twd_filename, error_stack);
    if (error_stack_is_empty(error_stack)) {
      StringSplitter *split_contents =
          split_string_by_newline(file_contents, error_stack);
      if (error_stack_is_empty(error_stack)) {
        twd = twd_create_zeroed(twd_name);
        twd_parse_contents(twd, twd_name, split_contents, error_stack);
      }
      string_splitter_destroy(split_contents);
    }
    free(file_contents);
  }
  free(twd_filename);
  if (!error_stack_is_empty(error_stack)) {
    twd_destroy(twd);
    twd = NULL;
  }
  return twd;
}

void twd_write(const TWDWeights *twd, const char *data_paths,
               const char *twd_name, ErrorStack *error_stack) {
  char *twd_filename = data_filepaths_get_writable_filename(
      data_paths, twd_name, DATA_FILEPATH_TYPE_TWS_DEFENSE, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    free(twd_filename);
    return;
  }
  StringBuilder *sb = string_builder_create();
  string_builder_add_formatted_string(sb, "%s\n", TWD_MAGIC_HEADER);
  string_builder_add_string(
      sb, "# trained TWS defense weights; units: milli-equity per feature "
          "unit; all values <= 0\n");
  char feature_name[64];
  for (int feature_index = 0; feature_index < TWD_NUM_FEATURES;
       feature_index++) {
    twd_feature_name(feature_index, feature_name, sizeof(feature_name));
    string_builder_add_formatted_string(sb, "%s,%d\n", feature_name,
                                        twd->weights[feature_index]);
  }
  write_string_to_file(twd_filename, "w", string_builder_peek(sb), error_stack);
  string_builder_destroy(sb);
  free(twd_filename);
}
