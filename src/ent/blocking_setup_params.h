#ifndef BLOCKING_SETUP_PARAMS_H
#define BLOCKING_SETUP_PARAMS_H

#include "../util/io_util.h"
#include "equity.h"
#include <stdbool.h>

// Per-lexicon blocking/setup parameters: how a lexicon's pass-relative
// blocking and setup deltas (see impl/blocking_setup.h) become equity
// adjustments, and the teacher settings the weights were fitted under.
//
// File: data/strategy/<name>.bsp, a text file of "key,value" rows after a
// "magpie_bsp_v<version>" header. Lines starting with '#' are comments.
//
//   lexicon,<name>            lexicon the weights were fitted for (required)
//   model_version,<text>      free-form version tag (required)
//   objective,<text>          what the weights optimize, e.g. sim_admission
//                             or static_choice (required)
//   teacher_racks,<int>       sampled opponent racks per position
//   teacher_partition,<0|1>   disjoint rack deals
//   teacher_condition_draws,<0|1>
//   teacher_followup_draws,<int>
//   teacher_value,<score|equity_score|equity|equity_reply>   (see
//   blocking_setup_value_t) blocking_weight,<real>    default weights
//   (required) setup_weight,<real>
//   bin,<min_bag>,<max_bag>,<min_lead>,<max_lead>,<blocking_weight>,
//       <setup_weight>        optional conditional weights, inclusive ranges;
//                             the first matching bin wins, else the defaults
//   provenance,<text>         repeatable free text (seeds, hashes, commands)
//
// Units: deltas are score points, and a weight is equity points per delta
// point, so a move's adjusted value is
//   equity + double_to_equity(blocking_weight * blocking_delta
//                             + setup_weight * setup_delta).
// Bag is tiles in the bag; lead is the on-turn player's score minus the
// opponent's, in points.

#define BLOCKING_SETUP_PARAMS_MAGIC_PREFIX "magpie_bsp_v"
enum {
  BLOCKING_SETUP_PARAMS_VERSION = 1,
  BLOCKING_SETUP_PARAMS_MAX_BINS = 64,
  BLOCKING_SETUP_PARAMS_MAX_TEXT = 256,
};

typedef struct BlockingSetupParams BlockingSetupParams;

// How the teacher picks and values the opponent's reply and our follow-up
// (the teacher_value row): by score and in points (score, the default), by
// static equity (score plus leave, exchanges allowed, as a static player
// chooses) but valued in points (equity_score), by and in static equity
// (equity), or the reply by and in static equity and the follow-up by score
// and in points (equity_reply), so a setup does not count our leave again.
typedef enum {
  BLOCKING_SETUP_VALUE_SCORE,
  BLOCKING_SETUP_VALUE_EQUITY_SCORE,
  BLOCKING_SETUP_VALUE_EQUITY,
  BLOCKING_SETUP_VALUE_EQUITY_REPLY,
} blocking_setup_value_t;

// Picking only the best candidate needs less than measuring all of them.
// A race (blocking_setup_checker_choose) measures the candidates in batches
// of racks, all on the same racks, and after each batch drops any candidate
// whose adjusted value trails the leader's by more than z standard errors
// of their paired per-rack difference. Adjusted value = base equity +
// blocking_weight * blocking_delta + setup_weight * setup_delta. With z <= 0
// nothing is dropped and the result is exactly the argmax of full
// measurements; with z > 0 it is a different, faster policy whose choices
// can differ.
enum {
  BLOCKING_SETUP_RACE_BATCH = 8,
};

typedef struct BlockingSetupRaceSettings {
  // Racks per batch; 0 for BLOCKING_SETUP_RACE_BATCH.
  int batch_racks;
  // Racks measured before the first elimination.
  int min_racks;
  // Elimination threshold in standard errors; <= 0 disables elimination.
  double z;
} BlockingSetupRaceSettings;

// A static-ish move policy (blocking_setup_policy_choose): the on-turn
// player's top universe no-PAT static placements and up to exchange_quota
// exchanges within exchange_margin of the top move, raced (or, with
// race.z <= 0, measured in full) on num_racks sampled racks, choosing the
// highest static equity plus the params' weighted deltas. Positions with an
// empty bag, or whose weights (by bag and lead) are both zero, get the plain
// no-PAT static choice.
typedef struct BlockingSetupPolicySettings {
  // Not owned; must outlive every policy made from these settings.
  const BlockingSetupParams *params;
  // 0 for the params' teacher_racks.
  int num_racks;
  int universe;
  int exchange_quota;
  Equity exchange_margin;
  BlockingSetupRaceSettings race;
} BlockingSetupPolicySettings;

// Reads data/strategy/<name>.bsp from the first data path that has it.
// Missing or malformed files push an error and return NULL.
BlockingSetupParams *blocking_setup_params_create(const char *data_paths,
                                                  const char *name,
                                                  ErrorStack *error_stack);
// Parses file contents; name labels errors.
BlockingSetupParams *
blocking_setup_params_create_from_string(const char *name, const char *contents,
                                         ErrorStack *error_stack);
void blocking_setup_params_destroy(BlockingSetupParams *params);

const char *blocking_setup_params_get_name(const BlockingSetupParams *params);
const char *
blocking_setup_params_get_lexicon(const BlockingSetupParams *params);
const char *
blocking_setup_params_get_model_version(const BlockingSetupParams *params);
const char *
blocking_setup_params_get_objective(const BlockingSetupParams *params);
int blocking_setup_params_get_teacher_racks(const BlockingSetupParams *params);
bool blocking_setup_params_get_teacher_partition(
    const BlockingSetupParams *params);
bool blocking_setup_params_get_teacher_condition_draws(
    const BlockingSetupParams *params);
int blocking_setup_params_get_teacher_followup_draws(
    const BlockingSetupParams *params);
int blocking_setup_params_get_num_bins(const BlockingSetupParams *params);
blocking_setup_value_t
blocking_setup_params_get_teacher_value(const BlockingSetupParams *params);

// The weights for a position with bag tiles in the bag and the on-turn
// player leading by lead points.
void blocking_setup_params_get_weights(const BlockingSetupParams *params,
                                       int bag, int lead,
                                       double *blocking_weight,
                                       double *setup_weight);

#endif
