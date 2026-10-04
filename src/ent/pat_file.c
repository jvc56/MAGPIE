#include "pat_file.h"

#include "../def/pat_defs.h"
#include "../def/rack_defs.h"
#include "../util/fileproxy.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "data_filepaths.h"
#include "pat.h"
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

// Rows that versions 3 to 5 wrote for channels and training flags retired
// in version 6: the hypergeometric-scaled channels, the letter-multiplier
// channels, and the fit_scaled, train_overlay and fit_shrink flags. Every
// file trained with them left at zero plays the same without them, so a
// zero row is skipped; a nonzero one describes a model this build cannot
// evaluate and is rejected.
static const char *const pat_retired_row_prefixes[] = {
    "hook_scaled_d", "float_flex_scaled_d", "lm_span_d",
    "lm_ext_d",      "dws_lm_span_d",       "dws_lm_ext_d",
    "fit_scaled,",   "train_overlay,",      "fit_shrink,",
};

static bool pat_row_is_retired(const char *line) {
  const int num_prefixes = (int)(sizeof(pat_retired_row_prefixes) /
                                 sizeof(pat_retired_row_prefixes[0]));
  for (int prefix_idx = 0; prefix_idx < num_prefixes; prefix_idx++) {
    if (has_prefix(pat_retired_row_prefixes[prefix_idx], line)) {
      return true;
    }
  }
  return false;
}

