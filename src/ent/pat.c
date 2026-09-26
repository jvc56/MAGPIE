#include "pat.h"

#include "../def/pat_defs.h"
#include "../def/rack_defs.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "equity.h"
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

void pat_set_combine_gamma(PATWeights *pat, double combine_gamma) {
  pat->combine_gamma = combine_gamma;
}

double pat_get_own_asset_discount(const PATWeights *pat) {
  return pat->own_asset_discount;
}

void pat_set_own_asset_discount(PATWeights *pat, double own_asset_discount) {
  // Clamped here too, not only when parsing: pat_eval_move_penalty_bound
  // relies on the discount staying in [0, 1].
  if (own_asset_discount < 0.0) {
    own_asset_discount = 0.0;
  } else if (own_asset_discount > 1.0) {
    own_asset_discount = 1.0;
  }
  pat->own_asset_discount = own_asset_discount;
}

bool pat_get_lexicon_floaters(const PATWeights *pat) {
  return pat->lexicon_floaters;
}

bool pat_get_signed_through(const PATWeights *pat) {
  return pat->signed_through;
}

// See PATWeights.prepared.
void pat_require_prepared(const PATWeights *pat) {
  if (pat != NULL && !pat->prepared) {
    log_fatal("PAT '%s' is used without its lexicon tables prepared "
              "(pat_prepare_hook_flex)",
              pat->name);
  }
}

Equity pat_get_opening_tiles_adjustment(const PATWeights *pat, int tiles) {
  return pat->opening_tiles[tiles];
}

void pat_set_opening_tiles_adjustment(PATWeights *pat, int tiles,
                                      Equity adjustment) {
  if (tiles < 1 || tiles > RACK_SIZE || adjustment > 0) {
    log_fatal("PAT opening adjustment must be for 1..%d tiles and <= 0, got "
              "%d tiles, %d",
              RACK_SIZE, tiles, adjustment);
  }
  pat->opening_tiles[tiles] = adjustment;
}

Equity pat_get_opening_exchange_adjustment(const PATWeights *pat) {
  return pat->opening_exchange;
}

void pat_set_opening_exchange_adjustment(PATWeights *pat, Equity adjustment) {
  if (adjustment > 0) {
    log_fatal("PAT opening exchange adjustment must be <= 0, got %d",
              adjustment);
  }
  pat->opening_exchange = adjustment;
}

double pat_get_stage_scale(const PATWeights *pat, int stage) {
  return pat->stage_scale[stage];
}

void pat_set_stage_scale(PATWeights *pat, int stage, double scale) {
  if (!isfinite(scale) || scale < 0.0) {
    log_fatal("PAT stage scale must be finite and >= 0, got %f", scale);
  }
  pat->stage_scale[stage] = scale;
}

void pat_set_run_through(PATWeights *pat, bool run_through) {
  pat->run_through = run_through;
}

void pat_set_exact_created_hooks(PATWeights *pat, bool exact_created_hooks) {
  pat->exact_created_hooks = exact_created_hooks;
}

int pat_get_fit_residual(const PATWeights *pat) { return pat->fit_residual; }

void pat_set_fit_residual(PATWeights *pat, int fit_residual) {
  pat->fit_residual = fit_residual;
}

void pat_set_signed_through(PATWeights *pat, bool signed_through) {
  pat->signed_through = signed_through;
}

void pat_set_lexicon_floaters(PATWeights *pat, bool lexicon_floaters) {
  pat->lexicon_floaters = lexicon_floaters;
}

const char *pat_get_name(const PATWeights *pat) { return pat->name; }

Equity pat_get_weight(const PATWeights *pat, int feature_index) {
  return pat->weights[feature_index];
}

void pat_set_weight(PATWeights *pat, int feature_index, Equity weight) {
  if (weight > 0) {
    log_fatal("PAT weight for feature %d must be <= 0, got %d", feature_index,
              weight);
  }
  pat->weights[feature_index] = weight;
}

uint64_t pat_get_mutation_counter(const PATWeights *pat) {
  return pat->mutation_counter;
}

void pat_bump_mutation_counter(PATWeights *pat) { pat->mutation_counter++; }

