#ifndef OUTCOME_MODEL_H
#define OUTCOME_MODEL_H

#include "../util/io_util.h"
#include "outcome_features.h"
#include <stdint.h>

// Linear outcome model: two regression heads sharing the same input
// feature vector.
//
//   win_logit = w_win . x + b_win
//   win_prob  = sigmoid(win_logit)
//   spread    = w_spread . x + b_spread     (millipoints, signed)
//
// File format (.ocm, big-endian — actually host-endian for now,
// MAGPIE's other binary formats are host-endian and we'll cross that
// bridge if it comes up):
//
//   bytes 0-3   : ASCII "OCM1"  (magic + version)
//   bytes 4-7   : uint32_t num_features  (sanity-check, must equal
//                 OUTCOME_MODEL_NUM_FEATURES at load time)
//   next 8      : double win_bias
//   next 8*N    : double win_weights[N]
//   next 8      : double spread_bias
//   next 8*N    : double spread_weights[N]
//
// Feature ordering matches the OutcomeFeatures struct and the autoplay
// CSV column order; see outcome_model_features_to_array.

#define OUTCOME_MODEL_NUM_FEATURES 12

typedef struct {
  double win_bias;
  double win_weights[OUTCOME_MODEL_NUM_FEATURES];
  double spread_bias;
  double spread_weights[OUTCOME_MODEL_NUM_FEATURES];
} OutcomeModel;

typedef struct {
  double win_prob; // in [0, 1]
  double spread;   // millipoints, signed
} OutcomePrediction;

// Returns the canonical CSV column name for feature index i (0..N-1),
// or NULL if i is out of range. Static storage; do not free.
const char *outcome_model_feature_name(int i);

// Fills out[OUTCOME_MODEL_NUM_FEATURES] from features in the canonical
// order. Used by both eval and any test/debug code that needs to
// inspect the feature vector.
void outcome_features_to_array(const OutcomeFeatures *features, double *out);

// Loads an .ocm file. On error pushes onto error_stack and returns
// NULL.
OutcomeModel *outcome_model_create_from_file(const char *path,
                                             ErrorStack *error_stack);

// Writes an .ocm file from the supplied weights. On error pushes onto
// error_stack and leaves the file in an indeterminate state.
void outcome_model_write_to_file(const OutcomeModel *model, const char *path,
                                 ErrorStack *error_stack);

void outcome_model_destroy(OutcomeModel *model);

// O(N) dot products + one sigmoid.
void outcome_model_eval(const OutcomeModel *model,
                        const OutcomeFeatures *features,
                        OutcomePrediction *out);

#endif