// Parses the weights file contents into pat. The format is:
//   line 1: the magic header, PAT_MAGIC_PREFIX followed by the format
//     version as a decimal integer (e.g. "magpie_pat_v2")
//   then, ignoring empty lines and lines starting with '#', one line of
//   "<feature_name>,<millipoints>" per feature that version has, in
//   canonical feature order, every value <= 0. A version 1 file omits the
//   double letter square rows added in version 2; those weights stay zero.
static void pat_parse_contents(PATWeights *pat, const char *pat_name,
                               const StringSplitter *split_contents,
                               ErrorStack *error_stack) {
  const int num_lines = string_splitter_get_number_of_items(split_contents);
  const char *header_line =
      num_lines > 0 ? string_splitter_get_item(split_contents, 0) : NULL;
  if (!header_line || !has_prefix(PAT_MAGIC_PREFIX, header_line)) {
    error_stack_push(
        error_stack, ERROR_STATUS_PAT_INVALID_HEADER,
        get_formatted_string(
            "PAT file '%s' does not start with the header '%sN' for some "
            "version N",
            pat_name, PAT_MAGIC_PREFIX));
    return;
  }
  const int version =
      string_to_int(header_line + strlen(PAT_MAGIC_PREFIX), error_stack);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_push(
        error_stack, ERROR_STATUS_PAT_INVALID_HEADER,
        get_formatted_string(
            "PAT file '%s' header '%s' does not end in a version number",
            pat_name, header_line));
    return;
  }
  if (version < PAT_EARLIEST_SUPPORTED_VERSION) {
    error_stack_push(
        error_stack, ERROR_STATUS_PAT_UNSUPPORTED_VERSION,
        get_formatted_string(
            "PAT file '%s' is version %d but only %d or greater is "
            "supported",
            pat_name, version, PAT_EARLIEST_SUPPORTED_VERSION));
    return;
  }
  if (version > PAT_VERSION) {
    error_stack_push(
        error_stack, ERROR_STATUS_PAT_UNSUPPORTED_VERSION,
        get_formatted_string(
            "PAT file '%s' is version %d but this build reads versions up "
            "to %d",
            pat_name, version, PAT_VERSION));
    return;
  }
  pat->version = version;
  int feature_index = 0;
  char expected_name[64];
  for (int line_index = 1; line_index < num_lines; line_index++) {
    const char *line = string_splitter_get_item(split_contents, line_index);
    if (is_string_empty_or_whitespace(line) || line[0] == '#') {
      continue;
    }
    // A version 1 file has no double letter square rows: they were added
    // in version 2. Skip past that range so the next row is matched
    // against the feature that actually follows it in an old file; the
    // weights array already reads zero there from the zeroed allocation.
    if (version < 2 && feature_index >= PAT_FEATURE_DLS_HOOK_START &&
        feature_index < PAT_FEATURE_QWS_HOOK_START) {
      feature_index = PAT_FEATURE_QWS_HOOK_START;
    }
    // A version below 4 has no hook-score rows: added in version 4.
    if (version < 4 && feature_index >= PAT_FEATURE_HOOK_SCORE_START &&
        feature_index < PAT_FEATURE_DWS_HOOK_START) {
      feature_index = PAT_FEATURE_DWS_HOOK_START;
    }
    if (version < 6 && pat_row_is_retired(line)) {
      const char *comma = strchr(line, ',');
      const int value = comma ? string_to_int(comma + 1, error_stack) : -1;
      if (!error_stack_is_empty(error_stack) || value != 0) {
        error_stack_push(
            error_stack, ERROR_STATUS_PAT_INVALID_ROW,
            get_formatted_string("PAT file '%s' line %d is a retired row "
                                 "with a value other than 0: '%s'",
                                 pat_name, line_index + 1, line));
        return;
      }
      continue;
    }
    if (has_prefix(PAT_GAMMA_ROW_PREFIX, line)) {
      const char *gamma_text = line + strlen(PAT_GAMMA_ROW_PREFIX);
      char *gamma_end = NULL;
      const double parsed_gamma = strtod(gamma_text, &gamma_end);
      // Outside [0, 1] the combination stops being a convex one, so it is
      // no longer nondecreasing in each unit penalty and the bound shadow
      // pruning relies on stops being an upper bound: moves would be
      // silently pruned. Unparseable text would otherwise read as 0.0,
      // which is a different model accepted in silence.
      if (gamma_end == gamma_text || !isfinite(parsed_gamma) ||
          parsed_gamma < 0.0 || parsed_gamma > 1.0) {
        error_stack_push(
            error_stack, ERROR_STATUS_PAT_INVALID_ROW,
            get_formatted_string("PAT file '%s' line %d has a "
                                 "combination gamma outside [0, 1]: '%s'",
                                 pat_name, line_index + 1, line));
        return;
      }
      pat->combine_gamma = parsed_gamma;
      continue;
    }
    if (has_prefix(PAT_OWN_ASSET_DISCOUNT_ROW_PREFIX, line)) {
      const char *discount_text =
          line + strlen(PAT_OWN_ASSET_DISCOUNT_ROW_PREFIX);
      char *discount_end = NULL;
      const double parsed_discount = strtod(discount_text, &discount_end);
      // Outside [0, 1] the adjusted penalty p * (1 - discount) could land
      // above 0 (a positive weight would be reachable) or below p (would
      // credit more than the unit's own penalty), either of which breaks
      // the <= 0 invariant the shadow-pruning bound relies on.
      if (discount_end == discount_text || !isfinite(parsed_discount) ||
          parsed_discount < 0.0 || parsed_discount > 1.0) {
        error_stack_push(
            error_stack, ERROR_STATUS_PAT_INVALID_ROW,
            get_formatted_string("PAT file '%s' line %d has an own-asset "
                                 "discount outside [0, 1]: '%s'",
                                 pat_name, line_index + 1, line));
        return;
      }
      pat->own_asset_discount = parsed_discount;
      continue;
    }
    if (has_prefix(PAT_LEXICON_FLOATERS_ROW_PREFIX, line)) {
      const int flag = string_to_int(
          line + strlen(PAT_LEXICON_FLOATERS_ROW_PREFIX), error_stack);
      // Anything but an explicit 0 or 1 is rejected: the two values are
      // different models, and a typo silently reading as one of them
      // would be accepted in silence.
      if (!error_stack_is_empty(error_stack) || (flag != 0 && flag != 1)) {
        error_stack_push(
            error_stack, ERROR_STATUS_PAT_INVALID_ROW,
            get_formatted_string("PAT file '%s' line %d has a lexicon "
                                 "floaters flag other than 0 or 1: '%s'",
                                 pat_name, line_index + 1, line));
        return;
      }
      pat->lexicon_floaters = (flag == 1);
      continue;
    }
    if (has_prefix(PAT_SIGNED_THROUGH_ROW_PREFIX, line)) {
      const int flag = string_to_int(
          line + strlen(PAT_SIGNED_THROUGH_ROW_PREFIX), error_stack);
      if (!error_stack_is_empty(error_stack) || (flag != 0 && flag != 1)) {
        error_stack_push(
            error_stack, ERROR_STATUS_PAT_INVALID_ROW,
            get_formatted_string("PAT file '%s' line %d has a signed "
                                 "through flag other than 0 or 1: '%s'",
                                 pat_name, line_index + 1, line));
        return;
      }
      pat->signed_through = (flag == 1);
      continue;
    }
    if (has_prefix(PAT_FIT_RESIDUAL_ROW_PREFIX, line)) {
      const int flag = string_to_int(line + strlen(PAT_FIT_RESIDUAL_ROW_PREFIX),
                                     error_stack);
      if (!error_stack_is_empty(error_stack) ||
          (flag != PAT_FIT_DEFAULT && flag != PAT_FIT_THROUGH &&
           flag != PAT_FIT_ALL)) {
        error_stack_push(
            error_stack, ERROR_STATUS_PAT_INVALID_ROW,
            get_formatted_string("PAT file '%s' line %d has a fit_residual "
                                 "value other than %d, %d or %d: '%s'",
                                 pat_name, line_index + 1, PAT_FIT_DEFAULT,
                                 PAT_FIT_THROUGH, PAT_FIT_ALL, line));
        return;
      }
      pat->fit_residual = flag;
      continue;
    }
    if (has_prefix(PAT_EXACT_CREATED_HOOKS_ROW_PREFIX, line)) {
      const int flag = string_to_int(
          line + strlen(PAT_EXACT_CREATED_HOOKS_ROW_PREFIX), error_stack);
      if (!error_stack_is_empty(error_stack) || (flag != 0 && flag != 1)) {
        error_stack_push(
            error_stack, ERROR_STATUS_PAT_INVALID_ROW,
            get_formatted_string("PAT file '%s' line %d has an "
                                 "exact_created_hooks flag other than 0 or "
                                 "1: '%s'",
                                 pat_name, line_index + 1, line));
        return;
      }
      pat->exact_created_hooks = (flag == 1);
      continue;
    }
    if (has_prefix(PAT_EXACT_FRESH_RUNS_ROW_PREFIX, line)) {
      const int flag = string_to_int(
          line + strlen(PAT_EXACT_FRESH_RUNS_ROW_PREFIX), error_stack);
      if (!error_stack_is_empty(error_stack) || (flag != 0 && flag != 1)) {
        error_stack_push(
            error_stack, ERROR_STATUS_PAT_INVALID_ROW,
            get_formatted_string("PAT file '%s' line %d has an "
                                 "exact_fresh_runs flag other than 0 or 1: "
                                 "'%s'",
                                 pat_name, line_index + 1, line));
        return;
      }
      pat->exact_fresh_runs = (flag == 1);
      continue;
    }
    if (has_prefix(PAT_HOOK_SCORE_PROB_ROW_PREFIX, line)) {
      const int flag = string_to_int(
          line + strlen(PAT_HOOK_SCORE_PROB_ROW_PREFIX), error_stack);
      if (!error_stack_is_empty(error_stack) || (flag != 0 && flag != 1)) {
        error_stack_push(
            error_stack, ERROR_STATUS_PAT_INVALID_ROW,
            get_formatted_string("PAT file '%s' line %d has a "
                                 "hook_score_prob flag other than 0 or 1: "
                                 "'%s'",
                                 pat_name, line_index + 1, line));
        return;
      }
      pat->hook_score_prob = (flag == 1);
      continue;
    }
    if (has_prefix(PAT_HOOK_VALUE_ROW_PREFIX, line)) {
      const int flag =
          string_to_int(line + strlen(PAT_HOOK_VALUE_ROW_PREFIX), error_stack);
      if (!error_stack_is_empty(error_stack) || (flag != 0 && flag != 1)) {
        error_stack_push(
            error_stack, ERROR_STATUS_PAT_INVALID_ROW,
            get_formatted_string("PAT file '%s' line %d has a hook_value "
                                 "flag other than 0 or 1: '%s'",
                                 pat_name, line_index + 1, line));
        return;
      }
      pat->hook_value = (flag == 1);
      continue;
    }
    if (has_prefix(PAT_FLEX_PROB_ROW_PREFIX, line)) {
      const int flag =
          string_to_int(line + strlen(PAT_FLEX_PROB_ROW_PREFIX), error_stack);
      if (!error_stack_is_empty(error_stack) || (flag != 0 && flag != 1)) {
        error_stack_push(
            error_stack, ERROR_STATUS_PAT_INVALID_ROW,
            get_formatted_string("PAT file '%s' line %d has a flex_prob "
                                 "flag other than 0 or 1: '%s'",
                                 pat_name, line_index + 1, line));
        return;
      }
      pat->flex_prob = (flag == 1);
      continue;
    }
    if (has_prefix(PAT_OPENING_TILES_ROW_PREFIX, line) ||
        has_prefix(PAT_OPENING_EXCHANGE_ROW_PREFIX, line)) {
      const bool is_exchange =
          has_prefix(PAT_OPENING_EXCHANGE_ROW_PREFIX, line);
      int tiles = 0;
      const char *value_text;
      if (is_exchange) {
        value_text = line + strlen(PAT_OPENING_EXCHANGE_ROW_PREFIX);
      } else {
        const char *tiles_text = line + strlen(PAT_OPENING_TILES_ROW_PREFIX);
        char *tiles_end = NULL;
        tiles = (int)strtol(tiles_text, &tiles_end, 10);
        if (tiles_end == tiles_text || *tiles_end != ',' || tiles < 1 ||
            tiles > RACK_SIZE) {
          error_stack_push(
              error_stack, ERROR_STATUS_PAT_INVALID_ROW,
              get_formatted_string("PAT file '%s' line %d is not "
                                   "'opening_tiles_<1..%d>,<value>': '%s'",
                                   pat_name, line_index + 1, RACK_SIZE, line));
          return;
        }
        value_text = tiles_end + 1;
      }
      const int adjustment = string_to_int(value_text, error_stack);
      if (!error_stack_is_empty(error_stack) || adjustment > 0) {
        error_stack_push(
            error_stack, ERROR_STATUS_PAT_INVALID_ROW,
            get_formatted_string("PAT file '%s' line %d has an opening "
                                 "adjustment that is not an integer <= 0: '%s'",
                                 pat_name, line_index + 1, line));
        return;
      }
      if (is_exchange) {
        pat->opening_exchange = adjustment;
      } else {
        pat->opening_tiles[tiles] = adjustment;
      }
      continue;
    }
    {
      static const char *const stage_prefixes[PAT_STAGE_COUNT] = {
          PAT_STAGE_SCALE_EARLY_ROW_PREFIX, PAT_STAGE_SCALE_MID_ROW_PREFIX,
          PAT_STAGE_SCALE_LATE_ROW_PREFIX};
      int stage = 0;
      while (stage < PAT_STAGE_COUNT &&
             !has_prefix(stage_prefixes[stage], line)) {
        stage++;
      }
      if (stage < PAT_STAGE_COUNT) {
        const char *scale_text = line + strlen(stage_prefixes[stage]);
        char *scale_end = NULL;
        const double parsed_scale = strtod(scale_text, &scale_end);
        // Negative would flip the term's sign and break every bound;
        // unparseable text would read as 0.0, a different model accepted
        // in silence.
        if (scale_end == scale_text || !isfinite(parsed_scale) ||
            parsed_scale < 0.0) {
          error_stack_push(
              error_stack, ERROR_STATUS_PAT_INVALID_ROW,
              get_formatted_string("PAT file '%s' line %d has a stage scale "
                                   "that is not a finite number >= 0: '%s'",
                                   pat_name, line_index + 1, line));
          return;
        }
        pat->stage_scale[stage] = parsed_scale;
        continue;
      }
    }
    if (has_prefix(PAT_UTILITY_ADJUST_ROW_PREFIX, line)) {
      const char *text = line + strlen(PAT_UTILITY_ADJUST_ROW_PREFIX);
      char *end = NULL;
      const double parsed = strtod(text, &end);
      if (end == text || parsed < 0.0) {
        error_stack_push(
            error_stack, ERROR_STATUS_PAT_INVALID_ROW,
            get_formatted_string("PAT file '%s' line %d has a utility_adjust "
                                 "that is not a nonnegative number: '%s'",
                                 pat_name, line_index + 1, line));
        return;
      }
      pat->utility_adjust = parsed;
      continue;
    }
    if (has_prefix(PAT_RUN_THROUGH_ROW_PREFIX, line)) {
      const int flag =
          string_to_int(line + strlen(PAT_RUN_THROUGH_ROW_PREFIX), error_stack);
      if (!error_stack_is_empty(error_stack) || (flag != 0 && flag != 1)) {
        error_stack_push(
            error_stack, ERROR_STATUS_PAT_INVALID_ROW,
            get_formatted_string("PAT file '%s' line %d has a run_through "
                                 "flag other than 0 or 1: '%s'",
                                 pat_name, line_index + 1, line));
        return;
      }
      pat->run_through = (flag == 1);
      continue;
    }
    if (feature_index >= PAT_NUM_FEATURES) {
      error_stack_push(
          error_stack, ERROR_STATUS_PAT_WRONG_NUMBER_OF_ROWS,
          get_formatted_string("PAT file '%s' has more than %d weight rows",
                               pat_name, PAT_NUM_FEATURES));
      return;
    }
    const char *comma = strchr(line, ',');
    if (!comma) {
      error_stack_push(error_stack, ERROR_STATUS_PAT_INVALID_ROW,
                       get_formatted_string(
                           "PAT file '%s' line %d is not '<name>,<value>': %s",
                           pat_name, line_index + 1, line));
      return;
    }
    pat_feature_name(feature_index, expected_name, sizeof(expected_name));
    const size_t name_length = (size_t)(comma - line);
    if (strlen(expected_name) != name_length ||
        strncmp(line, expected_name, name_length) != 0) {
      error_stack_push(
          error_stack, ERROR_STATUS_PAT_INVALID_ROW,
          get_formatted_string("PAT file '%s' line %d names feature "
                               "'%.*s' but '%s' was expected",
                               pat_name, line_index + 1, (int)name_length, line,
                               expected_name));
      return;
    }
    const int weight = string_to_int(comma + 1, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      error_stack_push(error_stack, ERROR_STATUS_PAT_INVALID_ROW,
                       get_formatted_string(
                           "PAT file '%s' line %d has an invalid weight: %s",
                           pat_name, line_index + 1, comma + 1));
      return;
    }
    if (weight > 0) {
      error_stack_push(
          error_stack, ERROR_STATUS_PAT_POSITIVE_WEIGHT,
          get_formatted_string(
              "PAT file '%s' line %d has a positive weight (%d); "
              "applied weights must be <= 0 so the defense term can never "
              "increase a move's equity",
              pat_name, line_index + 1, weight));
      return;
    }
    pat->weights[feature_index] = weight;
    feature_index++;
  }
  // A version below 7 has no hook_excess rows: added in version 7, last.
  if (version < 7 && feature_index == PAT_FEATURE_HOOK_EXCESS_START) {
    feature_index = PAT_NUM_FEATURES;
  }
  if (feature_index != PAT_NUM_FEATURES) {
    error_stack_push(
        error_stack, ERROR_STATUS_PAT_WRONG_NUMBER_OF_ROWS,
        get_formatted_string(
            "PAT file '%s' has %d weight rows but %d were expected", pat_name,
            feature_index, PAT_NUM_FEATURES));
  }
}