void pat_feature_name(int feature_index, char *buf, size_t buf_size) {
  if (feature_index >= PAT_FEATURE_HOOK_START &&
      feature_index < PAT_FEATURE_FLOAT_FLEX_START) {
    (void)snprintf(buf, buf_size, "hook_d%d",
                   feature_index - PAT_FEATURE_HOOK_START + 1);
  } else if (feature_index < PAT_FEATURE_FLOAT_SCORE_START) {
    (void)snprintf(buf, buf_size, "float_flex_d%d",
                   feature_index - PAT_FEATURE_FLOAT_FLEX_START + 1);
  } else if (feature_index < PAT_FEATURE_FLOAT_THROUGH_SCORE_START) {
    (void)snprintf(buf, buf_size, "float_score_d%d",
                   feature_index - PAT_FEATURE_FLOAT_SCORE_START + 1);
  } else if (feature_index < PAT_FEATURE_FLOAT_THROUGH_COUNT_START) {
    (void)snprintf(buf, buf_size, "float_through_score_d%d",
                   feature_index - PAT_FEATURE_FLOAT_THROUGH_SCORE_START + 1);
  } else if (feature_index < PAT_FEATURE_HOOK_SCORE_START) {
    (void)snprintf(buf, buf_size, "float_through_count_d%d",
                   feature_index - PAT_FEATURE_FLOAT_THROUGH_COUNT_START + 1);
  } else if (feature_index < PAT_FEATURE_DWS_HOOK_START) {
    (void)snprintf(buf, buf_size, "hook_score_d%d",
                   feature_index - PAT_FEATURE_HOOK_SCORE_START + 1);
  } else if (feature_index < PAT_FEATURE_DWS_FLOAT_SCORE_START) {
    (void)snprintf(buf, buf_size, "dws_hook_d%d",
                   feature_index - PAT_FEATURE_DWS_HOOK_START + 1);
  } else if (feature_index < PAT_FEATURE_TLS_HOOK_START) {
    (void)snprintf(buf, buf_size, "dws_float_score_d%d",
                   feature_index - PAT_FEATURE_DWS_FLOAT_SCORE_START + 1);
  } else if (feature_index < PAT_FEATURE_TLS_FLOAT_SCORE_START) {
    (void)snprintf(buf, buf_size, "tls_hook_d%d",
                   feature_index - PAT_FEATURE_TLS_HOOK_START + 1);
  } else if (feature_index < PAT_FEATURE_DLS_HOOK_START) {
    (void)snprintf(buf, buf_size, "tls_float_score_d%d",
                   feature_index - PAT_FEATURE_TLS_FLOAT_SCORE_START + 1);
  } else if (feature_index < PAT_FEATURE_DLS_FLOAT_SCORE_START) {
    (void)snprintf(buf, buf_size, "dls_hook_d%d",
                   feature_index - PAT_FEATURE_DLS_HOOK_START + 1);
  } else if (feature_index < PAT_FEATURE_QWS_HOOK_START) {
    (void)snprintf(buf, buf_size, "dls_float_score_d%d",
                   feature_index - PAT_FEATURE_DLS_FLOAT_SCORE_START + 1);
  } else if (feature_index < PAT_FEATURE_QWS_FLOAT_SCORE_START) {
    (void)snprintf(buf, buf_size, "qws_hook_d%d",
                   feature_index - PAT_FEATURE_QWS_HOOK_START + 1);
  } else if (feature_index < PAT_FEATURE_QLS_HOOK_START) {
    (void)snprintf(buf, buf_size, "qws_float_score_d%d",
                   feature_index - PAT_FEATURE_QWS_FLOAT_SCORE_START + 1);
  } else if (feature_index < PAT_FEATURE_QLS_FLOAT_SCORE_START) {
    (void)snprintf(buf, buf_size, "qls_hook_d%d",
                   feature_index - PAT_FEATURE_QLS_HOOK_START + 1);
  } else if (feature_index < PAT_FEATURE_TT_FLOATER) {
    (void)snprintf(buf, buf_size, "qls_float_score_d%d",
                   feature_index - PAT_FEATURE_QLS_FLOAT_SCORE_START + 1);
  } else if (feature_index == PAT_FEATURE_TT_FLOATER) {
    (void)snprintf(buf, buf_size, "tt_floater");
  } else if (feature_index == PAT_FEATURE_TT_HOOK_ONLY) {
    (void)snprintf(buf, buf_size, "tt_hook_only");
  } else if (feature_index < PAT_NUM_FEATURES) {
    static const char *const tier_names[PAT_WINDOW_TIER_COUNT] = {"dd", "w6",
                                                                  "w9", "w12"};
    static const char *const kind_names[PAT_WINDOW_FEATURES_PER_TIER] = {
        "floater", "hook_only", "tiles_saved"};
    const int offset = feature_index - PAT_FEATURE_WINDOW_START;
    (void)snprintf(buf, buf_size, "%s_%s",
                   tier_names[offset / PAT_WINDOW_FEATURES_PER_TIER],
                   kind_names[offset % PAT_WINDOW_FEATURES_PER_TIER]);
  } else {
    log_fatal("invalid PAT feature index: %d", feature_index);
  }
}

