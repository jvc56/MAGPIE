#include "blocking_setup_params.h"

#include "../util/fileproxy.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "data_filepaths.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

typedef struct BlockingSetupParamsBin {
  int min_bag;
  int max_bag;
  int min_lead;
  int max_lead;
  double blocking_weight;
  double setup_weight;
} BlockingSetupParamsBin;

struct BlockingSetupParams {
  char *name;
  char *lexicon;
  char *model_version;
  char *objective;
  int teacher_racks;
  bool teacher_partition;
  bool teacher_condition_draws;
  int teacher_followup_draws;
  blocking_setup_value_t teacher_value;
  bool has_blocking_weight;
  bool has_setup_weight;
  double blocking_weight;
  double setup_weight;
  int num_bins;
  BlockingSetupParamsBin bins[BLOCKING_SETUP_PARAMS_MAX_BINS];
};

enum {
  // The settings of the studies the parameter format came from; files may
  // override each one.
  BSP_DEFAULT_TEACHER_RACKS = 64,
  BSP_DEFAULT_TEACHER_FOLLOWUP_DRAWS = 1,
  BSP_BIN_FIELDS = 7,
};

void blocking_setup_params_destroy(BlockingSetupParams *params) {
  if (params == NULL) {
    return;
  }
  free(params->name);
  free(params->lexicon);
  free(params->model_version);
  free(params->objective);
  free(params);
}

static void bsp_push_error(ErrorStack *error_stack, error_code_t code,
                           const char *name, int line_number,
                           const char *detail) {
  error_stack_push(
      error_stack, code,
      get_formatted_string("blocking/setup parameters '%s' line %d: %s", name,
                           line_number, detail));
}

static bool bsp_parse_int(const char *text, const char *name, int line_number,
                          int *value, ErrorStack *error_stack) {
  ErrorStack *parse_errors = error_stack_create();
  *value = string_to_int(text, parse_errors);
  const bool ok = error_stack_is_empty(parse_errors);
  error_stack_destroy(parse_errors);
  if (!ok) {
    bsp_push_error(error_stack, ERROR_STATUS_BSP_INVALID_ROW, name, line_number,
                   "expected an integer");
  }
  return ok;
}

static bool bsp_parse_double(const char *text, const char *name,
                             int line_number, double *value,
                             ErrorStack *error_stack) {
  ErrorStack *parse_errors = error_stack_create();
  *value = string_to_double(text, parse_errors);
  const bool ok = error_stack_is_empty(parse_errors);
  error_stack_destroy(parse_errors);
  if (!ok) {
    bsp_push_error(error_stack, ERROR_STATUS_BSP_INVALID_ROW, name, line_number,
                   "expected a number");
  }
  return ok;
}

static bool bsp_parse_bool(const char *text, const char *name, int line_number,
                           bool *value, ErrorStack *error_stack) {
  int parsed = 0;
  if (!bsp_parse_int(text, name, line_number, &parsed, error_stack)) {
    return false;
  }
  if (parsed != 0 && parsed != 1) {
    bsp_push_error(error_stack, ERROR_STATUS_BSP_INVALID_ROW, name, line_number,
                   "expected 0 or 1");
    return false;
  }
  *value = parsed == 1;
  return true;
}

static void bsp_parse_bin(BlockingSetupParams *params, const char *value,
                          int line_number, ErrorStack *error_stack) {
  if (params->num_bins >= BLOCKING_SETUP_PARAMS_MAX_BINS) {
    bsp_push_error(error_stack, ERROR_STATUS_BSP_INVALID_ROW, params->name,
                   line_number, "too many bins");
    return;
  }
  StringSplitter *fields = split_string(value, ',', false);
  if (string_splitter_get_number_of_items(fields) != BSP_BIN_FIELDS - 1) {
    bsp_push_error(error_stack, ERROR_STATUS_BSP_INVALID_ROW, params->name,
                   line_number,
                   "bin needs min_bag,max_bag,min_lead,max_lead,"
                   "blocking_weight,setup_weight");
    string_splitter_destroy(fields);
    return;
  }
  BlockingSetupParamsBin *bin = &params->bins[params->num_bins];
  int *int_fields[] = {&bin->min_bag, &bin->max_bag, &bin->min_lead,
                       &bin->max_lead};
  bool ok = true;
  for (int field_idx = 0; field_idx < 4 && ok; field_idx++) {
    ok =
        bsp_parse_int(string_splitter_get_item(fields, field_idx), params->name,
                      line_number, int_fields[field_idx], error_stack);
  }
  ok = ok && bsp_parse_double(string_splitter_get_item(fields, 4), params->name,
                              line_number, &bin->blocking_weight, error_stack);
  ok = ok && bsp_parse_double(string_splitter_get_item(fields, 5), params->name,
                              line_number, &bin->setup_weight, error_stack);
  string_splitter_destroy(fields);
  if (!ok) {
    return;
  }
  if (bin->min_bag > bin->max_bag || bin->min_lead > bin->max_lead) {
    bsp_push_error(error_stack, ERROR_STATUS_BSP_INVALID_ROW, params->name,
                   line_number, "bin range is empty");
    return;
  }
  params->num_bins++;
}