PATWeights *pat_create(const char *data_paths, const char *pat_name,
                       ErrorStack *error_stack) {
  char *pat_filename = data_filepaths_get_readable_filename(
      data_paths, pat_name, DATA_FILEPATH_TYPE_PAT, error_stack);
  PATWeights *pat = NULL;
  if (error_stack_is_empty(error_stack)) {
    char *file_contents =
        fileproxy_get_string_from_filename(pat_filename, error_stack);
    if (error_stack_is_empty(error_stack)) {
      StringSplitter *split_contents =
          split_string_by_newline(file_contents, error_stack);
      if (error_stack_is_empty(error_stack)) {
        pat = pat_create_zeroed(pat_name);
        pat_parse_contents(pat, pat_name, split_contents, error_stack);
      }
      string_splitter_destroy(split_contents);
    }
    free(file_contents);
  }
  free(pat_filename);
  if (!error_stack_is_empty(error_stack)) {
    pat_destroy(pat);
    pat = NULL;
  }
  return pat;
}

void pat_write(const PATWeights *pat, const char *data_paths,
               const char *pat_name, ErrorStack *error_stack) {
  char *pat_filename = data_filepaths_get_writable_filename(
      data_paths, pat_name, DATA_FILEPATH_TYPE_PAT, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    free(pat_filename);
    return;
  }
  StringBuilder *sb = string_builder_create();
  string_builder_add_formatted_string(sb, "%s%d\n", PAT_MAGIC_PREFIX,
                                      PAT_VERSION);
  string_builder_add_formatted_string(sb, "%s%.6f\n", PAT_GAMMA_ROW_PREFIX,
                                      pat->combine_gamma);
  string_builder_add_formatted_string(sb, "%s%.6f\n",
                                      PAT_OWN_ASSET_DISCOUNT_ROW_PREFIX,
                                      pat->own_asset_discount);
  string_builder_add_formatted_string(sb, "%s%d\n",
                                      PAT_LEXICON_FLOATERS_ROW_PREFIX,
                                      pat->lexicon_floaters ? 1 : 0);
  string_builder_add_formatted_string(
      sb, "%s%d\n", PAT_SIGNED_THROUGH_ROW_PREFIX, pat->signed_through ? 1 : 0);
  string_builder_add_formatted_string(sb, "%s%d\n", PAT_FIT_RESIDUAL_ROW_PREFIX,
                                      pat->fit_residual);
  string_builder_add_formatted_string(sb, "%s%d\n",
                                      PAT_EXACT_CREATED_HOOKS_ROW_PREFIX,
                                      pat->exact_created_hooks ? 1 : 0);
  string_builder_add_formatted_string(sb, "%s%d\n",
                                      PAT_EXACT_FRESH_RUNS_ROW_PREFIX,
                                      pat->exact_fresh_runs ? 1 : 0);
  string_builder_add_formatted_string(sb, "%s%d\n",
                                      PAT_HOOK_SCORE_PROB_ROW_PREFIX,
                                      pat->hook_score_prob ? 1 : 0);
  string_builder_add_formatted_string(sb, "%s%d\n", PAT_HOOK_VALUE_ROW_PREFIX,
                                      pat->hook_value ? 1 : 0);
  string_builder_add_formatted_string(sb, "%s%d\n", PAT_FLEX_PROB_ROW_PREFIX,
                                      pat->flex_prob ? 1 : 0);
  string_builder_add_formatted_string(sb, "%s%d\n", PAT_RUN_THROUGH_ROW_PREFIX,
                                      pat->run_through ? 1 : 0);
  if (pat->utility_adjust > 0.0) {
    string_builder_add_formatted_string(
        sb, "%s%.6f\n", PAT_UTILITY_ADJUST_ROW_PREFIX, pat->utility_adjust);
  }
  string_builder_add_formatted_string(sb, "%s%.6f\n",
                                      PAT_STAGE_SCALE_EARLY_ROW_PREFIX,
                                      pat->stage_scale[PAT_STAGE_EARLY]);
  string_builder_add_formatted_string(sb, "%s%.6f\n",
                                      PAT_STAGE_SCALE_MID_ROW_PREFIX,
                                      pat->stage_scale[PAT_STAGE_MID]);
  string_builder_add_formatted_string(sb, "%s%.6f\n",
                                      PAT_STAGE_SCALE_LATE_ROW_PREFIX,
                                      pat->stage_scale[PAT_STAGE_LATE]);
  for (int tiles = 1; tiles <= RACK_SIZE; tiles++) {
    string_builder_add_formatted_string(sb, "%s%d,%d\n",
                                        PAT_OPENING_TILES_ROW_PREFIX, tiles,
                                        pat->opening_tiles[tiles]);
  }
  string_builder_add_formatted_string(
      sb, "%s%d\n", PAT_OPENING_EXCHANGE_ROW_PREFIX, pat->opening_exchange);
  string_builder_add_string(
      sb, "# trained PAT weights; units: milli-equity per feature "
          "unit; all values <= 0\n");
  char feature_name[64];
  for (int feature_index = 0; feature_index < PAT_NUM_FEATURES;
       feature_index++) {
    pat_feature_name(feature_index, feature_name, sizeof(feature_name));
    string_builder_add_formatted_string(sb, "%s,%d\n", feature_name,
                                        pat->weights[feature_index]);
  }
  write_string_to_file(pat_filename, "w", string_builder_peek(sb), error_stack);
  string_builder_destroy(sb);
  free(pat_filename);
}