PATWeights *pat_create_zeroed(const char *pat_name) {
  PATWeights *pat = calloc_or_die(1, sizeof(PATWeights));
  pat->name = string_duplicate(pat_name);
  pat->combine_gamma = PAT_DEFAULT_COMBINE_GAMMA;
  pat->own_asset_discount = PAT_DEFAULT_OWN_ASSET_DISCOUNT;
  pat->lexicon_floaters = PAT_DEFAULT_LEXICON_FLOATERS;
  pat->signed_through = PAT_DEFAULT_SIGNED_THROUGH;
  pat->fit_residual = PAT_FIT_DEFAULT;
  pat->exact_created_hooks = PAT_DEFAULT_EXACT_CREATED_HOOKS;
  pat->run_through = PAT_DEFAULT_RUN_THROUGH;
  pat->utility_adjust = 0.0;
  pat->utility_max_bag = 0;
  pat->utility_table = NULL;
  pat->utility_win_pct_name = NULL;
  pat->utility_suffix_max = NULL;
  for (int stage = 0; stage < PAT_STAGE_COUNT; stage++) {
    pat->stage_scale[stage] = PAT_DEFAULT_STAGE_SCALE;
  }
  for (int tiles = 0; tiles <= RACK_SIZE; tiles++) {
    pat->opening_tiles[tiles] = 0;
  }
  pat->opening_exchange = 0;
  pat->run_through_count = NULL;
  pat->run_through_score = NULL;
  pat->run_through_alphabet_size = 0;
  pat->version = PAT_VERSION;
  return pat;
}

void pat_destroy(PATWeights *pat) {
  if (!pat) {
    return;
  }
  free(pat->utility_table);
  free(pat->utility_suffix_max);
  free(pat->utility_win_pct_name);
  free(pat->name);
  free(pat->run_through_count);
  free(pat->run_through_score);
  free(pat);
}

// Indexed by pat_premium_class_t.
static const char *const pat_class_names[PAT_NUM_PREMIUM_CLASSES] = {
    "tws", "dws", "tls", "dls", "qws", "qls",
};

uint32_t pat_parse_classes_mask(const char *value, ErrorStack *error_stack) {
  if (strings_equal(value, "all")) {
    return PAT_CLASS_MASK_ALL;
  }
  if (strings_equal(value, "none")) {
    return 0;
  }
  StringSplitter *split = split_string(value, ',', true);
  const int num_items = string_splitter_get_number_of_items(split);
  uint32_t mask = 0;
  for (int item_index = 0; item_index < num_items; item_index++) {
    const char *item = string_splitter_get_item(split, item_index);
    if (strings_equal(item, "windows")) {
      mask |= PAT_CLASS_MASK_WINDOWS;
      continue;
    }
    bool matched = false;
    for (int premium_class = 0; premium_class < PAT_NUM_PREMIUM_CLASSES;
         premium_class++) {
      if (strings_equal(item, pat_class_names[premium_class])) {
        mask |= 1U << premium_class;
        matched = true;
        break;
      }
    }
    if (!matched) {
      error_stack_push(
          error_stack, ERROR_STATUS_PAT_INVALID_CLASSES_ARG,
          get_formatted_string("unrecognized PAT class: '%s'", item));
      string_splitter_destroy(split);
      return 0;
    }
  }
  string_splitter_destroy(split);
  return mask;
}

char *pat_classes_mask_to_string(uint32_t enabled_classes_mask) {
  if (enabled_classes_mask == PAT_CLASS_MASK_ALL) {
    return string_duplicate("all");
  }
  if (enabled_classes_mask == 0) {
    return string_duplicate("none");
  }
  StringBuilder *sb = string_builder_create();
  bool first = true;
  for (int premium_class = 0; premium_class < PAT_NUM_PREMIUM_CLASSES;
       premium_class++) {
    if (enabled_classes_mask & (1U << premium_class)) {
      if (!first) {
        string_builder_add_string(sb, ",");
      }
      string_builder_add_string(sb, pat_class_names[premium_class]);
      first = false;
    }
  }
  if (enabled_classes_mask & PAT_CLASS_MASK_WINDOWS) {
    if (!first) {
      string_builder_add_string(sb, ",");
    }
    string_builder_add_string(sb, "windows");
  }
  char *result = string_builder_dump(sb, NULL);
  string_builder_destroy(sb);
  return result;
}