static void bsp_set_text(char **field, const char *value) {
  free(*field);
  *field = string_duplicate(value);
}

// One "key,value" row; value is everything after the first comma.
static void bsp_parse_row(BlockingSetupParams *params, const char *line,
                          int line_number, ErrorStack *error_stack) {
  const char *comma = strchr(line, ',');
  if (comma == NULL || comma == line) {
    bsp_push_error(error_stack, ERROR_STATUS_BSP_INVALID_ROW, params->name,
                   line_number, "expected key,value");
    return;
  }
  const size_t key_length = (size_t)(comma - line);
  char key[BLOCKING_SETUP_PARAMS_MAX_TEXT];
  if (key_length >= sizeof(key)) {
    bsp_push_error(error_stack, ERROR_STATUS_BSP_INVALID_ROW, params->name,
                   line_number, "key too long");
    return;
  }
  memcpy(key, line, key_length);
  key[key_length] = '\0';
  const char *value = comma + 1;
  if (strings_equal(key, "lexicon")) {
    bsp_set_text(&params->lexicon, value);
  } else if (strings_equal(key, "model_version")) {
    bsp_set_text(&params->model_version, value);
  } else if (strings_equal(key, "objective")) {
    bsp_set_text(&params->objective, value);
  } else if (strings_equal(key, "provenance")) {
    // Kept in the file for people; nothing reads it.
  } else if (strings_equal(key, "teacher_racks")) {
    if (bsp_parse_int(value, params->name, line_number, &params->teacher_racks,
                      error_stack) &&
        params->teacher_racks < 1) {
      bsp_push_error(error_stack, ERROR_STATUS_BSP_INVALID_ROW, params->name,
                     line_number, "teacher_racks must be positive");
    }
  } else if (strings_equal(key, "teacher_partition")) {
    bsp_parse_bool(value, params->name, line_number, &params->teacher_partition,
                   error_stack);
  } else if (strings_equal(key, "teacher_condition_draws")) {
    bsp_parse_bool(value, params->name, line_number,
                   &params->teacher_condition_draws, error_stack);
  } else if (strings_equal(key, "teacher_followup_draws")) {
    if (bsp_parse_int(value, params->name, line_number,
                      &params->teacher_followup_draws, error_stack) &&
        params->teacher_followup_draws < 1) {
      bsp_push_error(error_stack, ERROR_STATUS_BSP_INVALID_ROW, params->name,
                     line_number, "teacher_followup_draws must be positive");
    }
  } else if (strings_equal(key, "teacher_value")) {
    if (strings_equal(value, "score")) {
      params->teacher_value = BLOCKING_SETUP_VALUE_SCORE;
    } else if (strings_equal(value, "equity_score")) {
      params->teacher_value = BLOCKING_SETUP_VALUE_EQUITY_SCORE;
    } else if (strings_equal(value, "equity")) {
      params->teacher_value = BLOCKING_SETUP_VALUE_EQUITY;
    } else {
      bsp_push_error(error_stack, ERROR_STATUS_BSP_INVALID_ROW, params->name,
                     line_number,
                     "teacher_value must be score, equity_score or equity");
    }
  } else if (strings_equal(key, "blocking_weight")) {
    params->has_blocking_weight =
        bsp_parse_double(value, params->name, line_number,
                         &params->blocking_weight, error_stack);
  } else if (strings_equal(key, "setup_weight")) {
    params->has_setup_weight = bsp_parse_double(
        value, params->name, line_number, &params->setup_weight, error_stack);
  } else if (strings_equal(key, "bin")) {
    bsp_parse_bin(params, value, line_number, error_stack);
  } else {
    bsp_push_error(error_stack, ERROR_STATUS_BSP_INVALID_ROW, params->name,
                   line_number, "unknown key");
  }
}

static void bsp_parse_header(const BlockingSetupParams *params,
                             const char *line, int line_number,
                             ErrorStack *error_stack) {
  if (!has_prefix(BLOCKING_SETUP_PARAMS_MAGIC_PREFIX, line)) {
    bsp_push_error(
        error_stack, ERROR_STATUS_BSP_INVALID_HEADER, params->name, line_number,
        "expected header " BLOCKING_SETUP_PARAMS_MAGIC_PREFIX "<version>");
    return;
  }
  int version = 0;
  if (!bsp_parse_int(line + strlen(BLOCKING_SETUP_PARAMS_MAGIC_PREFIX),
                     params->name, line_number, &version, error_stack)) {
    return;
  }
  if (version != BLOCKING_SETUP_PARAMS_VERSION) {
    bsp_push_error(error_stack, ERROR_STATUS_BSP_UNSUPPORTED_VERSION,
                   params->name, line_number, "unsupported version");
  }
}

