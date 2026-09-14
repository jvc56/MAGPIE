#include "pat.h"

#include "../def/board_defs.h"
#include "../def/cross_set_defs.h"
#include "../def/equity_defs.h"
#include "../def/game_history_defs.h"
#include "../def/kwg_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/pat_defs.h"
#include "../def/rack_defs.h"
#include "../util/fileproxy.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "board.h"
#include "bonus_square.h"
#include "data_filepaths.h"
#include "equity.h"
#include "kwg.h"
#include "letter_distribution.h"
#include "move.h"
#include "rack.h"
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct PATWeights {
  char *name;
  Equity weights[PAT_NUM_FEATURES];
  // Per-letter count of two-letter words containing the letter; the
  // flexibility approximation for hooks and floaters the evaluated move
  // itself creates (see pat_prepare_hook_flex).
  uint8_t hook_flex[MAX_ALPHABET_SIZE];
  // What a floater is worth to whoever plays through it, from the lexicon
  // alone. through_score[ml][len] is the mean total tile value the rest of
  // a len-letter word carries when ml sits at one end of it, and
  // through_count[ml][len] is how many such words there are, log-scaled so
  // a common letter does not swamp the fit. Both are indexed by the span a
  // word must cover to run from the floater to the triple.
  uint8_t through_score[MAX_ALPHABET_SIZE][PAT_MAX_THROUGH_LEN];
  uint8_t through_count[MAX_ALPHABET_SIZE][PAT_MAX_THROUGH_LEN];
  // The same statistics kept separately for the letter being the word's
  // first letter ([0]) or its last ([1]). A floater beyond the premium is
  // the LAST letter of the word that reaches the premium from it, a
  // floater before the premium the FIRST, and for letters like J
  // (thousands of words start with it, a handful end with it) or Y
  // (the reverse) the two differ enormously; the unsigned tables above
  // sum both ends into one figure. Used only when signed_through is set.
  uint8_t through_score_end[2][MAX_ALPHABET_SIZE][PAT_MAX_THROUGH_LEN];
  uint8_t through_count_end[2][MAX_ALPHABET_SIZE][PAT_MAX_THROUGH_LEN];
  // Run-keyed through tables: for a key of 1..PAT_RUN_THROUGH_MAX_KEY
  // letters, per word length and per end, the same two statistics over
  // the words that begin (end 0) or end (end 1) with that key. Built by
  // pat_prepare_hook_flex, sized by the alphabet; see pat_run_through_index.
  uint8_t *run_through_count;
  uint8_t *run_through_score;
  int run_through_alphabet_size;
  // How much a second route to danger counts once the worst one is already
  // counted. The opponent plays one move, so the threats a board offers do
  // not simply add: 0 charges only the worst scan unit, 1 charges every
  // unit in full (the original behaviour), and values between allow for a
  // rack that cannot use the worst route. See pat_combine_unit_penalties.
  double combine_gamma;
  // Fraction of a unit's penalty credited back when the move's own leave
  // holds a letter that could exploit that exact unit itself, whether or
  // not the move's placement touches it. 0 (the default for every file
  // that predates this, and the default pat_create_zeroed writes) is
  // exactly today's behavior: no credit, penalty unchanged. Clamped to
  // [0, 1] on read (see pat_parse_contents) so the adjusted penalty
  // p * (1 - discount) always stays in [p, 0]: it can only shrink a
  // penalty toward zero, never flip its sign, so no new shadow-pruning
  // bound is needed for it (see pat_eval_move_penalty).
  double own_asset_discount;
  // Whether a floater run's flexibility counts only the unseen tiles the
  // lexicon actually lets extend that run toward the premium (the run's
  // real extension set), or every unseen tile. false is what every file
  // before this row was trained under: pat_scan_unit read the extension
  // set from fields game_gen_cross_set never writes on an empty square
  // (an empty square's right_extension_set) or writes for the run on its
  // other side (its left_extension_set), so it always saw the trivial
  // all-letters set and float_flex_dN reduced to "floater runs at
  // distance N, times tiles unseen". The measured gap is large -- a
  // quarter of floater runs cannot be extended toward their premium at
  // all, and real flexibility totals about half of the trivial figure
  // (test/pat_overlap_pilot_test.c) -- but the shipped champion's
  // float_flex weights were fitted to the trivial figure, so the two
  // semantics are different models, selectable per file rather than
  // switched globally, until a refit under the real sets is validated
  // against the champion.
  bool lexicon_floaters;
  // Whether pat_scan_unit reads the end-specific through tables
  // (through_score_end / through_count_end) for a floater, by which side
  // of the premium it lies on, instead of the unsigned tables. false is
  // what every file before this row was trained under. Same reasoning as
  // lexicon_floaters for keeping it per file.
  bool signed_through;
  // Whether patgen's regression fits the hypergeometric-scaled channels
  // at all. They are deterministic rescalings of the raw hook/floater
  // channels, so ridge splits the fitted mass between the two under
  // near-collinearity, and under iterative training the split drifted
  // (a five-generation retrain with them lost to the champion, and a
  // reproduction of the champion's own recipe on this branch came out
  // below it). false drops their columns from the solve so a file trained
  // this way carries exactly the champion's feature set; evaluation is
  // unaffected either way (zero weights are zero).
  bool fit_scaled_channels;
  // Whether patgen builds training rows through the runtime overlay path
  // (pat_extract_move_features_combined) rather than by scanning the
  // post-move board. The two differ wherever the move creates a hook or
  // floater: the overlay approximates those with the hook_flex table,
  // the post-move scan measures them with real cross and extension sets
  // (mean absolute difference 0.66 equity over the v3 champion's top
  // candidates, see test_pat_train_runtime_parity). Training on the
  // overlay rows makes fitting and evaluation see the same measurement.
  bool train_overlay;
  // Whether patgen fits only the hook-score channels, as a residual on
  // top of every other weight exactly as loaded. A whole-vector refit
  // re-randomizes everything (identical-recipe seeds differ by 0.2-0.4
  // equity per disagreement on the move-choice harness), which would
  // swamp a small new channel's effect; the residual fit isolates it.
  bool fit_residual;
  // Whether hooks the evaluated move creates are scored exactly (see
  // pat_fresh_cross_set) rather than from the hook_flex approximation.
  // A runtime semantic, per file like the floater flags, and opt-in: a
  // file fitted on exact post-move rows but evaluated with the
  // approximation can still play better than one evaluated exactly, and
  // only whole-game play can say.
  bool exact_created_hooks;
  // Whether floater through channels use the run-keyed tables (see
  // pat_scan_unit). The per-tile sum they replace adds each tile's
  // single-letter log-count, i.e. the log of a product of unrelated
  // numbers: for AT four squares from a triple it claims ~169 units
  // against an actual 48.7 (67 words), for QI 121 against 0 (no word of
  // length six ends in QI), for NARCEIN 560 against 0. Opt-in, like the
  // other semantic flags, until a file using it is validated.
  bool run_through;
  // 1: only the hook-score channels are fitted; 2: the triple-word hook
  // flexibility channels too; 3: only the floater through channels (see
  // pat_regression_solve_into_weights).
  int fit_residual_mode;
  // Set by pat_prepare_hook_flex. The lexicon tables (hook_flex,
  // through_score, through_count) are part of the model: a file loaded
  // without them evaluates differently (it disagreed with the same
  // weights prepared on 12% of positions in one measurement), so the
  // evaluation and training entry points refuse an unprepared model
  // instead of quietly scoring with empty tables.
  bool prepared;
  uint64_t mutation_counter;
  // The version named on the file's header line (see PAT_VERSION).
  int version;
};

double pat_get_combine_gamma(const PATWeights *pat) {
  return pat->combine_gamma;
}

void pat_set_combine_gamma(PATWeights *pat, double combine_gamma) {
  pat->combine_gamma = combine_gamma;
}

double pat_get_own_asset_discount(const PATWeights *pat) {
  return pat->own_asset_discount;
}

void pat_set_own_asset_discount(PATWeights *pat, double own_asset_discount) {
  // Clamped here, not just at the file-parsing boundary: a search over
  // candidate discounts (e.g. a 1D fit) mutates a PATWeights directly, and
  // the bound argument in pat_eval_move_penalty_bound depends on this
  // invariant holding for every value this object could ever carry.
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

bool pat_get_prepared(const PATWeights *pat) { return pat->prepared; }

// See PATWeights.prepared.
static void pat_require_prepared(const PATWeights *pat) {
  if (pat != NULL && !pat->prepared) {
    log_fatal("PAT '%s' is used without its lexicon tables prepared "
              "(pat_prepare_hook_flex)",
              pat->name);
  }
}

bool pat_get_fit_scaled_channels(const PATWeights *pat) {
  return pat->fit_scaled_channels;
}

bool pat_get_train_overlay(const PATWeights *pat) { return pat->train_overlay; }

void pat_set_train_overlay(PATWeights *pat, bool train_overlay) {
  pat->train_overlay = train_overlay;
}

bool pat_get_run_through(const PATWeights *pat) { return pat->run_through; }

void pat_set_run_through(PATWeights *pat, bool run_through) {
  pat->run_through = run_through;
}

// Row index of a key in the run-keyed tables: keys of each length are
// laid out consecutively (all 1-letter keys, then 2-letter, ...), each
// block indexed base alphabet_size in word order; within a row, word
// length then end.
static int pat_run_through_index(const PATWeights *pat, int word_end,
                                 const MachineLetter *key, int key_len,
                                 int word_length) {
  const int n = pat->run_through_alphabet_size;
  int block_start = 0;
  int block = 1;
  for (int k = 1; k < key_len; k++) {
    block *= n;
    block_start += block;
  }
  // block_start now holds n + n^2 + ... + n^(key_len-1); add the key's
  // base-n value.
  int within = 0;
  for (int k = 0; k < key_len; k++) {
    within = within * n + key[k];
  }
  return ((block_start + within) * PAT_MAX_THROUGH_LEN + word_length) * 2 +
         word_end;
}

static int pat_run_through_rows(int alphabet_size) {
  int rows = 0;
  int block = 1;
  for (int k = 1; k <= PAT_RUN_THROUGH_MAX_KEY; k++) {
    block *= alphabet_size;
    rows += block;
  }
  return rows;
}

int pat_get_run_through_count(const PATWeights *pat, int word_end,
                              const MachineLetter *key, int key_len,
                              int word_length) {
  if (pat->run_through_count == NULL || key_len < 1 ||
      key_len > PAT_RUN_THROUGH_MAX_KEY || word_length >= PAT_MAX_THROUGH_LEN) {
    return 0;
  }
  return pat->run_through_count[pat_run_through_index(pat, word_end, key,
                                                      key_len, word_length)];
}

int pat_get_run_through_score(const PATWeights *pat, int word_end,
                              const MachineLetter *key, int key_len,
                              int word_length) {
  if (pat->run_through_score == NULL || key_len < 1 ||
      key_len > PAT_RUN_THROUGH_MAX_KEY || word_length >= PAT_MAX_THROUGH_LEN) {
    return 0;
  }
  return pat->run_through_score[pat_run_through_index(pat, word_end, key,
                                                      key_len, word_length)];
}

bool pat_get_exact_created_hooks(const PATWeights *pat) {
  return pat->exact_created_hooks;
}

void pat_set_exact_created_hooks(PATWeights *pat, bool exact_created_hooks) {
  pat->exact_created_hooks = exact_created_hooks;
}

bool pat_get_fit_residual(const PATWeights *pat) { return pat->fit_residual; }

int pat_get_fit_residual_mode(const PATWeights *pat) {
  return pat->fit_residual_mode;
}

void pat_set_fit_residual(PATWeights *pat, bool fit_residual) {
  pat->fit_residual = fit_residual;
  pat->fit_residual_mode = fit_residual ? 1 : 0;
}

void pat_set_fit_scaled_channels(PATWeights *pat, bool fit_scaled_channels) {
  pat->fit_scaled_channels = fit_scaled_channels;
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
    snprintf(buf, buf_size, "hook_d%d",
             feature_index - PAT_FEATURE_HOOK_START + 1);
  } else if (feature_index < PAT_FEATURE_FLOAT_SCORE_START) {
    snprintf(buf, buf_size, "float_flex_d%d",
             feature_index - PAT_FEATURE_FLOAT_FLEX_START + 1);
  } else if (feature_index < PAT_FEATURE_FLOAT_THROUGH_SCORE_START) {
    snprintf(buf, buf_size, "float_score_d%d",
             feature_index - PAT_FEATURE_FLOAT_SCORE_START + 1);
  } else if (feature_index < PAT_FEATURE_FLOAT_THROUGH_COUNT_START) {
    snprintf(buf, buf_size, "float_through_score_d%d",
             feature_index - PAT_FEATURE_FLOAT_THROUGH_SCORE_START + 1);
  } else if (feature_index < PAT_FEATURE_HOOK_SCALED_START) {
    snprintf(buf, buf_size, "float_through_count_d%d",
             feature_index - PAT_FEATURE_FLOAT_THROUGH_COUNT_START + 1);
  } else if (feature_index < PAT_FEATURE_FLOAT_FLEX_SCALED_START) {
    snprintf(buf, buf_size, "hook_scaled_d%d",
             feature_index - PAT_FEATURE_HOOK_SCALED_START + 1);
  } else if (feature_index < PAT_FEATURE_HOOK_SCORE_START) {
    snprintf(buf, buf_size, "float_flex_scaled_d%d",
             feature_index - PAT_FEATURE_FLOAT_FLEX_SCALED_START + 1);
  } else if (feature_index < PAT_FEATURE_DWS_HOOK_START) {
    snprintf(buf, buf_size, "hook_score_d%d",
             feature_index - PAT_FEATURE_HOOK_SCORE_START + 1);
  } else if (feature_index < PAT_FEATURE_DWS_FLOAT_SCORE_START) {
    snprintf(buf, buf_size, "dws_hook_d%d",
             feature_index - PAT_FEATURE_DWS_HOOK_START + 1);
  } else if (feature_index < PAT_FEATURE_TLS_HOOK_START) {
    snprintf(buf, buf_size, "dws_float_score_d%d",
             feature_index - PAT_FEATURE_DWS_FLOAT_SCORE_START + 1);
  } else if (feature_index < PAT_FEATURE_TLS_FLOAT_SCORE_START) {
    snprintf(buf, buf_size, "tls_hook_d%d",
             feature_index - PAT_FEATURE_TLS_HOOK_START + 1);
  } else if (feature_index < PAT_FEATURE_DLS_HOOK_START) {
    snprintf(buf, buf_size, "tls_float_score_d%d",
             feature_index - PAT_FEATURE_TLS_FLOAT_SCORE_START + 1);
  } else if (feature_index < PAT_FEATURE_DLS_FLOAT_SCORE_START) {
    snprintf(buf, buf_size, "dls_hook_d%d",
             feature_index - PAT_FEATURE_DLS_HOOK_START + 1);
  } else if (feature_index < PAT_FEATURE_QWS_HOOK_START) {
    snprintf(buf, buf_size, "dls_float_score_d%d",
             feature_index - PAT_FEATURE_DLS_FLOAT_SCORE_START + 1);
  } else if (feature_index < PAT_FEATURE_QWS_FLOAT_SCORE_START) {
    snprintf(buf, buf_size, "qws_hook_d%d",
             feature_index - PAT_FEATURE_QWS_HOOK_START + 1);
  } else if (feature_index < PAT_FEATURE_QLS_HOOK_START) {
    snprintf(buf, buf_size, "qws_float_score_d%d",
             feature_index - PAT_FEATURE_QWS_FLOAT_SCORE_START + 1);
  } else if (feature_index < PAT_FEATURE_QLS_FLOAT_SCORE_START) {
    snprintf(buf, buf_size, "qls_hook_d%d",
             feature_index - PAT_FEATURE_QLS_HOOK_START + 1);
  } else if (feature_index < PAT_FEATURE_TT_FLOATER) {
    snprintf(buf, buf_size, "qls_float_score_d%d",
             feature_index - PAT_FEATURE_QLS_FLOAT_SCORE_START + 1);
  } else if (feature_index == PAT_FEATURE_TT_FLOATER) {
    snprintf(buf, buf_size, "tt_floater");
  } else if (feature_index == PAT_FEATURE_TT_HOOK_ONLY) {
    snprintf(buf, buf_size, "tt_hook_only");
  } else if (feature_index < PAT_FEATURE_LM_SPAN_START) {
    static const char *const tier_names[PAT_WINDOW_TIER_COUNT] = {"dd", "w6",
                                                                  "w9", "w12"};
    static const char *const kind_names[PAT_WINDOW_FEATURES_PER_TIER] = {
        "floater", "hook_only", "tiles_saved"};
    const int offset = feature_index - PAT_FEATURE_WINDOW_START;
    snprintf(buf, buf_size, "%s_%s",
             tier_names[offset / PAT_WINDOW_FEATURES_PER_TIER],
             kind_names[offset % PAT_WINDOW_FEATURES_PER_TIER]);
  } else if (feature_index < PAT_FEATURE_LM_EXT_START) {
    snprintf(buf, buf_size, "lm_span_d%d",
             feature_index - PAT_FEATURE_LM_SPAN_START + 1);
  } else if (feature_index < PAT_FEATURE_DWS_LM_SPAN_START) {
    snprintf(buf, buf_size, "lm_ext_d%d",
             feature_index - PAT_FEATURE_LM_EXT_START + 1);
  } else if (feature_index < PAT_FEATURE_DWS_LM_EXT_START) {
    snprintf(buf, buf_size, "dws_lm_span_d%d",
             feature_index - PAT_FEATURE_DWS_LM_SPAN_START + 1);
  } else if (feature_index < PAT_NUM_FEATURES) {
    snprintf(buf, buf_size, "dws_lm_ext_d%d",
             feature_index - PAT_FEATURE_DWS_LM_EXT_START + 1);
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
  pat->fit_scaled_channels = PAT_DEFAULT_FIT_SCALED;
  pat->train_overlay = PAT_DEFAULT_TRAIN_OVERLAY;
  pat->fit_residual = PAT_DEFAULT_FIT_RESIDUAL;
  pat->fit_residual_mode = 0;
  pat->exact_created_hooks = PAT_DEFAULT_EXACT_CREATED_HOOKS;
  pat->run_through = PAT_DEFAULT_RUN_THROUGH;
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
  free(pat->name);
  free(pat->run_through_count);
  free(pat->run_through_score);
  free(pat);
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
    // A version below 3 has no hypergeometric-scaled rows: they were added
    // in version 3. Same skip, same reasoning.
    if (version < 3 && feature_index >= PAT_FEATURE_HOOK_SCALED_START &&
        feature_index < PAT_FEATURE_HOOK_SCORE_START) {
      feature_index = PAT_FEATURE_HOOK_SCORE_START;
    }
    // A version below 4 has no hook-score rows: added in version 4.
    if (version < 4 && feature_index >= PAT_FEATURE_HOOK_SCORE_START &&
        feature_index < PAT_FEATURE_DWS_HOOK_START) {
      feature_index = PAT_FEATURE_DWS_HOOK_START;
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
    if (has_prefix(PAT_FIT_SCALED_ROW_PREFIX, line)) {
      const int flag =
          string_to_int(line + strlen(PAT_FIT_SCALED_ROW_PREFIX), error_stack);
      if (!error_stack_is_empty(error_stack) || (flag != 0 && flag != 1)) {
        error_stack_push(
            error_stack, ERROR_STATUS_PAT_INVALID_ROW,
            get_formatted_string("PAT file '%s' line %d has a fit_scaled "
                                 "flag other than 0 or 1: '%s'",
                                 pat_name, line_index + 1, line));
        return;
      }
      pat->fit_scaled_channels = (flag == 1);
      continue;
    }
    if (has_prefix(PAT_TRAIN_OVERLAY_ROW_PREFIX, line)) {
      const int flag = string_to_int(
          line + strlen(PAT_TRAIN_OVERLAY_ROW_PREFIX), error_stack);
      if (!error_stack_is_empty(error_stack) || (flag != 0 && flag != 1)) {
        error_stack_push(
            error_stack, ERROR_STATUS_PAT_INVALID_ROW,
            get_formatted_string("PAT file '%s' line %d has a train_overlay "
                                 "flag other than 0 or 1: '%s'",
                                 pat_name, line_index + 1, line));
        return;
      }
      pat->train_overlay = (flag == 1);
      continue;
    }
    if (has_prefix(PAT_FIT_RESIDUAL_ROW_PREFIX, line)) {
      const int flag = string_to_int(line + strlen(PAT_FIT_RESIDUAL_ROW_PREFIX),
                                     error_stack);
      if (!error_stack_is_empty(error_stack) || flag < 0 || flag > 4) {
        error_stack_push(
            error_stack, ERROR_STATUS_PAT_INVALID_ROW,
            get_formatted_string("PAT file '%s' line %d has a fit_residual "
                                 "value other than 0 to 4: '%s'",
                                 pat_name, line_index + 1, line));
        return;
      }
      pat->fit_residual = (flag != 0);
      pat->fit_residual_mode = flag;
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
  // A version below 5 has no premium-combination rows: added in version
  // 5 at the end, so such a file simply stops before them.
  if (version < 5 && feature_index == PAT_FEATURE_LM_SPAN_START) {
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
  string_builder_add_formatted_string(sb, "%s%d\n", PAT_FIT_SCALED_ROW_PREFIX,
                                      pat->fit_scaled_channels ? 1 : 0);
  string_builder_add_formatted_string(
      sb, "%s%d\n", PAT_TRAIN_OVERLAY_ROW_PREFIX, pat->train_overlay ? 1 : 0);
  string_builder_add_formatted_string(sb, "%s%d\n", PAT_FIT_RESIDUAL_ROW_PREFIX,
                                      pat->fit_residual_mode);
  string_builder_add_formatted_string(sb, "%s%d\n",
                                      PAT_EXACT_CREATED_HOOKS_ROW_PREFIX,
                                      pat->exact_created_hooks ? 1 : 0);
  string_builder_add_formatted_string(sb, "%s%d\n", PAT_RUN_THROUGH_ROW_PREFIX,
                                      pat->run_through ? 1 : 0);
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

// Accumulators for the through table: for each (end letter, word length),
// the number of words and the summed tile value of everything in them but
// that end letter.
typedef struct PATThroughStats {
  // [0]: the letter is the word's first; [1]: its last.
  double count[2][MAX_ALPHABET_SIZE][PAT_MAX_THROUGH_LEN];
  double score_sum[2][MAX_ALPHABET_SIZE][PAT_MAX_THROUGH_LEN];
  // The run-keyed accumulators, indexed like the tables they become
  // (pat_run_through_index): the count of words with the key at that end
  // and the summed score of the rest of each such word.
  double *run_count;
  double *run_score_sum;
  const PATWeights *pat;
} PATThroughStats;

// Walks every word in the lexicon, crediting each to the letters at its two
// ends. A floater reaches the triple by being one end of the word that
// covers the span between them, so those are the only positions that
// matter; the value carried is what the REST of the word scores, which is
// what the opponent lays down to get there.
static void pat_walk_words(const KWG *kwg, const LetterDistribution *ld,
                           uint32_t node_index, MachineLetter *word, int length,
                           PATThroughStats *stats) {
  if (node_index == 0) {
    return;
  }
  for (uint32_t index = node_index;; index++) {
    const uint32_t node = kwg_node(kwg, index);
    const MachineLetter machine_letter = (MachineLetter)kwg_node_tile(node);
    word[length] = machine_letter;
    const int word_length = length + 1;
    if (kwg_node_accepts(node) && word_length >= MINIMUM_WORD_LENGTH &&
        word_length < PAT_MAX_THROUGH_LEN) {
      int total_score = 0;
      for (int letter_index = 0; letter_index < word_length; letter_index++) {
        total_score += equity_to_int(ld_get_score(ld, word[letter_index]));
      }
      const MachineLetter first = word[0];
      const MachineLetter last = word[word_length - 1];
      // Every prefix and suffix of the word up to the key depth, with what
      // the rest of the word scores.
      for (int key_len = 1;
           key_len <= PAT_RUN_THROUGH_MAX_KEY && key_len <= word_length;
           key_len++) {
        int prefix_score = 0;
        int suffix_score = 0;
        for (int k = 0; k < key_len; k++) {
          prefix_score += equity_to_int(ld_get_score(ld, word[k]));
          suffix_score +=
              equity_to_int(ld_get_score(ld, word[word_length - key_len + k]));
        }
        const int prefix_index =
            pat_run_through_index(stats->pat, 0, word, key_len, word_length);
        const int suffix_index = pat_run_through_index(
            stats->pat, 1, word + word_length - key_len, key_len, word_length);
        stats->run_count[prefix_index] += 1.0;
        stats->run_score_sum[prefix_index] += total_score - prefix_score;
        stats->run_count[suffix_index] += 1.0;
        stats->run_score_sum[suffix_index] += total_score - suffix_score;
      }
      stats->count[0][first][word_length] += 1.0;
      stats->score_sum[0][first][word_length] +=
          total_score - equity_to_int(ld_get_score(ld, first));
      // The last letter is a distinct position even when it repeats the
      // first, since accepted words are at least two letters long.
      stats->count[1][last][word_length] += 1.0;
      stats->score_sum[1][last][word_length] +=
          total_score - equity_to_int(ld_get_score(ld, last));
    }
    if (word_length < PAT_MAX_THROUGH_LEN - 1) {
      pat_walk_words(kwg, ld, kwg_node_arc_index(node), word, word_length,
                     stats);
    }
    if (kwg_node_is_end(node)) {
      break;
    }
  }
}

void pat_prepare_hook_flex(PATWeights *pat, const KWG *kwg,
                           const LetterDistribution *ld) {
  pat->prepared = true;
  memset(pat->hook_flex, 0, sizeof(pat->hook_flex));
  memset(pat->through_score, 0, sizeof(pat->through_score));
  memset(pat->through_count, 0, sizeof(pat->through_count));
  memset(pat->through_score_end, 0, sizeof(pat->through_score_end));
  memset(pat->through_count_end, 0, sizeof(pat->through_count_end));
  if (!kwg) {
    return;
  }
  int counts[MAX_ALPHABET_SIZE] = {0};
  const uint32_t dawg_root = kwg_get_dawg_root_node_index(kwg);
  const int ld_size = ld_get_size(ld);
  for (int first_ml = 1; first_ml < ld_size; first_ml++) {
    const uint32_t node_index =
        kwg_get_next_node_index(kwg, dawg_root, (MachineLetter)first_ml);
    if (node_index == 0) {
      continue;
    }
    uint64_t extension_set = 0;
    const uint64_t accepted_set =
        kwg_get_letter_sets(kwg, node_index, &extension_set) & ~(uint64_t)1;
    for (int second_ml = 1; second_ml < ld_size; second_ml++) {
      if (accepted_set & ((uint64_t)1 << second_ml)) {
        counts[first_ml]++;
        counts[second_ml]++;
      }
    }
  }
  for (int ml = 0; ml < MAX_ALPHABET_SIZE; ml++) {
    if (counts[ml] > UINT8_MAX) {
      counts[ml] = UINT8_MAX;
    }
    pat->hook_flex[ml] = (uint8_t)counts[ml];
  }

  PATThroughStats *stats = calloc_or_die(1, sizeof(PATThroughStats));
  pat->run_through_alphabet_size = ld_size;
  const size_t run_cells =
      (size_t)pat_run_through_rows(ld_size) * PAT_MAX_THROUGH_LEN * 2;
  stats->run_count = calloc_or_die(run_cells, sizeof(double));
  stats->run_score_sum = calloc_or_die(run_cells, sizeof(double));
  stats->pat = pat;
  MachineLetter word[PAT_MAX_THROUGH_LEN];
  pat_walk_words(kwg, ld, dawg_root, word, 0, stats);
  free(pat->run_through_count);
  free(pat->run_through_score);
  pat->run_through_count = calloc_or_die(run_cells, sizeof(uint8_t));
  pat->run_through_score = calloc_or_die(run_cells, sizeof(uint8_t));
  for (size_t cell = 0; cell < run_cells; cell++) {
    const double word_count = stats->run_count[cell];
    if (word_count <= 0.0) {
      continue;
    }
    double mean_score = stats->run_score_sum[cell] / word_count;
    if (mean_score > UINT8_MAX) {
      mean_score = UINT8_MAX;
    }
    double scaled = 8.0 * log2(1.0 + word_count);
    if (scaled > UINT8_MAX) {
      scaled = UINT8_MAX;
    }
    pat->run_through_score[cell] = (uint8_t)(mean_score + 0.5);
    pat->run_through_count[cell] = (uint8_t)(scaled + 0.5);
  }
  free(stats->run_count);
  free(stats->run_score_sum);
  for (int ml = 0; ml < MAX_ALPHABET_SIZE; ml++) {
    for (int len = 0; len < PAT_MAX_THROUGH_LEN; len++) {
      // The unsigned tables are exactly what they always were: both ends
      // pooled into one count and one mean.
      for (int word_end = 0; word_end <= 2; word_end++) {
        const double word_count =
            (word_end < 2)
                ? stats->count[word_end][ml][len]
                : stats->count[0][ml][len] + stats->count[1][ml][len];
        if (word_count <= 0.0) {
          continue;
        }
        const double score_sum =
            (word_end < 2)
                ? stats->score_sum[word_end][ml][len]
                : stats->score_sum[0][ml][len] + stats->score_sum[1][ml][len];
        double mean_score = score_sum / word_count;
        if (mean_score > UINT8_MAX) {
          mean_score = UINT8_MAX;
        }
        // Log scale: the useful distinction is between a letter that
        // reaches nothing, a few words, and thousands, not between 900
        // and 1000.
        double scaled = 8.0 * log2(1.0 + word_count);
        if (scaled > UINT8_MAX) {
          scaled = UINT8_MAX;
        }
        if (word_end < 2) {
          pat->through_score_end[word_end][ml][len] =
              (uint8_t)(mean_score + 0.5);
          pat->through_count_end[word_end][ml][len] = (uint8_t)(scaled + 0.5);
        } else {
          pat->through_score[ml][len] = (uint8_t)(mean_score + 0.5);
          pat->through_count[ml][len] = (uint8_t)(scaled + 0.5);
        }
      }
    }
  }
  free(stats);
}

int pat_get_through_score_end(const PATWeights *pat, int word_end,
                              MachineLetter ml, int span) {
  return (span < PAT_MAX_THROUGH_LEN)
             ? pat->through_score_end[word_end][ml][span]
             : 0;
}

int pat_get_through_count_end(const PATWeights *pat, int word_end,
                              MachineLetter ml, int span) {
  return (span < PAT_MAX_THROUGH_LEN)
             ? pat->through_count_end[word_end][ml][span]
             : 0;
}

int pat_get_through_score(const PATWeights *pat, MachineLetter ml, int span) {
  return (span < PAT_MAX_THROUGH_LEN) ? pat->through_score[ml][span] : 0;
}

int pat_get_through_count(const PATWeights *pat, MachineLetter ml, int span) {
  return (span < PAT_MAX_THROUGH_LEN) ? pat->through_count[ml][span] : 0;
}

int pat_get_hook_flex(const PATWeights *pat, MachineLetter ml) {
  return pat->hook_flex[ml];
}

static inline int pat_ctz(uint64_t bits) {
#if defined(__has_builtin) && __has_builtin(__builtin_ctzll)
  return __builtin_ctzll(bits);
#else
  int count = 0;
  while (!(bits & 1)) {
    bits >>= 1;
    count++;
  }
  return count;
#endif
}

// The blank marker occupies bit 0 of cross and extension sets; flexibility
// counts real letters only.
// The flexibility of a hook or extension point: how many tiles the opponent
// could still hold that fit it. Counting unseen tiles rather than the
// letters the set admits makes a hook needing a J the near-nothing it
// usually is, and makes a hook only the evaluating player can fill (its
// letters all sitting on their own rack) score as no threat at all.
static inline int pat_set_flex(const uint8_t *unseen_counts,
                               uint64_t letter_set) {
  int flex = 0;
  uint64_t remaining = letter_set & ~(uint64_t)1;
  while (remaining) {
    const int machine_letter = pat_ctz(remaining);
    remaining &= remaining - 1;
    flex += unseen_counts[machine_letter];
  }
  return flex;
}

// The hypergeometric expectation of how many of the unseen pool's copies
// of a given letter are already in the opponent's rack right now, as a
// fraction: opponent_rack_size / total_unseen. Raw flex implicitly uses a
// fraction of 1 (every unseen copy is treated as if it were already in
// their hand), which is only right when the bag is empty; early on, most
// of the unseen pool is still sitting in the bag, unreachable for many
// turns, so raw flex overstates immediate risk. total_unseen of 0 means
// nothing is left to fear either way, and dividing by it would be
// meaningless, so that case scales to 0.
static inline double pat_hyper_scale(const uint8_t *unseen_counts,
                                     int opponent_rack_size) {
  int total_unseen = 0;
  for (int letter = 0; letter < MAX_ALPHABET_SIZE; letter++) {
    total_unseen += unseen_counts[letter];
  }
  if (total_unseen <= 0) {
    return 0.0;
  }
  return (double)opponent_rack_size / (double)total_unseen;
}

// pat_set_flex scaled by the hypergeometric fraction (see pat_hyper_scale),
// rounded to the nearest integer so it stays comparable to the raw count
// feature it sits beside.
static inline int pat_set_flex_scaled(const uint8_t *unseen_counts,
                                      uint64_t letter_set, double hyper_scale) {
  return (int)lround(pat_set_flex(unseen_counts, letter_set) * hyper_scale);
}

// Fills unseen_counts (MAX_ALPHABET_SIZE entries) with the tiles that are
// neither on the board nor on the evaluating player's rack, which is
// exactly the pool the opponent draws from plus what they already hold.
// Blanks on the board are counted against the blank.
static void pat_compute_unseen_counts(const Square *lanes,
                                      const LetterDistribution *ld,
                                      const Rack *player_rack,
                                      uint8_t *unseen_counts) {
  memset(unseen_counts, 0, sizeof(uint8_t) * MAX_ALPHABET_SIZE);
  const int ld_size = ld_get_size(ld);
  for (int machine_letter = 0; machine_letter < ld_size; machine_letter++) {
    unseen_counts[machine_letter] = (uint8_t)ld_get_dist(ld, machine_letter);
  }
  for (int row = 0; row < BOARD_DIM; row++) {
    const Square *lane =
        board_get_row_cache(lanes, row, BOARD_HORIZONTAL_DIRECTION);
    for (int col = 0; col < BOARD_DIM; col++) {
      const MachineLetter letter = square_get_letter(&lane[col]);
      if (letter == ALPHABET_EMPTY_SQUARE_MARKER) {
        continue;
      }
      const MachineLetter counted =
          get_is_blanked(letter) ? BLANK_MACHINE_LETTER : letter;
      if (counted < MAX_ALPHABET_SIZE && unseen_counts[counted] > 0) {
        unseen_counts[counted]--;
      }
    }
  }
  if (player_rack == NULL) {
    return;
  }
  for (int machine_letter = 0; machine_letter < ld_size; machine_letter++) {
    const int held = rack_get_letter(player_rack, machine_letter);
    unseen_counts[machine_letter] =
        (uint8_t)((unseen_counts[machine_letter] > held)
                      ? unseen_counts[machine_letter] - held
                      : 0);
  }
}

// Overlay describing the candidate move's fresh tiles on top of the
// pre-move board. hook_flex approximates the flexibility of hooks and
// floaters the move itself creates, whose real cross and extension sets do
// not exist yet.
typedef struct PATMoveOverlay {
  const Move *move;
  int row_start;
  int col_start;
  int row_end;
  int col_end;
  bool vertical;
  const uint8_t *hook_flex;
  // Non-NULL when hooks the move creates are to be resolved exactly (see
  // pat_fresh_cross_set); the lane cache goes with it.
  const KWG *kwg;
  const Square *lanes;
} PATMoveOverlay;

static inline bool pat_move_covers(const PATMoveOverlay *overlay, int row,
                                   int col, MachineLetter *fresh_letter_out) {
  if (!overlay) {
    return false;
  }
  if (row < overlay->row_start || row > overlay->row_end ||
      col < overlay->col_start || col > overlay->col_end) {
    return false;
  }
  const int tile_index =
      overlay->vertical ? row - overlay->row_start : col - overlay->col_start;
  const MachineLetter tile = move_get_tile(overlay->move, tile_index);
  if (tile == PLAYED_THROUGH_MARKER) {
    return false;
  }
  *fresh_letter_out = tile;
  return true;
}

// The cross set of the empty square (row, col) on a dir lane after the
// move, and the cross score to go with it: the perpendicular pattern
// through the square, existing and fresh tiles alike, resolved on the
// GADDAG the way game_gen_cross_set does for a played board. Tiles before
// the square are collected outward, which is the reversed prefix the
// GADDAG wants; a prefix-only pattern takes the separator and reads the
// accepted set (back hooks); a suffix-only pattern walks the reversed
// suffix and reads the accepted set (front hooks); with both, each letter
// arc after the separator is checked through the suffix. 0 means no
// letter fits (a dead square).
static uint64_t pat_fresh_cross_set(const KWG *kwg,
                                    const LetterDistribution *ld,
                                    const Square *lanes, int dir, int row,
                                    int col, const PATMoveOverlay *overlay,
                                    int *cross_score_out) {
  const int perp_dir = (dir == BOARD_HORIZONTAL_DIRECTION)
                           ? BOARD_VERTICAL_DIRECTION
                           : BOARD_HORIZONTAL_DIRECTION;
  const int perp_lane_index =
      (perp_dir == BOARD_HORIZONTAL_DIRECTION) ? row : col;
  const int perp_idx = (perp_dir == BOARD_HORIZONTAL_DIRECTION) ? col : row;
  const Square *perp_lane =
      board_get_row_cache(lanes, perp_lane_index, perp_dir);
  MachineLetter before[BOARD_DIM];
  MachineLetter after[BOARD_DIM];
  int num_before = 0;
  int num_after = 0;
  int cross_score = 0;
  for (int side = -1; side <= 1; side += 2) {
    int idx = perp_idx + side;
    while (idx >= 0 && idx < BOARD_DIM &&
           !square_get_is_brick(&perp_lane[idx])) {
      const int r =
          (perp_dir == BOARD_HORIZONTAL_DIRECTION) ? perp_lane_index : idx;
      const int c =
          (perp_dir == BOARD_HORIZONTAL_DIRECTION) ? idx : perp_lane_index;
      MachineLetter letter;
      if (!pat_move_covers(overlay, r, c, &letter)) {
        letter = square_get_letter(&perp_lane[idx]);
        if (letter == ALPHABET_EMPTY_SQUARE_MARKER) {
          break;
        }
      }
      if (!get_is_blanked(letter)) {
        cross_score += equity_to_int(ld_get_score(ld, letter));
      }
      const MachineLetter unblanked = get_unblanked_machine_letter(letter);
      if (side < 0) {
        before[num_before++] = unblanked;
      } else {
        after[num_after++] = unblanked;
      }
      idx += side;
    }
  }
  *cross_score_out = cross_score;
  if (num_before == 0 && num_after == 0) {
    return TRIVIAL_CROSS_SET;
  }
  const uint32_t root = kwg_get_root_node_index(kwg);
  uint64_t extension_set = 0;
  if (num_before == 0) {
    uint32_t node = root;
    for (int k = num_after - 1; k >= 0; k--) {
      node = kwg_get_next_node_index(kwg, node, after[k]);
      if (node == 0) {
        return 0;
      }
    }
    return kwg_get_letter_sets(kwg, node, &extension_set) & ~(uint64_t)1;
  }
  uint32_t node = root;
  for (int k = 0; k < num_before; k++) {
    node = kwg_get_next_node_index(kwg, node, before[k]);
    if (node == 0) {
      return 0;
    }
  }
  const uint32_t separated =
      kwg_get_next_node_index(kwg, node, SEPARATION_MACHINE_LETTER);
  if (separated == 0) {
    return 0;
  }
  if (num_after == 0) {
    return kwg_get_letter_sets(kwg, separated, &extension_set) & ~(uint64_t)1;
  }
  uint64_t cross_set = 0;
  for (uint32_t i = separated;; i++) {
    const uint32_t arc = kwg_node(kwg, i);
    const MachineLetter ml = (MachineLetter)kwg_node_tile(arc);
    if (ml != SEPARATION_MACHINE_LETTER) {
      uint32_t cur = kwg_node_arc_index_prefetch(arc, kwg);
      bool ok = cur != 0;
      for (int k = 0; ok && k < num_after - 1; k++) {
        cur = kwg_get_next_node_index(kwg, cur, after[k]);
        ok = cur != 0;
      }
      if (ok && kwg_in_letter_set(kwg, after[num_after - 1], cur)) {
        cross_set |= (uint64_t)1 << ml;
      }
    }
    if (kwg_node_is_end(arc)) {
      break;
    }
  }
  return cross_set;
}

// Returns true and sets *fresh_letter_out if the move places a fresh tile
// on (row, col). Played-through positions fall through to the board.
static inline int pat_unit_row(int dir, int lane_index, int idx) {
  return (dir == BOARD_HORIZONTAL_DIRECTION) ? lane_index : idx;
}

static inline int pat_unit_col(int dir, int lane_index, int idx) {
  return (dir == BOARD_HORIZONTAL_DIRECTION) ? idx : lane_index;
}

static inline MachineLetter pat_effective_letter(const Square *lane, int idx,
                                                 const PATMoveOverlay *overlay,
                                                 int row, int col) {
  MachineLetter fresh_letter;
  if (pat_move_covers(overlay, row, col, &fresh_letter)) {
    return fresh_letter;
  }
  return square_get_letter(&lane[idx]);
}

typedef struct PATCrossInfo {
  bool dead;
  bool hooky;
  int flex;
  int scaled_flex;
  // Sum over admissible letters of unseen count times the letter's
  // incremental immediate score at this square (see
  // pat_effective_cross_info), divided by PAT_HOOK_SCORE_SCALE.
  int score_exposure;
  // The real cross-set this hook or floater route needs, valid only when
  // hooky; 0 otherwise. Blank stays at bit 0, same convention as every
  // other cross/extension set (see pat_set_flex).
  uint64_t letter_set;
} PATCrossInfo;

// The hook square's own multipliers and the premium's word multiplier are
// what a letter placed there actually earns: the hooked word's existing
// score times the square's word multiplier, plus the letter's value under
// the square's letter multiplier counted in the hook word (times the
// square's word multiplier) and again in the lane word the premium
// multiplies. When the square IS the premium the two word multipliers are
// the same one, which the formula already handles.
static inline int pat_letter_score_exposure(int cross_score, int tile_score,
                                            int letter_multiplier,
                                            int word_multiplier,
                                            int premium_word_multiplier) {
  return word_multiplier * cross_score +
         letter_multiplier * tile_score *
             (word_multiplier + premium_word_multiplier);
}

// The perpendicular constraint at an empty square: dead (no letter can be
// placed), hooky (constrained by an adjacent perpendicular word, i.e. a
// real hook), or unconstrained. When the move places a tile perpendicular-
// adjacent to the square, the real post-move cross set would require a KWG
// traversal, so it is approximated with the per-letter hook_flex table; a
// pre-move dead square is left dead even though a fresh adjacent tile
// technically changes its perpendicular pattern. hyper_scale is only used
// for scaled_flex (see pat_hyper_scale); pass 1.0 when the caller has no
// use for that channel. ld and premium_word_multiplier feed
// score_exposure only; a caller that never reads it may pass NULL and any
// multiplier.
static PATCrossInfo pat_effective_cross_info(
    const Square *lane, int idx, int dir, const PATMoveOverlay *overlay,
    int row, int col, const uint8_t *unseen_counts, double hyper_scale,
    const LetterDistribution *ld, int premium_word_multiplier) {
  const uint64_t base_cross_set = square_get_cross_set(&lane[idx]);
  PATCrossInfo info;
  // Exact created hooks: if a fresh tile sits perpendicular-adjacent,
  // resolve the whole perpendicular pattern on the GADDAG and score it
  // like a real hook; an empty set makes the square dead.
  if (overlay != NULL && overlay->kwg != NULL && ld != NULL) {
    bool fresh_adjacent = false;
    for (int side = -1; side <= 1; side += 2) {
      const int perp_row =
          (dir == BOARD_HORIZONTAL_DIRECTION) ? row + side : row;
      const int perp_col =
          (dir == BOARD_HORIZONTAL_DIRECTION) ? col : col + side;
      MachineLetter fresh_letter;
      if (perp_row >= 0 && perp_row < BOARD_DIM && perp_col >= 0 &&
          perp_col < BOARD_DIM &&
          pat_move_covers(overlay, perp_row, perp_col, &fresh_letter)) {
        fresh_adjacent = true;
      }
    }
    if (fresh_adjacent) {
      int cross_score = 0;
      const uint64_t cross_set =
          pat_fresh_cross_set(overlay->kwg, ld, overlay->lanes, dir, row, col,
                              overlay, &cross_score);
      info.dead = (cross_set == 0);
      info.hooky = !info.dead && (cross_set != TRIVIAL_CROSS_SET);
      info.flex = info.hooky ? pat_set_flex(unseen_counts, cross_set) : 0;
      info.scaled_flex =
          info.hooky
              ? pat_set_flex_scaled(unseen_counts, cross_set, hyper_scale)
              : 0;
      info.letter_set = info.hooky ? cross_set : 0;
      info.score_exposure = 0;
      if (info.hooky) {
        const BonusSquare bonus = square_get_bonus_square(&lane[idx]);
        const int letter_multiplier = bonus_square_get_letter_multiplier(bonus);
        const int word_multiplier = bonus_square_get_word_multiplier(bonus);
        int64_t exposure = 0;
        uint64_t remaining = cross_set & ~(uint64_t)1;
        while (remaining) {
          const int machine_letter = pat_ctz(remaining);
          remaining &= remaining - 1;
          if (unseen_counts[machine_letter] == 0) {
            continue;
          }
          exposure +=
              (int64_t)unseen_counts[machine_letter] *
              pat_letter_score_exposure(
                  cross_score, equity_to_int(ld_get_score(ld, machine_letter)),
                  letter_multiplier, word_multiplier, premium_word_multiplier);
        }
        info.score_exposure = (int)(exposure / PAT_HOOK_SCORE_SCALE);
      }
      return info;
    }
  }
  info.dead = (base_cross_set == 0);

  info.hooky = !info.dead && (base_cross_set != TRIVIAL_CROSS_SET);
  info.flex = info.hooky ? pat_set_flex(unseen_counts, base_cross_set) : 0;
  info.scaled_flex =
      info.hooky
          ? pat_set_flex_scaled(unseen_counts, base_cross_set, hyper_scale)
          : 0;
  info.letter_set = info.hooky ? base_cross_set : 0;
  info.score_exposure = 0;
  const BonusSquare bonus = square_get_bonus_square(&lane[idx]);
  const int letter_multiplier = bonus_square_get_letter_multiplier(bonus);
  const int word_multiplier = bonus_square_get_word_multiplier(bonus);
  if (info.hooky && ld != NULL) {
    const int cross_score = equity_to_int(square_get_cross_score(&lane[idx]));
    int64_t exposure = 0;
    uint64_t remaining = base_cross_set & ~(uint64_t)1;
    while (remaining) {
      const int machine_letter = pat_ctz(remaining);
      remaining &= remaining - 1;
      if (unseen_counts[machine_letter] == 0) {
        continue;
      }
      exposure +=
          (int64_t)unseen_counts[machine_letter] *
          pat_letter_score_exposure(
              cross_score, equity_to_int(ld_get_score(ld, machine_letter)),
              letter_multiplier, word_multiplier, premium_word_multiplier);
    }
    info.score_exposure = (int)(exposure / PAT_HOOK_SCORE_SCALE);
  }
  if (!overlay || info.dead) {
    return info;
  }
  int fresh_flex = -1;
  int fresh_score = 0;
  for (int side = -1; side <= 1; side += 2) {
    int perp_row = row;
    int perp_col = col;
    if (dir == BOARD_HORIZONTAL_DIRECTION) {
      perp_row += side;
    } else {
      perp_col += side;
    }
    if (perp_row < 0 || perp_row >= BOARD_DIM || perp_col < 0 ||
        perp_col >= BOARD_DIM) {
      continue;
    }
    MachineLetter fresh_letter;
    if (pat_move_covers(overlay, perp_row, perp_col, &fresh_letter)) {
      const int flex =
          overlay->hook_flex[get_unblanked_machine_letter(fresh_letter)];
      if (fresh_flex < 0 || flex < fresh_flex) {
        fresh_flex = flex;
        fresh_score = (ld != NULL && !get_is_blanked(fresh_letter))
                          ? equity_to_int(ld_get_score(ld, fresh_letter))
                          : 0;
      }
    }
  }
  if (fresh_flex >= 0) {
    const int scaled_fresh_flex = (int)lround(fresh_flex * hyper_scale);
    info.scaled_flex = (info.hooky && info.scaled_flex < scaled_fresh_flex)
                           ? info.scaled_flex
                           : scaled_fresh_flex;
    info.flex = (info.hooky && info.flex < fresh_flex) ? info.flex : fresh_flex;
    // The hook the move itself creates has no real cross set or cross
    // score yet: the hooked word is approximated by the fresh tile facing
    // this square and the admissible letters by a typical two-point tile,
    // weighted by the same two-letter-word count the flexibility uses.
    if (ld != NULL) {
      const int fresh_exposure =
          fresh_flex *
          pat_letter_score_exposure(fresh_score, 2, letter_multiplier,
                                    word_multiplier, premium_word_multiplier) /
          PAT_HOOK_SCORE_SCALE;
      if (!info.hooky || fresh_exposure < info.score_exposure) {
        info.score_exposure = fresh_exposure;
      }
    }
    info.hooky = true;
  }
  return info;
}

// Scans one (TWS, dir) unit: walks outward from the empty TWS square along
// its lane on both sides, accumulating hook, floater, and triple-triple
// features binned by the number of tiles a word reaching the TWS must play.
// A triple-triple span is counted from both of its endpoint TWS squares;
// the double counting is consistent between training and evaluation, so the
// trained weight absorbs it.
// extent_lo/extent_hi (optional) receive the lowest and highest lane index
// the walk visited, including the square it broke on: a move can only
// change this unit's features by placing a tile on one of those squares or
// directly beside them in the perpendicular direction.
static void pat_scan_unit(const Square *lanes, const LetterDistribution *ld,
                          const uint8_t *unseen_counts, const PATWeights *pat,
                          int tws_row, int tws_col, int premium_class, int dir,
                          const PATMoveOverlay *overlay, int32_t *features,
                          int *extent_lo, int *extent_hi,
                          int opponent_rack_size, uint64_t *hook_letters_out) {
  const int max_reach =
      (opponent_rack_size < RACK_SIZE) ? opponent_rack_size : RACK_SIZE;
  const double hyper_scale = pat_hyper_scale(unseen_counts, opponent_rack_size);
  // Each premium class writes its own hook and floater-value channels. The
  // richer channels (floater flexibility, the lexicon through-table, and
  // the triple-triple pair) stay exclusive to triple word squares, which
  // are the ones worth the feature budget.
  int hook_base = PAT_FEATURE_HOOK_START;
  int float_score_base = PAT_FEATURE_FLOAT_SCORE_START;
  if (premium_class == PAT_PREMIUM_DWS) {
    hook_base = PAT_FEATURE_DWS_HOOK_START;
    float_score_base = PAT_FEATURE_DWS_FLOAT_SCORE_START;
  } else if (premium_class == PAT_PREMIUM_TLS) {
    hook_base = PAT_FEATURE_TLS_HOOK_START;
    float_score_base = PAT_FEATURE_TLS_FLOAT_SCORE_START;
  } else if (premium_class == PAT_PREMIUM_DLS) {
    hook_base = PAT_FEATURE_DLS_HOOK_START;
    float_score_base = PAT_FEATURE_DLS_FLOAT_SCORE_START;
  } else if (premium_class == PAT_PREMIUM_QWS) {
    hook_base = PAT_FEATURE_QWS_HOOK_START;
    float_score_base = PAT_FEATURE_QWS_FLOAT_SCORE_START;
  } else if (premium_class == PAT_PREMIUM_QLS) {
    hook_base = PAT_FEATURE_QLS_HOOK_START;
    float_score_base = PAT_FEATURE_QLS_FLOAT_SCORE_START;
  }
  const bool full_channels = (premium_class == PAT_PREMIUM_TWS);
  // Premium combinations (see PAT_FEATURE_LM_SPAN_START): the letter
  // multiplier of every empty square the walk needs, in walk order per
  // side (entry 0 is the premium itself), and each feasible route's bin
  // and the entry it makes contact at, resolved after both sides are
  // walked because a word may extend past the premium onto the far side.
  int lm_span_base = -1;
  int lm_ext_base = -1;
  if (premium_class == PAT_PREMIUM_TWS) {
    lm_span_base = PAT_FEATURE_LM_SPAN_START;
    lm_ext_base = PAT_FEATURE_LM_EXT_START;
  } else if (premium_class == PAT_PREMIUM_DWS) {
    lm_span_base = PAT_FEATURE_DWS_LM_SPAN_START;
    lm_ext_base = PAT_FEATURE_DWS_LM_EXT_START;
  }
  int8_t lm_entries[2][RACK_SIZE + 1];
  int lm_num_entries[2] = {1, 1};
  int8_t lm_route_bin[2][2 * RACK_SIZE + 2];
  int8_t lm_route_position[2][2 * RACK_SIZE + 2];
  int lm_num_routes[2] = {0, 0};
  const int lane_index =
      (dir == BOARD_HORIZONTAL_DIRECTION) ? tws_row : tws_col;
  const int tws_idx = (dir == BOARD_HORIZONTAL_DIRECTION) ? tws_col : tws_row;
  const Square *lane = board_get_row_cache(lanes, lane_index, dir);
  if (extent_lo) {
    *extent_lo = tws_idx;
  }
  if (extent_hi) {
    *extent_hi = tws_idx;
  }

  if (pat_effective_letter(lane, tws_idx, overlay,
                           pat_unit_row(dir, lane_index, tws_idx),
                           pat_unit_col(dir, lane_index, tws_idx)) !=
      ALPHABET_EMPTY_SQUARE_MARKER) {
    // The TWS square is covered (by the board or by the move itself):
    // nothing along this lane can reach it. Covering a TWS is exactly the
    // blocking reward.
    return;
  }
  const int premium_word_multiplier =
      bonus_square_get_word_multiplier(square_get_bonus_square(&lane[tws_idx]));
  const PATCrossInfo tws_info = pat_effective_cross_info(
      lane, tws_idx, dir, overlay, pat_unit_row(dir, lane_index, tws_idx),
      pat_unit_col(dir, lane_index, tws_idx), unseen_counts, hyper_scale, ld,
      premium_word_multiplier);
  if (tws_info.dead) {
    // No word along this lane can cover the TWS square at all.
    return;
  }
  lm_entries[0][0] = (int8_t)bonus_square_get_letter_multiplier(
      square_get_bonus_square(&lane[tws_idx]));
  lm_entries[1][0] = lm_entries[0][0];
  if (tws_info.hooky) {
    // A one-tile play on the TWS square itself completes a perpendicular
    // word at triple word score: hook access at d = 1.
    features[hook_base] += tws_info.flex;
    if (full_channels) {
      features[PAT_FEATURE_HOOK_SCALED_START] += tws_info.scaled_flex;
      features[PAT_FEATURE_HOOK_SCORE_START] += tws_info.score_exposure;
    }
    if (hook_letters_out) {
      *hook_letters_out |= tws_info.letter_set;
    }
    if (tws_info.flex > 0) {
      // Contact at the premium itself; either side is its far side.
      lm_route_bin[0][0] = 1;
      lm_route_position[0][0] = 0;
      lm_num_routes[0] = 1;
    }
  }

  for (int side = -1; side <= 1; side += 2) {
    const int lm_side = (side < 0) ? 0 : 1;
    // Number of empty squares a word covering the span from the current
    // scan position through the TWS square must fill, i.e. the number of
    // tiles the opponent must play. The TWS square itself is the first.
    int empties_used = 1;
    bool span_has_floater = false;
    int span_floater_flex = 0;
    bool span_has_hook = tws_info.hooky;
    int prev_empty_idx = tws_idx;
    int last_visited_idx = tws_idx;
    int idx = tws_idx + side;
    while (idx >= 0 && idx < BOARD_DIM) {
      last_visited_idx = idx;
      if (square_get_is_brick(&lane[idx])) {
        break;
      }
      const int square_row = pat_unit_row(dir, lane_index, idx);
      const int square_col = pat_unit_col(dir, lane_index, idx);
      const MachineLetter letter =
          pat_effective_letter(lane, idx, overlay, square_row, square_col);
      if (letter != ALPHABET_EMPTY_SQUARE_MARKER) {
        // A run of tiles: floater (playthrough) access. The run costs the
        // opponent no tiles, so the whole run shares the bin of the empty
        // span between it and the TWS square.
        const int distance_bin = empties_used;
        const MachineLetter facing_letter = letter;
        bool run_has_fresh_tile = false;
        // The run's letters in encounter order (facing tile first), for
        // the run-keyed through lookup.
        MachineLetter run_letters[BOARD_DIM];
        int run_length = 0;
        const bool run_through =
            pat != NULL && pat->run_through && full_channels;
        while (idx >= 0 && idx < BOARD_DIM &&
               !square_get_is_brick(&lane[idx])) {
          const int run_row = pat_unit_row(dir, lane_index, idx);
          const int run_col = pat_unit_col(dir, lane_index, idx);
          const MachineLetter run_letter =
              pat_effective_letter(lane, idx, overlay, run_row, run_col);
          if (run_letter == ALPHABET_EMPTY_SQUARE_MARKER) {
            break;
          }
          last_visited_idx = idx;
          MachineLetter fresh_letter;
          if (pat_move_covers(overlay, run_row, run_col, &fresh_letter)) {
            run_has_fresh_tile = true;
          }
          int tile_score = 0;
          if (!get_is_blanked(run_letter)) {
            tile_score = equity_to_int(ld_get_score(ld, run_letter));
          }
          features[float_score_base + distance_bin - 1] += tile_score;
          // What a word through this floater would actually lay down on
          // the way to the triple. The span it must cover is the empties
          // between the two plus both endpoints; a blank contributes
          // nothing to score above but reaches whatever its letter reaches.
          if (run_through && run_length < BOARD_DIM) {
            run_letters[run_length++] =
                get_unblanked_machine_letter(run_letter);
          }
          if (pat != NULL && full_channels && !run_through) {
            const MachineLetter unblanked =
                get_unblanked_machine_letter(run_letter);
            const int span = distance_bin + 1;
            if (span < PAT_MAX_THROUGH_LEN) {
              // A run beyond the premium (side > 0) ends the word that
              // reaches it from the premium; a run before it begins that
              // word. See PATWeights.signed_through.
              if (pat->signed_through) {
                const int word_end = (side > 0) ? 1 : 0;
                features[PAT_FEATURE_FLOAT_THROUGH_SCORE_START + distance_bin -
                         1] +=
                    pat->through_score_end[word_end][unblanked][span];
                features[PAT_FEATURE_FLOAT_THROUGH_COUNT_START + distance_bin -
                         1] +=
                    pat->through_count_end[word_end][unblanked][span];
              } else {
                features[PAT_FEATURE_FLOAT_THROUGH_SCORE_START + distance_bin -
                         1] += pat->through_score[unblanked][span];
                features[PAT_FEATURE_FLOAT_THROUGH_COUNT_START + distance_bin -
                         1] += pat->through_count[unblanked][span];
              }
            }
          }
          idx += side;
        }
        if (run_through && run_length > 0) {
          // The whole run at the end of a word of exactly the covering
          // length: a suffix when the run lies beyond the premium (the
          // word runs premium to run, encounter order is word order), a
          // prefix when before it (word order is the reverse). Keyed by
          // the run's far-end letters, up to PAT_RUN_THROUGH_MAX_KEY.
          const int word_length = distance_bin + run_length;
          const int key_len = (run_length < PAT_RUN_THROUGH_MAX_KEY)
                                  ? run_length
                                  : PAT_RUN_THROUGH_MAX_KEY;
          MachineLetter key[PAT_RUN_THROUGH_MAX_KEY];
          for (int k = 0; k < key_len; k++) {
            // Far-end letters in word order: beyond the premium the far end
            // is the last encountered and word order is encounter order;
            // before it, word order is reversed, so the word's first
            // letters are the last encountered, read backwards.
            key[k] = (side > 0) ? run_letters[run_length - key_len + k]
                                : run_letters[run_length - 1 - k];
          }
          const int word_end = (side > 0) ? 1 : 0;
          features[PAT_FEATURE_FLOAT_THROUGH_SCORE_START + distance_bin - 1] +=
              pat_get_run_through_score(pat, word_end, key, key_len,
                                        word_length);
          features[PAT_FEATURE_FLOAT_THROUGH_COUNT_START + distance_bin - 1] +=
              pat_get_run_through_count(pat, word_end, key, key_len,
                                        word_length);
        }
        int run_flex;
        int scaled_run_flex;
        if (run_has_fresh_tile) {
          // The run's real extension sets do not exist yet; approximate
          // with the two-letter-word flexibility of the tile facing the
          // TWS square.
          run_flex =
              overlay->hook_flex[get_unblanked_machine_letter(facing_letter)];
          scaled_run_flex = (int)lround(run_flex * hyper_scale);
        } else {
          // The letters that could extend this run toward the TWS square.
          // game_gen_cross_set stores a run's own extension sets on its
          // rightmost (highest lane index) tile, and the leftward one also
          // on the empty square just before the run; an empty square's
          // right_extension_set is never written and stays trivial. The
          // legacy reads below therefore see the trivial set (or the wrong
          // run's) and count every unseen tile; see
          // PATWeights.lexicon_floaters for why both are kept.
          uint64_t extension_set;
          if (pat != NULL && pat->lexicon_floaters) {
            extension_set =
                (side > 0)
                    ? square_get_left_extension_set(&lane[prev_empty_idx])
                    : square_get_right_extension_set(&lane[prev_empty_idx - 1]);
          } else {
            extension_set =
                (side > 0)
                    ? square_get_right_extension_set(&lane[prev_empty_idx])
                    : square_get_left_extension_set(&lane[prev_empty_idx]);
          }
          run_flex = pat_set_flex(unseen_counts, extension_set);
          scaled_run_flex =
              pat_set_flex_scaled(unseen_counts, extension_set, hyper_scale);
          if (hook_letters_out) {
            *hook_letters_out |= extension_set;
          }
        }
        if (full_channels) {
          features[PAT_FEATURE_FLOAT_FLEX_START + distance_bin - 1] += run_flex;
          features[PAT_FEATURE_FLOAT_FLEX_SCALED_START + distance_bin - 1] +=
              scaled_run_flex;
        }
        if (run_flex > 0 && lm_num_routes[lm_side] < 2 * RACK_SIZE + 2) {
          // Contact at the last empty before the run.
          lm_route_bin[lm_side][lm_num_routes[lm_side]] = (int8_t)distance_bin;
          lm_route_position[lm_side][lm_num_routes[lm_side]] =
              (int8_t)(lm_num_entries[lm_side] - 1);
          lm_num_routes[lm_side]++;
        }
        span_has_floater = true;
        span_floater_flex += run_flex;
        continue;
      }
      const PATCrossInfo info = pat_effective_cross_info(
          lane, idx, dir, overlay, square_row, square_col, unseen_counts,
          hyper_scale, ld, premium_word_multiplier);
      if (info.dead) {
        break;
      }
      empties_used++;
      if (empties_used > max_reach) {
        break;
      }
      if (lm_num_entries[lm_side] < RACK_SIZE + 1) {
        lm_entries[lm_side][lm_num_entries[lm_side]++] =
            (int8_t)bonus_square_get_letter_multiplier(
                square_get_bonus_square(&lane[idx]));
      }
      if (full_channels && bonus_square_get_word_multiplier(
                               square_get_bonus_square(&lane[idx])) == 3) {
        // A second empty TWS within reach: the triple-triple span from the
        // scanned TWS square through this one.
        if (span_has_floater) {
          features[PAT_FEATURE_TT_FLOATER] += 1 + span_floater_flex;
        } else if (span_has_hook) {
          features[PAT_FEATURE_TT_HOOK_ONLY] += 1;
        }
        break;
      }
      if (info.hooky) {
        features[hook_base + empties_used - 1] += info.flex;
        if (full_channels) {
          features[PAT_FEATURE_HOOK_SCALED_START + empties_used - 1] +=
              info.scaled_flex;
          features[PAT_FEATURE_HOOK_SCORE_START + empties_used - 1] +=
              info.score_exposure;
        }
        if (hook_letters_out) {
          *hook_letters_out |= info.letter_set;
        }
        if (info.flex > 0 && lm_num_routes[lm_side] < 2 * RACK_SIZE + 2) {
          // Contact at the hook square, the entry just recorded.
          lm_route_bin[lm_side][lm_num_routes[lm_side]] = (int8_t)empties_used;
          lm_route_position[lm_side][lm_num_routes[lm_side]] =
              (int8_t)(lm_num_entries[lm_side] - 1);
          lm_num_routes[lm_side]++;
        }
        span_has_hook = true;
      }
      prev_empty_idx = idx;
      idx += side;
    }
    if (side < 0 && extent_lo) {
      *extent_lo = last_visited_idx;
    } else if (side > 0 && extent_hi) {
      *extent_hi = last_visited_idx;
    }
  }
  if (lm_span_base < 0) {
    return;
  }
  for (int lm_side = 0; lm_side < 2; lm_side++) {
    const int other = 1 - lm_side;
    const int num_entries = lm_num_entries[lm_side];
    // Largest letter multiplier up to each entry (the span of a route
    // contacting there) and from each entry on (its extension beyond).
    int prefix_max[RACK_SIZE + 1];
    int suffix_max[RACK_SIZE + 2];
    prefix_max[0] = lm_entries[lm_side][0];
    for (int k = 1; k < num_entries; k++) {
      prefix_max[k] = (lm_entries[lm_side][k] > prefix_max[k - 1])
                          ? lm_entries[lm_side][k]
                          : prefix_max[k - 1];
    }
    suffix_max[num_entries] = 1;
    for (int k = num_entries - 1; k >= 0; k--) {
      suffix_max[k] = (lm_entries[lm_side][k] > suffix_max[k + 1])
                          ? lm_entries[lm_side][k]
                          : suffix_max[k + 1];
    }
    for (int r = 0; r < lm_num_routes[lm_side]; r++) {
      const int bin = lm_route_bin[lm_side][r];
      const int position = lm_route_position[lm_side][r];
      const int span_lm = prefix_max[position];
      int ext_lm = suffix_max[position + 1];
      // Past the premium onto the far side, with the tiles the route
      // leaves in the rack (entry 0 there is the premium again).
      const int budget = max_reach - bin;
      for (int k = 1; k <= budget && k < lm_num_entries[other]; k++) {
        if (lm_entries[other][k] > ext_lm) {
          ext_lm = lm_entries[other][k];
        }
      }
      features[lm_span_base + bin - 1] +=
          premium_word_multiplier * (span_lm - 1);
      if (ext_lm > span_lm) {
        features[lm_ext_base + bin - 1] +=
            premium_word_multiplier * (ext_lm - 1);
      }
    }
  }
}

// Scans one double-double window: the lane squares from lo to hi, whose
// endpoints are both double word squares. A word covering the window doubles
// twice, so it is worth about as much as a triple-triple and is defended the
// same way. The window is dead when either endpoint is covered (covering one
// is exactly the blocking reward), when a square inside it is bricked or has
// an empty cross set, or when it still needs more fresh tiles than a rack
// holds. A live window also needs somewhere to attach: a playthrough tile
// inside it, or a hookable empty square.
static void pat_scan_dd_unit(const Square *lanes, const uint8_t *unseen_counts,
                             int dir, int lane_index, int lo, int hi, int tier,
                             const PATMoveOverlay *overlay, int32_t *features,
                             int *extent_lo, int *extent_hi,
                             int opponent_rack_size,
                             uint64_t *hook_letters_out) {
  const int max_reach =
      (opponent_rack_size < RACK_SIZE) ? opponent_rack_size : RACK_SIZE;
  const int tier_base =
      PAT_FEATURE_WINDOW_START + tier * PAT_WINDOW_FEATURES_PER_TIER;
  const Square *lane = board_get_row_cache(lanes, lane_index, dir);
  if (extent_lo != NULL) {
    *extent_lo = lo;
  }
  if (extent_hi != NULL) {
    *extent_hi = hi;
  }
  int empties = 0;
  bool has_floater = false;
  bool has_hook = false;
  for (int idx = lo; idx <= hi; idx++) {
    if (square_get_is_brick(&lane[idx])) {
      return;
    }
    const int square_row = pat_unit_row(dir, lane_index, idx);
    const int square_col = pat_unit_col(dir, lane_index, idx);
    const MachineLetter letter =
        pat_effective_letter(lane, idx, overlay, square_row, square_col);
    if (letter != ALPHABET_EMPTY_SQUARE_MARKER) {
      if (idx == lo || idx == hi) {
        return;
      }
      has_floater = true;
      continue;
    }
    // Never reads scaled_flex or score_exposure, so hyper_scale, ld and
    // the premium multiplier are don't-cares here.
    const PATCrossInfo info =
        pat_effective_cross_info(lane, idx, dir, overlay, square_row,
                                 square_col, unseen_counts, 1.0, NULL, 2);
    if (info.dead) {
      return;
    }
    if (info.hooky) {
      has_hook = true;
      if (hook_letters_out) {
        *hook_letters_out |= info.letter_set;
      }
    }
    empties++;
  }
  if (empties > max_reach) {
    return;
  }
  if (has_floater) {
    features[tier_base] += 1;
  } else if (has_hook) {
    features[tier_base + 1] += 1;
  } else {
    return;
  }
  features[tier_base + 2] += max_reach - empties;
}

// Finds the double-double windows: consecutive pairs of double word squares
// in one lane, near enough that a single word could cover both. Horizontal
// lanes come first, then vertical, each scanned in increasing order, so the
// truncation at PAT_MAX_DD is deterministic and training and evaluation
// always agree. Whether a window is currently live is left to the scan.
// Which window tier the product of two word multipliers belongs to.
static int pat_window_tier(int product) {
  if (product <= 4) {
    return 0; // double-double
  }
  if (product <= 8) {
    return 1; // double-triple, double-quad
  }
  if (product == 9) {
    return 2; // triple-triple
  }
  return 3; // triple-quad, quad-quad
}

// Finds the windows: consecutive pairs of word-multiplier squares in one
// lane, near enough that a single word could cover both. Horizontal lanes
// come first, then vertical, each scanned in increasing order, so the
// truncation at PAT_MAX_DD is deterministic and training and evaluation
// always agree. Whether a window is currently live is left to the scan.
static int pat_find_dd(const Square *lanes, uint8_t *dd_dirs, uint8_t *dd_lanes,
                       uint8_t *dd_los, uint8_t *dd_his, uint8_t *dd_tiers) {
  int num_dd = 0;
  for (int dir = 0; dir < 2; dir++) {
    for (int lane_index = 0; lane_index < BOARD_DIM; lane_index++) {
      const Square *lane = board_get_row_cache(lanes, lane_index, dir);
      int previous_idx = -1;
      int previous_multiplier = 0;
      for (int idx = 0; idx < BOARD_DIM; idx++) {
        const int word_multiplier = bonus_square_get_word_multiplier(
            square_get_bonus_square(&lane[idx]));
        if (word_multiplier < 2) {
          continue;
        }
        if (previous_idx >= 0 && idx - previous_idx <= PAT_DD_MAX_SPAN) {
          if (num_dd == PAT_MAX_DD) {
            return num_dd;
          }
          dd_dirs[num_dd] = (uint8_t)dir;
          dd_lanes[num_dd] = (uint8_t)lane_index;
          dd_los[num_dd] = (uint8_t)previous_idx;
          dd_his[num_dd] = (uint8_t)idx;
          dd_tiers[num_dd] =
              (uint8_t)pat_window_tier(previous_multiplier * word_multiplier);
          num_dd++;
        }
        previous_idx = idx;
        previous_multiplier = word_multiplier;
      }
    }
  }
  return num_dd;
}

// Which premium class a square belongs to, or -1 if it is not one worth
// walking a lane for.
static int pat_premium_class_of(const Square *square) {
  const BonusSquare bonus = square_get_bonus_square(square);
  const int word_multiplier = bonus_square_get_word_multiplier(bonus);
  if (word_multiplier >= 4) {
    return PAT_PREMIUM_QWS;
  }
  if (word_multiplier == 3) {
    return PAT_PREMIUM_TWS;
  }
  if (word_multiplier == 2) {
    return PAT_PREMIUM_DWS;
  }
  const int letter_multiplier = bonus_square_get_letter_multiplier(bonus);
  if (letter_multiplier >= 4) {
    return PAT_PREMIUM_QLS;
  }
  if (letter_multiplier == 3) {
    return PAT_PREMIUM_TLS;
  }
  if (letter_multiplier == 2) {
    return PAT_PREMIUM_DLS;
  }
  return -1;
}

// Finds up to PAT_MAX_PREMIUM uncovered premium squares in row-major order
// (the truncation is deterministic, so training and evaluation always
// agree). Bricked and occupied squares are excluded: a covered premium
// square can never be uncovered by a move.
static int pat_find_tws(const Square *lanes, uint8_t *tws_rows,
                        uint8_t *tws_cols, uint8_t *tws_classes) {
  int num_tws = 0;
  for (int row = 0; row < BOARD_DIM; row++) {
    const Square *lane =
        board_get_row_cache(lanes, row, BOARD_HORIZONTAL_DIRECTION);
    for (int col = 0; col < BOARD_DIM; col++) {
      const Square *square = &lane[col];
      if (square_get_is_brick(square) ||
          square_get_letter(square) != ALPHABET_EMPTY_SQUARE_MARKER) {
        continue;
      }
      const int premium_class = pat_premium_class_of(square);
      if (premium_class < 0) {
        continue;
      }
      if (num_tws == PAT_MAX_PREMIUM) {
        return num_tws;
      }
      tws_rows[num_tws] = (uint8_t)row;
      tws_cols[num_tws] = (uint8_t)col;
      tws_classes[num_tws] = (uint8_t)premium_class;
      num_tws++;
    }
  }
  return num_tws;
}

void pat_extract_features(const Square *lanes, const LetterDistribution *ld,
                          const Rack *player_rack, const PATWeights *pat,
                          int opponent_rack_size, int32_t *features) {
  memset(features, 0, sizeof(int32_t) * PAT_NUM_FEATURES);
  uint8_t unseen_counts[MAX_ALPHABET_SIZE];
  pat_compute_unseen_counts(lanes, ld, player_rack, unseen_counts);
  uint8_t tws_rows[PAT_MAX_PREMIUM];
  uint8_t tws_cols[PAT_MAX_PREMIUM];
  uint8_t tws_classes[PAT_MAX_PREMIUM];
  const int num_tws = pat_find_tws(lanes, tws_rows, tws_cols, tws_classes);
  for (int tws_idx = 0; tws_idx < num_tws; tws_idx++) {
    pat_scan_unit(lanes, ld, unseen_counts, pat, tws_rows[tws_idx],
                  tws_cols[tws_idx], tws_classes[tws_idx],
                  BOARD_HORIZONTAL_DIRECTION, NULL, features, NULL, NULL,
                  opponent_rack_size, NULL);
    pat_scan_unit(lanes, ld, unseen_counts, pat, tws_rows[tws_idx],
                  tws_cols[tws_idx], tws_classes[tws_idx],
                  BOARD_VERTICAL_DIRECTION, NULL, features, NULL, NULL,
                  opponent_rack_size, NULL);
  }
  uint8_t dd_dirs[PAT_MAX_DD];
  uint8_t dd_lanes[PAT_MAX_DD];
  uint8_t dd_los[PAT_MAX_DD];
  uint8_t dd_his[PAT_MAX_DD];
  uint8_t dd_tiers[PAT_MAX_DD];
  const int num_dd =
      pat_find_dd(lanes, dd_dirs, dd_lanes, dd_los, dd_his, dd_tiers);
  for (int dd_idx = 0; dd_idx < num_dd; dd_idx++) {
    pat_scan_dd_unit(lanes, unseen_counts, dd_dirs[dd_idx], dd_lanes[dd_idx],
                     dd_los[dd_idx], dd_his[dd_idx], dd_tiers[dd_idx], NULL,
                     features, NULL, NULL, opponent_rack_size, NULL);
  }
}

static int64_t pat_dot_raw(const PATWeights *pat, const int32_t *features) {
  int64_t acc = 0;
  for (int feature_index = 0; feature_index < PAT_NUM_FEATURES;
       feature_index++) {
    acc += (int64_t)pat->weights[feature_index] * features[feature_index];
  }
  return acc;
}

static Equity pat_clamp_dot(int64_t acc) {
  if (acc < EQUITY_MIN_VALUE) {
    acc = EQUITY_MIN_VALUE;
  }
  if (acc > 0) {
    // Cannot happen with the enforced weight and feature signs; clamp
    // anyway so the shadow invariant survives any future bug here.
    acc = 0;
  }
  return (Equity)acc;
}

static Equity pat_dot(const PATWeights *pat, const int32_t *features) {
  return pat_clamp_dot(pat_dot_raw(pat, features));
}

// The same product over only the weighted features; the other terms are
// zero. Every path that has a context uses this one.
static Equity pat_dot_ctx(const PATEvalContext *pat_eval_ctx,
                          const int32_t *features) {
  const Equity *weights = pat_eval_ctx->weights->weights;
  int64_t acc = 0;
  for (int nonzero_idx = 0; nonzero_idx < pat_eval_ctx->num_nonzero_features;
       nonzero_idx++) {
    const int feature_index = pat_eval_ctx->nonzero_feature_index[nonzero_idx];
    acc += (int64_t)weights[feature_index] * features[feature_index];
  }
  return pat_clamp_dot(acc);
}

// The per-row and per-column unit masks are 64-bit.
static_assert(PAT_MASK_WORDS >= 1, "unit masks need at least one word");

// The affected-unit set outgrew one word when the lesser premium squares
// joined the scan, so it is a small fixed bitset. All of these are hot: the
// per-move path builds one and walks its bits.
static inline void pat_mask_clear(uint64_t *mask) {
  for (int word = 0; word < PAT_MASK_WORDS; word++) {
    mask[word] = 0;
  }
}

static inline void pat_mask_set(uint64_t *mask, int unit_index) {
  mask[unit_index / 64] |= (uint64_t)1 << (unit_index % 64);
}

static inline bool pat_mask_test(const uint64_t *mask, int unit_index) {
  return (mask[unit_index / 64] >> (unit_index % 64)) & 1;
}

static inline void pat_mask_or_into(uint64_t *dst, const uint64_t *src) {
  for (int word = 0; word < PAT_MASK_WORDS; word++) {
    dst[word] |= src[word];
  }
}

static inline bool pat_mask_is_empty(const uint64_t *mask) {
  for (int word = 0; word < PAT_MASK_WORDS; word++) {
    if (mask[word] != 0) {
      return false;
    }
  }
  return true;
}

// Combines the per-unit penalties into the position's defense term. The
// opponent plays one move next turn, so two open lanes are not two separate
// losses: the worst route is charged in full and every other route at
// gamma, which is a convex combination of the minimum and the sum. Gamma 1
// is the plain sum. Being a convex combination of non-positive values the
// result is non-positive, and it is nondecreasing in every unit penalty,
// which is what lets the shadow bound below zero out the units a move can
// reach.
static Equity pat_combine(int64_t worst, int64_t sum, double combine_gamma) {
  double combined =
      (1.0 - combine_gamma) * (double)worst + combine_gamma * (double)sum;
  if (combined > 0.0) {
    combined = 0.0;
  }
  if (combined < (double)EQUITY_MIN_VALUE) {
    combined = (double)EQUITY_MIN_VALUE;
  }
  return (Equity)llround(combined);
}

static Equity pat_combine_unit_penalties(const Equity *unit_penalties,
                                         int num_units, double combine_gamma) {
  int64_t sum = 0;
  int64_t worst = 0;
  for (int unit_index = 0; unit_index < num_units; unit_index++) {
    const int64_t penalty = unit_penalties[unit_index];
    sum += penalty;
    if (penalty < worst) {
      worst = penalty;
    }
  }
  return pat_combine(worst, sum, combine_gamma);
}

// The moves affecting the units can at best zero out each affected unit's (<=
// 0) baseline contribution; every unaffected unit keeps its baseline exactly.
static Equity pat_units_penalty_bound(const PATEvalContext *pat_eval_ctx,
                                      const uint64_t *affected_units) {
  // A move can at best zero out every unit it reaches, and the combination
  // is nondecreasing in each unit, so combining with those units at zero
  // bounds the term from above. A zeroed unit adds nothing to the sum and
  // can never be the worst, so it simply drops out of both: the sum is the
  // total less the reached baselines, and the worst is the first unit in
  // penalty order the move does not reach. Only the reached units are
  // visited, and a move reaches few.
  int64_t sum = pat_eval_ctx->total_unit_penalty;
  for (int word = 0; word < PAT_MASK_WORDS; word++) {
    uint64_t bits = affected_units[word];
    while (bits != 0) {
      const int unit_index = word * 64 + pat_ctz(bits);
      sum -= pat_eval_ctx->unit_penalty[unit_index];
      bits &= bits - 1;
    }
  }
  int64_t worst = 0;
  for (int order_idx = 0; order_idx < pat_eval_ctx->num_units; order_idx++) {
    const int unit_index = pat_eval_ctx->units_by_penalty[order_idx];
    if (!pat_mask_test(affected_units, unit_index)) {
      worst = pat_eval_ctx->unit_penalty[unit_index];
      break;
    }
  }
  return pat_combine(worst, sum, pat_eval_ctx->weights->combine_gamma);
}

void pat_eval_context_disable(PATEvalContext *pat_eval_ctx) {
  pat_eval_ctx->weights = NULL;
}

// Scans one of the context's units into `features`, reporting the lane it
// walks and the span of lane squares that walk could read. Units 2*i and
// 2*i+1 are TWS i's horizontal and vertical walks; the units after those are
// the double-double windows.
static void pat_scan_context_unit(const PATEvalContext *pat_eval_ctx,
                                  int unit_index, const PATMoveOverlay *overlay,
                                  int32_t *features, int *dir_out,
                                  int *lane_out, int *extent_lo, int *extent_hi,
                                  uint64_t *hook_letters_out) {
  const int num_tws_units = pat_eval_ctx->num_tws * 2;
  if (unit_index < num_tws_units) {
    const int tws_idx = unit_index / 2;
    const int dir = unit_index % 2;
    const int tws_row = pat_eval_ctx->tws_rows[tws_idx];
    const int tws_col = pat_eval_ctx->tws_cols[tws_idx];
    *dir_out = dir;
    *lane_out = (dir == BOARD_HORIZONTAL_DIRECTION) ? tws_row : tws_col;
    pat_scan_unit(pat_eval_ctx->lanes, pat_eval_ctx->ld,
                  pat_eval_ctx->unseen_counts, pat_eval_ctx->weights, tws_row,
                  tws_col, pat_eval_ctx->tws_classes[tws_idx], dir, overlay,
                  features, extent_lo, extent_hi,
                  pat_eval_ctx->opponent_rack_size, hook_letters_out);
    return;
  }
  const int dd_idx = unit_index - num_tws_units;
  *dir_out = pat_eval_ctx->dd_dirs[dd_idx];
  *lane_out = pat_eval_ctx->dd_lanes[dd_idx];
  pat_scan_dd_unit(pat_eval_ctx->lanes, pat_eval_ctx->unseen_counts,
                   pat_eval_ctx->dd_dirs[dd_idx],
                   pat_eval_ctx->dd_lanes[dd_idx], pat_eval_ctx->dd_los[dd_idx],
                   pat_eval_ctx->dd_his[dd_idx], pat_eval_ctx->dd_tiers[dd_idx],
                   overlay, features, extent_lo, extent_hi,
                   pat_eval_ctx->opponent_rack_size, hook_letters_out);
}

// Whether any channel a walk from this premium class can write carries a
// nonzero weight and the runtime mask has not excluded the class outright.
// Each class writes only its own hook and floater-value channels; the
// triple word class alone also writes the flexibility, through-table and
// triple-triple channels.
static bool pat_class_is_weighted(const PATWeights *weights, int premium_class,
                                  uint32_t enabled_classes_mask) {
  if (!(enabled_classes_mask & (1u << premium_class))) {
    return false;
  }
  int start = 0;
  int end = 0;
  switch (premium_class) {
  case PAT_PREMIUM_TWS:
    start = PAT_FEATURE_HOOK_START;
    end = PAT_FEATURE_DWS_HOOK_START;
    break;
  case PAT_PREMIUM_DWS:
    start = PAT_FEATURE_DWS_HOOK_START;
    end = PAT_FEATURE_TLS_HOOK_START;
    break;
  case PAT_PREMIUM_TLS:
    start = PAT_FEATURE_TLS_HOOK_START;
    end = PAT_FEATURE_DLS_HOOK_START;
    break;
  case PAT_PREMIUM_DLS:
    start = PAT_FEATURE_DLS_HOOK_START;
    end = PAT_FEATURE_QWS_HOOK_START;
    break;
  case PAT_PREMIUM_QWS:
    start = PAT_FEATURE_QWS_HOOK_START;
    end = PAT_FEATURE_QLS_HOOK_START;
    break;
  case PAT_PREMIUM_QLS:
    start = PAT_FEATURE_QLS_HOOK_START;
    end = PAT_FEATURE_TT_FLOATER;
    break;
  default:
    return true;
  }
  for (int feature_index = start; feature_index < end; feature_index++) {
    if (weights->weights[feature_index] != 0) {
      return true;
    }
  }
  return premium_class == PAT_PREMIUM_TWS &&
         (weights->weights[PAT_FEATURE_TT_FLOATER] != 0 ||
          weights->weights[PAT_FEATURE_TT_HOOK_ONLY] != 0);
}

// Whether any of a window tier's three channels carries a nonzero weight
// and the runtime mask has not excluded windows outright.
static bool pat_tier_is_weighted(const PATWeights *weights, int tier,
                                 uint32_t enabled_classes_mask) {
  if (!(enabled_classes_mask & PAT_CLASS_MASK_WINDOWS)) {
    return false;
  }
  const int start =
      PAT_FEATURE_WINDOW_START + tier * PAT_WINDOW_FEATURES_PER_TIER;
  for (int feature_index = start;
       feature_index < start + PAT_WINDOW_FEATURES_PER_TIER; feature_index++) {
    if (weights->weights[feature_index] != 0) {
      return true;
    }
  }
  return false;
}

// Drops the units whose every channel is unweighted. Such a unit's penalty
// is zero on any board, with or without a move overlaid, and a zero penalty
// is never the worst unit and adds nothing to the sum, so removing it
// changes no penalty, combination, or lane bound: it only saves the walk.
// The enumeration and its caps have already run, so which squares exist
// was decided exactly as training decides it.
static void pat_drop_unweighted_units(PATEvalContext *pat_eval_ctx,
                                      const PATWeights *weights,
                                      uint32_t enabled_classes_mask) {
  bool class_weighted[PAT_NUM_PREMIUM_CLASSES];
  for (int premium_class = 0; premium_class < PAT_NUM_PREMIUM_CLASSES;
       premium_class++) {
    class_weighted[premium_class] =
        pat_class_is_weighted(weights, premium_class, enabled_classes_mask);
  }
  int kept = 0;
  for (int tws_idx = 0; tws_idx < pat_eval_ctx->num_tws; tws_idx++) {
    if (!class_weighted[pat_eval_ctx->tws_classes[tws_idx]]) {
      continue;
    }
    pat_eval_ctx->tws_rows[kept] = pat_eval_ctx->tws_rows[tws_idx];
    pat_eval_ctx->tws_cols[kept] = pat_eval_ctx->tws_cols[tws_idx];
    pat_eval_ctx->tws_classes[kept] = pat_eval_ctx->tws_classes[tws_idx];
    kept++;
  }
  pat_eval_ctx->num_tws = kept;

  bool tier_weighted[PAT_WINDOW_TIER_COUNT];
  for (int tier = 0; tier < PAT_WINDOW_TIER_COUNT; tier++) {
    tier_weighted[tier] =
        pat_tier_is_weighted(weights, tier, enabled_classes_mask);
  }
  kept = 0;
  for (int dd_idx = 0; dd_idx < pat_eval_ctx->num_dd; dd_idx++) {
    if (!tier_weighted[pat_eval_ctx->dd_tiers[dd_idx]]) {
      continue;
    }
    pat_eval_ctx->dd_dirs[kept] = pat_eval_ctx->dd_dirs[dd_idx];
    pat_eval_ctx->dd_lanes[kept] = pat_eval_ctx->dd_lanes[dd_idx];
    pat_eval_ctx->dd_los[kept] = pat_eval_ctx->dd_los[dd_idx];
    pat_eval_ctx->dd_his[kept] = pat_eval_ctx->dd_his[dd_idx];
    pat_eval_ctx->dd_tiers[kept] = pat_eval_ctx->dd_tiers[dd_idx];
    kept++;
  }
  pat_eval_ctx->num_dd = kept;
}

static void pat_eval_context_load_units(
    PATEvalContext *pat_eval_ctx, const PATWeights *weights,
    const Square *lanes, const LetterDistribution *ld, const Rack *player_rack,
    bool drop_unweighted_units, uint32_t enabled_classes_mask,
    int opponent_rack_size) {
  pat_eval_ctx->weights = weights;
  if (!weights) {
    return;
  }
  pat_eval_ctx->opponent_rack_size = opponent_rack_size;
  pat_eval_ctx->active_classes_mask = 0;
  for (int premium_class = 0; premium_class < PAT_NUM_PREMIUM_CLASSES;
       premium_class++) {
    if (pat_class_is_weighted(weights, premium_class, enabled_classes_mask)) {
      pat_eval_ctx->active_classes_mask |= 1u << premium_class;
    }
  }
  pat_eval_ctx->ld = ld;
  pat_eval_ctx->lanes = lanes;
  pat_compute_unseen_counts(lanes, ld, player_rack,
                            pat_eval_ctx->unseen_counts);
  pat_eval_ctx->num_tws =
      pat_find_tws(lanes, pat_eval_ctx->tws_rows, pat_eval_ctx->tws_cols,
                   pat_eval_ctx->tws_classes);
  pat_eval_ctx->num_dd = pat_find_dd(
      lanes, pat_eval_ctx->dd_dirs, pat_eval_ctx->dd_lanes,
      pat_eval_ctx->dd_los, pat_eval_ctx->dd_his, pat_eval_ctx->dd_tiers);
  if (drop_unweighted_units) {
    pat_drop_unweighted_units(pat_eval_ctx, weights, enabled_classes_mask);
  }
  pat_eval_ctx->num_units = pat_eval_ctx->num_tws * 2 + pat_eval_ctx->num_dd;
  memset(pat_eval_ctx->unit_mask_by_row, 0,
         sizeof(pat_eval_ctx->unit_mask_by_row));
  memset(pat_eval_ctx->unit_mask_by_col, 0,
         sizeof(pat_eval_ctx->unit_mask_by_col));
  static_assert(PAT_MAX_SCAN_UNITS <= PAT_MASK_WORDS * 64,
                "unit masks must cover every scan unit");
  static_assert(PAT_MAX_SCAN_UNITS <= UINT16_MAX,
                "unit order entries must hold every unit index");
  pat_eval_ctx->num_nonzero_features = 0;
  for (int feature_index = 0; feature_index < PAT_NUM_FEATURES;
       feature_index++) {
    if (weights->weights[feature_index] != 0) {
      pat_eval_ctx
          ->nonzero_feature_index[pat_eval_ctx->num_nonzero_features++] =
          feature_index;
    }
  }
  for (int unit_index = 0; unit_index < pat_eval_ctx->num_units; unit_index++) {
    int32_t *unit_features = pat_eval_ctx->unit_features[unit_index];
    memset(unit_features, 0, sizeof(int32_t) * PAT_NUM_FEATURES);
    int dir = 0;
    int lane = 0;
    int extent_lo = 0;
    int extent_hi = 0;
    pat_eval_ctx->unit_hook_letters[unit_index] = 0;
    pat_scan_context_unit(pat_eval_ctx, unit_index, NULL, unit_features, &dir,
                          &lane, &extent_lo, &extent_hi,
                          &pat_eval_ctx->unit_hook_letters[unit_index]);
    pat_eval_ctx->unit_penalty[unit_index] =
        pat_dot_ctx(pat_eval_ctx, unit_features);
    // A move affects this unit only when it has a tile on or directly
    // beside the lane (perpendicular halo of one) within the span of
    // squares the baseline walk visited: squares beyond the walk's break
    // point are unreachable within the empty-square budget either way.
    // Farther effects (a move extending a distant perpendicular word
    // into a lane square's cross set) are deliberately ignored in the
    // per-move delta.
    uint64_t (*halo_masks)[PAT_MASK_WORDS] =
        (dir == BOARD_HORIZONTAL_DIRECTION) ? pat_eval_ctx->unit_mask_by_row
                                            : pat_eval_ctx->unit_mask_by_col;
    uint64_t (*extent_masks)[PAT_MASK_WORDS] =
        (dir == BOARD_HORIZONTAL_DIRECTION) ? pat_eval_ctx->unit_mask_by_col
                                            : pat_eval_ctx->unit_mask_by_row;
    for (int halo = lane - 1; halo <= lane + 1; halo++) {
      if (halo >= 0 && halo < BOARD_DIM) {
        pat_mask_set(halo_masks[halo], unit_index);
      }
    }
    for (int idx = extent_lo; idx <= extent_hi; idx++) {
      pat_mask_set(extent_masks[idx], unit_index);
    }
  }
  // Reverse index of unit_hook_letters: bit u of units_by_hook_letter[L] set
  // exactly when unit u's baseline scan found a live hook or floater route
  // admitting letter L. Built once per position so a move's own leave (at
  // most RACK_SIZE distinct letters) can find every unit it could exploit
  // in a handful of ORs, entirely independent of the move's placement.
  memset(pat_eval_ctx->units_by_hook_letter, 0,
         sizeof(pat_eval_ctx->units_by_hook_letter));
  for (int unit_index = 0; unit_index < pat_eval_ctx->num_units; unit_index++) {
    uint64_t remaining_letters =
        pat_eval_ctx->unit_hook_letters[unit_index] & ~(uint64_t)1;
    while (remaining_letters) {
      const int machine_letter = pat_ctz(remaining_letters);
      remaining_letters &= remaining_letters - 1;
      pat_mask_set(pat_eval_ctx->units_by_hook_letter[machine_letter],
                   unit_index);
    }
  }
  // The total and the penalty order that let a move's combination be
  // formed from the units it reaches alone (see pat_units_penalty_bound).
  // Insertion sort: a few dozen units, once per position.
  int64_t total_unit_penalty = 0;
  for (int unit_index = 0; unit_index < pat_eval_ctx->num_units; unit_index++) {
    const Equity penalty = pat_eval_ctx->unit_penalty[unit_index];
    total_unit_penalty += penalty;
    int order_idx = unit_index;
    while (
        order_idx > 0 &&
        pat_eval_ctx
                ->unit_penalty[pat_eval_ctx->units_by_penalty[order_idx - 1]] >
            penalty) {
      pat_eval_ctx->units_by_penalty[order_idx] =
          pat_eval_ctx->units_by_penalty[order_idx - 1];
      order_idx--;
    }
    pat_eval_ctx->units_by_penalty[order_idx] = (uint16_t)unit_index;
  }
  pat_eval_ctx->total_unit_penalty = total_unit_penalty;
  pat_eval_ctx->pre_penalty = pat_combine_unit_penalties(
      pat_eval_ctx->unit_penalty, pat_eval_ctx->num_units,
      weights->combine_gamma);
  // Every move's leave is some subset of the player's starting rack, so the
  // units ANY move from this position could earn leave credit for (see
  // pat_leave_affected_units) are a subset of the units reachable through
  // some letter the player's rack holds right now. lane_penalty_bound is
  // position-level, not move-level, so it cannot know which specific leave
  // a later move will keep; folding this worst case in uniformly keeps it a
  // sound upper bound for every move in the lane rather than only the ones
  // whose leave happens to be empty. Empty whenever the file carries no
  // discount, so that case's bound is identical to before this feature.
  pat_mask_clear(pat_eval_ctx->worst_case_leave_units);
  if (weights->own_asset_discount > 0.0 && player_rack) {
    bool rack_has_blank =
        rack_get_letter(player_rack, BLANK_MACHINE_LETTER) > 0;
    const int dist_size = rack_get_dist_size(player_rack);
    for (int ml = 1; ml < dist_size; ml++) {
      if (rack_get_letter(player_rack, ml) > 0) {
        pat_mask_or_into(pat_eval_ctx->worst_case_leave_units,
                         pat_eval_ctx->units_by_hook_letter[ml]);
      }
    }
    if (rack_has_blank) {
      for (int ml = 1; ml < MAX_ALPHABET_SIZE; ml++) {
        pat_mask_or_into(pat_eval_ctx->worst_case_leave_units,
                         pat_eval_ctx->units_by_hook_letter[ml]);
      }
    }
  }
  for (int lane = 0; lane < BOARD_DIM; lane++) {
    uint64_t row_bound_units[PAT_MASK_WORDS];
    uint64_t col_bound_units[PAT_MASK_WORDS];
    for (int word = 0; word < PAT_MASK_WORDS; word++) {
      row_bound_units[word] = pat_eval_ctx->unit_mask_by_row[lane][word] |
                              pat_eval_ctx->worst_case_leave_units[word];
      col_bound_units[word] = pat_eval_ctx->unit_mask_by_col[lane][word] |
                              pat_eval_ctx->worst_case_leave_units[word];
    }
    pat_eval_ctx->lane_penalty_bound[BOARD_HORIZONTAL_DIRECTION][lane] =
        pat_units_penalty_bound(pat_eval_ctx, row_bound_units);
    pat_eval_ctx->lane_penalty_bound[BOARD_VERTICAL_DIRECTION][lane] =
        pat_units_penalty_bound(pat_eval_ctx, col_bound_units);
  }
}

void pat_eval_context_set_kwg(PATEvalContext *pat_eval_ctx, const KWG *kwg) {
  pat_eval_ctx->kwg = kwg;
}

void pat_eval_context_load(PATEvalContext *pat_eval_ctx,
                           const PATWeights *weights, const Square *lanes,
                           const LetterDistribution *ld,
                           const Rack *player_rack,
                           uint32_t enabled_classes_mask,
                           int opponent_rack_size) {
  pat_require_prepared(weights);
  pat_eval_context_load_units(pat_eval_ctx, weights, lanes, ld, player_rack,
                              true, enabled_classes_mask, opponent_rack_size);
}

void pat_eval_context_load_all_units(PATEvalContext *pat_eval_ctx,
                                     const PATWeights *weights,
                                     const Square *lanes,
                                     const LetterDistribution *ld,
                                     const Rack *player_rack,
                                     int opponent_rack_size) {
  pat_eval_context_load_units(pat_eval_ctx, weights, lanes, ld, player_rack,
                              false, PAT_CLASS_MASK_ALL, opponent_rack_size);
}

// Returns the bitset of scan units the move can affect (see the
// unit_mask_by_row comment in the header).
static inline void pat_move_affected_units(const PATEvalContext *pat_eval_ctx,
                                           int row_start, int row_end,
                                           int col_start, int col_end,
                                           uint64_t *affected_units) {
  uint64_t row_units[PAT_MASK_WORDS];
  uint64_t col_units[PAT_MASK_WORDS];
  pat_mask_clear(row_units);
  pat_mask_clear(col_units);
  for (int row = row_start; row <= row_end; row++) {
    pat_mask_or_into(row_units, pat_eval_ctx->unit_mask_by_row[row]);
  }
  for (int col = col_start; col <= col_end; col++) {
    pat_mask_or_into(col_units, pat_eval_ctx->unit_mask_by_col[col]);
  }
  for (int word = 0; word < PAT_MASK_WORDS; word++) {
    affected_units[word] = row_units[word] & col_units[word];
  }
}

// Bit L (L != BLANK_MACHINE_LETTER) is set when the leave holds at least
// one unblanked tile of machine letter L; *has_blank_out reports whether
// the leave holds a blank, which could stand in for whatever letter a
// hook needs. A NULL leave (a caller with no leave to offer) reports an
// empty mask and no blank, which is exactly "this move's own leave helps
// nothing" -- safe by construction, not a special case below.
static uint64_t pat_leave_letter_mask(const Rack *leave, bool *has_blank_out) {
  *has_blank_out = false;
  uint64_t mask = 0;
  if (!leave) {
    return 0;
  }
  const int dist_size = rack_get_dist_size(leave);
  for (int ml = 0; ml < dist_size; ml++) {
    if (rack_get_letter(leave, ml) == 0) {
      continue;
    }
    if (ml == BLANK_MACHINE_LETTER) {
      *has_blank_out = true;
    } else {
      mask |= (uint64_t)1 << ml;
    }
  }
  return mask;
}

// Every unit reachable through some letter in letter_mask, via the reverse
// index (see units_by_hook_letter); every unit with any live route at all
// when has_blank, since a blank stands in for whatever letter a hook needs.
static void pat_units_from_letter_mask(const PATEvalContext *pat_eval_ctx,
                                       uint64_t letter_mask, bool has_blank,
                                       uint64_t *units_out) {
  pat_mask_clear(units_out);
  uint64_t remaining_letters = letter_mask;
  while (remaining_letters) {
    const int machine_letter = pat_ctz(remaining_letters);
    remaining_letters &= remaining_letters - 1;
    pat_mask_or_into(units_out,
                     pat_eval_ctx->units_by_hook_letter[machine_letter]);
  }
  if (has_blank) {
    for (int machine_letter = 1; machine_letter < MAX_ALPHABET_SIZE;
         machine_letter++) {
      pat_mask_or_into(units_out,
                       pat_eval_ctx->units_by_hook_letter[machine_letter]);
    }
  }
}

// Units the move's own leave could exploit itself (see unit_hook_letters),
// independent of whether its placement geometrically touches them. Empty
// whenever the file carries no discount, so that case matches every
// earlier file's behavior exactly -- byte for byte, not just numerically.
static void pat_leave_affected_units(const PATEvalContext *pat_eval_ctx,
                                     const Rack *leave,
                                     uint64_t *leave_units_out) {
  pat_mask_clear(leave_units_out);
  if (pat_eval_ctx->weights->own_asset_discount <= 0.0) {
    return;
  }
  bool leave_has_blank = false;
  const uint64_t leave_mask = pat_leave_letter_mask(leave, &leave_has_blank);
  pat_units_from_letter_mask(pat_eval_ctx, leave_mask, leave_has_blank,
                             leave_units_out);
}

// Bounds pat_eval_move_penalty(move, leave) without rescanning: besides the
// units the move's placement can reach, the units its own leave could
// exploit (see pat_leave_affected_units) can also move all the way to 0 in
// the best case (own_asset_discount capping at 1), so both sets are zeroed
// for the bound exactly as pat_units_penalty_bound already zeros a
// geometrically reached unit.
//
// A NULL leave does NOT mean "assume no credit": that would make the bound
// too small when the move's actual (unknown to this call) leave would have
// qualified, which is the unsound direction for an upper bound -- the real
// pat_eval_move_penalty could then exceed what this claimed to bound.
// Every real move-generation caller has the actual leave in hand and passes
// it, so this only matters for a caller with none to offer; that caller
// gets pat_eval_ctx->worst_case_leave_units, the same conservative
// rack-wide superset lane_penalty_bound already uses for exactly this
// reason (every leave is a subset of the current starting rack).
Equity pat_eval_move_penalty_bound(const PATEvalContext *pat_eval_ctx,
                                   const Move *move, const Rack *leave) {
  if (!pat_eval_ctx || !pat_eval_ctx->weights) {
    return 0;
  }
  if (move_get_type(move) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
    return pat_eval_ctx->pre_penalty;
  }
  const bool vertical = board_is_dir_vertical(move_get_dir(move));
  const int row_start = move_get_row_start(move);
  const int col_start = move_get_col_start(move);
  const int tiles_length = move_get_tiles_length(move);
  const int row_end = vertical ? row_start + tiles_length - 1 : row_start;
  const int col_end = vertical ? col_start : col_start + tiles_length - 1;
  uint64_t affected_units[PAT_MASK_WORDS];
  pat_move_affected_units(pat_eval_ctx, row_start, row_end, col_start, col_end,
                          affected_units);
  uint64_t leave_units[PAT_MASK_WORDS];
  if (leave) {
    pat_leave_affected_units(pat_eval_ctx, leave, leave_units);
  } else {
    for (int word = 0; word < PAT_MASK_WORDS; word++) {
      leave_units[word] = pat_eval_ctx->worst_case_leave_units[word];
    }
  }
  uint64_t combined_units[PAT_MASK_WORDS];
  for (int word = 0; word < PAT_MASK_WORDS; word++) {
    combined_units[word] = affected_units[word] | leave_units[word];
  }
  return pat_units_penalty_bound(pat_eval_ctx, combined_units);
}

Equity pat_eval_move_penalty(const PATEvalContext *pat_eval_ctx,
                             const Move *move, const Rack *leave) {
  if (!pat_eval_ctx || !pat_eval_ctx->weights) {
    return 0;
  }
  if (move_get_type(move) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
    // Exchanges and passes leave the board unchanged, so their defense term
    // is exactly the position baseline. Including it keeps the comparison
    // against tile placements (whose term is baseline plus delta) fair.
    return pat_eval_ctx->pre_penalty;
  }
  const bool vertical = board_is_dir_vertical(move_get_dir(move));
  const int row_start = move_get_row_start(move);
  const int col_start = move_get_col_start(move);
  const int tiles_length = move_get_tiles_length(move);
  const int row_end = vertical ? row_start + tiles_length - 1 : row_start;
  const int col_end = vertical ? col_start : col_start + tiles_length - 1;
  uint64_t affected_units[PAT_MASK_WORDS];
  pat_move_affected_units(pat_eval_ctx, row_start, row_end, col_start, col_end,
                          affected_units);
  // Units the move's own leave could exploit itself, whether or not its
  // placement geometrically touches them (see unit_hook_letters). Empty
  // whenever the file carries no discount, so that case runs the exact
  // byte-for-byte loop every earlier file already ran. This uses the
  // baseline (pre-move) reverse index, which is exactly right for a unit
  // the move's placement never touches -- but a unit the placement DOES
  // touch may have gained or lost hook letters the move itself created or
  // destroyed, so that case is re-checked below against a fresh, overlay-
  // aware scan instead of trusting this baseline membership test.
  const double discount = pat_eval_ctx->weights->own_asset_discount;
  bool leave_has_blank = false;
  const uint64_t leave_mask =
      (discount > 0.0) ? pat_leave_letter_mask(leave, &leave_has_blank) : 0;
  uint64_t leave_units[PAT_MASK_WORDS];
  pat_leave_affected_units(pat_eval_ctx, leave, leave_units);
  uint64_t combined_units[PAT_MASK_WORDS];
  for (int word = 0; word < PAT_MASK_WORDS; word++) {
    combined_units[word] = affected_units[word] | leave_units[word];
  }
  if (pat_mask_is_empty(combined_units)) {
    return pat_eval_ctx->pre_penalty;
  }
  const PATMoveOverlay overlay = {
      .move = move,
      .row_start = row_start,
      .col_start = col_start,
      .row_end = row_end,
      .col_end = col_end,
      .vertical = vertical,
      .hook_flex = pat_eval_ctx->weights->hook_flex,
      .kwg =
          pat_eval_ctx->weights->exact_created_hooks ? pat_eval_ctx->kwg : NULL,
      .lanes = pat_eval_ctx->lanes,
  };
  // Rescan only the units the move can reach (geometrically, or through
  // its own leave) and combine the whole set: the units in neither set
  // keep exactly the penalty they were loaded with, so the sum is the
  // total moved by each reached unit's change and the worst is the lesser
  // of the reached units' new penalties and the first unreached unit in
  // penalty order.
  int64_t sum = pat_eval_ctx->total_unit_penalty;
  int64_t worst = 0;
  for (int word = 0; word < PAT_MASK_WORDS; word++) {
    uint64_t bits = combined_units[word];
    while (bits != 0) {
      const int unit_index = word * 64 + pat_ctz(bits);
      bits &= bits - 1;
      Equity penalty;
      bool qualifies_for_credit;
      if (pat_mask_test(affected_units, unit_index)) {
        int32_t overlay_features[PAT_NUM_FEATURES] = {0};
        int scan_dir = 0;
        int scan_lane = 0;
        uint64_t fresh_hook_letters = 0;
        // The same rack the training label was built against: whatever the
        // player holds now, not the leave this move would keep. Training
        // opens its observation after the mover has drawn back to full, so
        // scoring a candidate against its leave would call every hook
        // uncontested in proportion to how many tiles the move played,
        // which is a penalty on bingos and nothing to do with hooks.
        pat_scan_context_unit(pat_eval_ctx, unit_index, &overlay,
                              overlay_features, &scan_dir, &scan_lane, NULL,
                              NULL,
                              discount > 0.0 ? &fresh_hook_letters : NULL);
        penalty = pat_dot_ctx(pat_eval_ctx, overlay_features);
        // The move's own placement can create or destroy this unit's hook
        // letters (a newly hooked square, or covering one that existed at
        // baseline), so eligibility is re-checked against the fresh,
        // overlay-aware scan rather than trusted from the baseline reverse
        // index that built leave_units.
        qualifies_for_credit =
            discount > 0.0 &&
            (leave_has_blank ? (fresh_hook_letters != 0)
                             : (fresh_hook_letters & leave_mask) != 0);
      } else {
        // Reached only through the leave: the move's placement never
        // touched this unit, so its geometry (and hence its hook letters)
        // is exactly what was loaded, and the baseline membership test is
        // still correct.
        penalty = pat_eval_ctx->unit_penalty[unit_index];
        qualifies_for_credit = pat_mask_test(leave_units, unit_index);
      }
      if (qualifies_for_credit) {
        // p <= 0 and discount in [0, 1], so p * (1 - discount) always
        // lands in [p, 0]: the credit can shrink a penalty toward zero
        // but never past it, so no new shadow-pruning bound is needed.
        penalty = (Equity)lround((double)penalty * (1.0 - discount));
      }
      sum += penalty - pat_eval_ctx->unit_penalty[unit_index];
      if (penalty < worst) {
        worst = penalty;
      }
    }
  }
  for (int order_idx = 0; order_idx < pat_eval_ctx->num_units; order_idx++) {
    const int unit_index = pat_eval_ctx->units_by_penalty[order_idx];
    if (!pat_mask_test(combined_units, unit_index)) {
      if (pat_eval_ctx->unit_penalty[unit_index] < worst) {
        worst = pat_eval_ctx->unit_penalty[unit_index];
      }
      break;
    }
  }
  return pat_combine(worst, sum, pat_eval_ctx->weights->combine_gamma);
}

// The training row for a candidate move built the way the runtime term is
// built: the pre-move context's stored unit rows, with the units the move
// geometrically touches rescanned through the move overlay, combined by
// the same worst-unit / gamma rule pat_eval_move_penalty applies. A model
// fitted on these rows is fitted on exactly what it is later evaluated
// with (the overlay's approximations included), whereas
// pat_extract_features_combined on the post-move board measures the
// created hooks and floaters with their real cross and extension sets,
// which the runtime never sees. Units the context dropped as unweighted
// contribute nothing, so their classes cannot gain weight from rows built
// this way; own-asset credit is not applied (training runs with no
// discount). Non-placement moves get the baseline rows unchanged.
void pat_extract_move_features_combined(const PATEvalContext *pat_eval_ctx,
                                        const Move *move, double *features) {
  for (int feature_index = 0; feature_index < PAT_NUM_FEATURES;
       feature_index++) {
    features[feature_index] = 0.0;
  }
  if (!pat_eval_ctx || !pat_eval_ctx->weights || pat_eval_ctx->num_units == 0) {
    return;
  }
  const int num_units = pat_eval_ctx->num_units;
  uint64_t affected_units[PAT_MASK_WORDS];
  pat_mask_clear(affected_units);
  PATMoveOverlay overlay;
  const bool is_placement =
      move_get_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE;
  if (is_placement) {
    const bool vertical = board_is_dir_vertical(move_get_dir(move));
    const int row_start = move_get_row_start(move);
    const int col_start = move_get_col_start(move);
    const int tiles_length = move_get_tiles_length(move);
    const int row_end = vertical ? row_start + tiles_length - 1 : row_start;
    const int col_end = vertical ? col_start : col_start + tiles_length - 1;
    pat_move_affected_units(pat_eval_ctx, row_start, row_end, col_start,
                            col_end, affected_units);
    overlay.move = move;
    overlay.row_start = row_start;
    overlay.col_start = col_start;
    overlay.row_end = row_end;
    overlay.col_end = col_end;
    overlay.vertical = vertical;
    overlay.hook_flex = pat_eval_ctx->weights->hook_flex;
    overlay.kwg =
        pat_eval_ctx->weights->exact_created_hooks ? pat_eval_ctx->kwg : NULL;
    overlay.lanes = pat_eval_ctx->lanes;
  }
  int32_t unit_rows[PAT_MAX_SCAN_UNITS][PAT_NUM_FEATURES];
  int worst_unit = -1;
  Equity worst_penalty = 0;
  for (int unit_index = 0; unit_index < num_units; unit_index++) {
    int32_t *row = unit_rows[unit_index];
    Equity penalty;
    if (is_placement && pat_mask_test(affected_units, unit_index)) {
      memset(row, 0, sizeof(int32_t) * PAT_NUM_FEATURES);
      int scan_dir = 0;
      int scan_lane = 0;
      pat_scan_context_unit(pat_eval_ctx, unit_index, &overlay, row, &scan_dir,
                            &scan_lane, NULL, NULL, NULL);
      penalty = pat_dot_ctx(pat_eval_ctx, row);
    } else {
      memcpy(row, pat_eval_ctx->unit_features[unit_index],
             sizeof(int32_t) * PAT_NUM_FEATURES);
      penalty = pat_eval_ctx->unit_penalty[unit_index];
    }
    if (penalty < worst_penalty) {
      worst_penalty = penalty;
      worst_unit = unit_index;
    }
  }
  const double gamma =
      (worst_unit >= 0) ? pat_eval_ctx->weights->combine_gamma : 1.0;
  for (int unit_index = 0; unit_index < num_units; unit_index++) {
    for (int feature_index = 0; feature_index < PAT_NUM_FEATURES;
         feature_index++) {
      features[feature_index] +=
          gamma * (double)unit_rows[unit_index][feature_index];
    }
  }
  if (worst_unit >= 0) {
    for (int feature_index = 0; feature_index < PAT_NUM_FEATURES;
         feature_index++) {
      features[feature_index] +=
          (1.0 - gamma) * (double)unit_rows[worst_unit][feature_index];
    }
  }
}

void pat_extract_features_combined(const Square *lanes,
                                   const LetterDistribution *ld,
                                   const Rack *player_rack,
                                   const PATWeights *pat,
                                   int opponent_rack_size, double *features) {
  pat_require_prepared(pat);
  for (int feature_index = 0; feature_index < PAT_NUM_FEATURES;
       feature_index++) {
    features[feature_index] = 0.0;
  }
  uint8_t unseen_counts[MAX_ALPHABET_SIZE];
  pat_compute_unseen_counts(lanes, ld, player_rack, unseen_counts);
  uint8_t tws_rows[PAT_MAX_PREMIUM];
  uint8_t tws_cols[PAT_MAX_PREMIUM];
  uint8_t tws_classes[PAT_MAX_PREMIUM];
  const int num_tws = pat_find_tws(lanes, tws_rows, tws_cols, tws_classes);
  uint8_t dd_dirs[PAT_MAX_DD];
  uint8_t dd_lanes[PAT_MAX_DD];
  uint8_t dd_los[PAT_MAX_DD];
  uint8_t dd_his[PAT_MAX_DD];
  uint8_t dd_tiers[PAT_MAX_DD];
  const int num_dd =
      pat_find_dd(lanes, dd_dirs, dd_lanes, dd_los, dd_his, dd_tiers);
  const int num_units = num_tws * 2 + num_dd;

  int32_t unit_features[PAT_MAX_SCAN_UNITS][PAT_NUM_FEATURES];
  int worst_unit = -1;
  Equity worst_penalty = 0;
  for (int unit_index = 0; unit_index < num_units; unit_index++) {
    int32_t *row = unit_features[unit_index];
    memset(row, 0, sizeof(int32_t) * PAT_NUM_FEATURES);
    if (unit_index < num_tws * 2) {
      const int tws_idx = unit_index / 2;
      pat_scan_unit(lanes, ld, unseen_counts, pat, tws_rows[tws_idx],
                    tws_cols[tws_idx], tws_classes[tws_idx], unit_index % 2,
                    NULL, row, NULL, NULL, opponent_rack_size, NULL);
    } else {
      const int dd_idx = unit_index - num_tws * 2;
      pat_scan_dd_unit(lanes, unseen_counts, dd_dirs[dd_idx], dd_lanes[dd_idx],
                       dd_los[dd_idx], dd_his[dd_idx], dd_tiers[dd_idx], NULL,
                       row, NULL, NULL, opponent_rack_size, NULL);
    }
    const Equity penalty = pat_dot(pat, row);
    if (penalty < worst_penalty) {
      worst_penalty = penalty;
      worst_unit = unit_index;
    }
  }

  // Untrained weights rank every unit alike, so there is no worst one to
  // charge in full. Fall back to the sum, which makes the first generation
  // an ordinary fit and gives later ones something to rank with.
  const double gamma = (worst_unit >= 0) ? pat->combine_gamma : 1.0;
  for (int unit_index = 0; unit_index < num_units; unit_index++) {
    for (int feature_index = 0; feature_index < PAT_NUM_FEATURES;
         feature_index++) {
      features[feature_index] +=
          gamma * (double)unit_features[unit_index][feature_index];
    }
  }
  if (worst_unit >= 0) {
    for (int feature_index = 0; feature_index < PAT_NUM_FEATURES;
         feature_index++) {
      features[feature_index] +=
          (1.0 - gamma) * (double)unit_features[worst_unit][feature_index];
    }
  }
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
        mask |= 1u << premium_class;
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
    if (enabled_classes_mask & (1u << premium_class)) {
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