static void bsp_require(const BlockingSetupParams *params, bool present,
                        const char *key, ErrorStack *error_stack) {
  if (!present && error_stack_is_empty(error_stack)) {
    error_stack_push(
        error_stack, ERROR_STATUS_BSP_MISSING_ROW,
        get_formatted_string("blocking/setup parameters '%s' have no %s row",
                             params->name, key));
  }
}

BlockingSetupParams *
blocking_setup_params_create_from_string(const char *name, const char *contents,
                                         ErrorStack *error_stack) {
  BlockingSetupParams *params = calloc_or_die(1, sizeof(BlockingSetupParams));
  params->name = string_duplicate(name);
  params->teacher_racks = BSP_DEFAULT_TEACHER_RACKS;
  params->teacher_partition = true;
  params->teacher_condition_draws = true;
  params->teacher_followup_draws = BSP_DEFAULT_TEACHER_FOLLOWUP_DRAWS;
  StringSplitter *lines = split_string_by_newline(contents, false);
  bool seen_header = false;
  const int num_lines = string_splitter_get_number_of_items(lines);
  for (int line_idx = 0;
       line_idx < num_lines && error_stack_is_empty(error_stack); line_idx++) {
    char *line = string_duplicate(string_splitter_get_item(lines, line_idx));
    trim_whitespace(line);
    if (line[0] != '\0' && line[0] != '#') {
      if (!seen_header) {
        bsp_parse_header(params, line, line_idx + 1, error_stack);
        seen_header = true;
      } else {
        bsp_parse_row(params, line, line_idx + 1, error_stack);
      }
    }
    free(line);
  }
  string_splitter_destroy(lines);
  bsp_require(params, seen_header, "header", error_stack);
  bsp_require(params, params->lexicon != NULL, "lexicon", error_stack);
  bsp_require(params, params->model_version != NULL, "model_version",
              error_stack);
  bsp_require(params, params->objective != NULL, "objective", error_stack);
  bsp_require(params, params->has_blocking_weight, "blocking_weight",
              error_stack);
  bsp_require(params, params->has_setup_weight, "setup_weight", error_stack);
  if (!error_stack_is_empty(error_stack)) {
    blocking_setup_params_destroy(params);
    return NULL;
  }
  return params;
}

BlockingSetupParams *blocking_setup_params_create(const char *data_paths,
                                                  const char *name,
                                                  ErrorStack *error_stack) {
  char *filename = data_filepaths_get_readable_filename(
      data_paths, name, DATA_FILEPATH_TYPE_BLOCKING_SETUP, error_stack);
  BlockingSetupParams *params = NULL;
  if (error_stack_is_empty(error_stack)) {
    char *contents = fileproxy_get_string_from_filename(filename, error_stack);
    if (error_stack_is_empty(error_stack)) {
      params =
          blocking_setup_params_create_from_string(name, contents, error_stack);
    }
    free(contents);
  }
  free(filename);
  return params;
}

const char *blocking_setup_params_get_name(const BlockingSetupParams *params) {
  return params->name;
}

const char *
blocking_setup_params_get_lexicon(const BlockingSetupParams *params) {
  return params->lexicon;
}

const char *
blocking_setup_params_get_model_version(const BlockingSetupParams *params) {
  return params->model_version;
}

const char *
blocking_setup_params_get_objective(const BlockingSetupParams *params) {
  return params->objective;
}

int blocking_setup_params_get_teacher_racks(const BlockingSetupParams *params) {
  return params->teacher_racks;
}

bool blocking_setup_params_get_teacher_partition(
    const BlockingSetupParams *params) {
  return params->teacher_partition;
}

bool blocking_setup_params_get_teacher_condition_draws(
    const BlockingSetupParams *params) {
  return params->teacher_condition_draws;
}

int blocking_setup_params_get_teacher_followup_draws(
    const BlockingSetupParams *params) {
  return params->teacher_followup_draws;
}

blocking_setup_value_t
blocking_setup_params_get_teacher_value(const BlockingSetupParams *params) {
  return params->teacher_value;
}

int blocking_setup_params_get_num_bins(const BlockingSetupParams *params) {
  return params->num_bins;
}

void blocking_setup_params_get_weights(const BlockingSetupParams *params,
                                       int bag, int lead,
                                       double *blocking_weight,
                                       double *setup_weight) {
  for (int bin_idx = 0; bin_idx < params->num_bins; bin_idx++) {
    const BlockingSetupParamsBin *bin = &params->bins[bin_idx];
    if (bag >= bin->min_bag && bag <= bin->max_bag && lead >= bin->min_lead &&
        lead <= bin->max_lead) {
      *blocking_weight = bin->blocking_weight;
      *setup_weight = bin->setup_weight;
      return;
    }
  }
  *blocking_weight = params->blocking_weight;
  *setup_weight = params->setup_weight;
}
