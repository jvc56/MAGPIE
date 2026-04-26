// Linear outcome model: load + evaluate.

#include "outcome_model.h"

#include "../util/io_util.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OCM_MAGIC "OCM1"
#define OCM_MAGIC_LEN 4

// Canonical feature ordering. MUST match the field order in
// OutcomeFeatures and the CSV header in outcome_recorder.c.
static const char *const FEATURE_NAMES[OUTCOME_MODEL_NUM_FEATURES] = {
    "us_st_frac_playable",  "us_st_top1",     "us_st_top2",
    "opp_st_frac_playable", "opp_st_top1",    "opp_st_top2",
    "us_bingo_prob",        "opp_bingo_prob", "unplayed_blanks",
    "tiles_unseen",         "score_diff",     "us_leave_value",
};

const char *outcome_model_feature_name(int i) {
  if (i < 0 || i >= OUTCOME_MODEL_NUM_FEATURES) {
    return NULL;
  }
  return FEATURE_NAMES[i];
}

void outcome_features_to_array(const OutcomeFeatures *f, double *out) {
  out[0] = f->us_st_frac_playable;
  out[1] = f->us_st_top1;
  out[2] = f->us_st_top2;
  out[3] = f->opp_st_frac_playable;
  out[4] = f->opp_st_top1;
  out[5] = f->opp_st_top2;
  out[6] = f->us_bingo_prob;
  out[7] = f->opp_bingo_prob;
  out[8] = (double)f->unplayed_blanks;
  out[9] = (double)f->tiles_unseen;
  out[10] = (double)f->score_diff;
  out[11] = (double)f->us_leave_value;
}

OutcomeModel *outcome_model_create_from_file(const char *path,
                                             ErrorStack *error_stack) {
  FILE *fp = fopen(path, "rb");
  if (fp == NULL) {
    error_stack_push(
        error_stack, ERROR_STATUS_CONFIG_LOAD_GAME_DATA_MISSING,
        get_formatted_string("could not open outcome model file '%s'", path));
    return NULL;
  }

  char magic[OCM_MAGIC_LEN];
  if (fread(magic, 1, OCM_MAGIC_LEN, fp) != OCM_MAGIC_LEN ||
      memcmp(magic, OCM_MAGIC, OCM_MAGIC_LEN) != 0) {
    fclose(fp);
    error_stack_push(
        error_stack, ERROR_STATUS_CONFIG_LOAD_GAME_DATA_MISSING,
        get_formatted_string("outcome model file '%s' has invalid magic bytes "
                             "(expected '%s')",
                             path, OCM_MAGIC));
    return NULL;
  }

  uint32_t num_features = 0;
  if (fread(&num_features, sizeof(num_features), 1, fp) != 1) {
    fclose(fp);
    error_stack_push(
        error_stack, ERROR_STATUS_CONFIG_LOAD_GAME_DATA_MISSING,
        get_formatted_string(
            "outcome model file '%s' is truncated reading num_features", path));
    return NULL;
  }
  if (num_features != OUTCOME_MODEL_NUM_FEATURES) {
    fclose(fp);
    error_stack_push(
        error_stack, ERROR_STATUS_CONFIG_LOAD_GAME_DATA_MISSING,
        get_formatted_string(
            "outcome model file '%s' has num_features=%u, expected %d", path,
            (unsigned)num_features, OUTCOME_MODEL_NUM_FEATURES));
    return NULL;
  }

  OutcomeModel *model = malloc_or_die(sizeof(OutcomeModel));
  if (fread(&model->win_bias, sizeof(double), 1, fp) != 1 ||
      fread(model->win_weights, sizeof(double), OUTCOME_MODEL_NUM_FEATURES,
            fp) != OUTCOME_MODEL_NUM_FEATURES ||
      fread(&model->spread_bias, sizeof(double), 1, fp) != 1 ||
      fread(model->spread_weights, sizeof(double), OUTCOME_MODEL_NUM_FEATURES,
            fp) != OUTCOME_MODEL_NUM_FEATURES) {
    free(model);
    fclose(fp);
    error_stack_push(
        error_stack, ERROR_STATUS_CONFIG_LOAD_GAME_DATA_MISSING,
        get_formatted_string(
            "outcome model file '%s' is truncated reading weights", path));
    return NULL;
  }
  fclose(fp);
  return model;
}

void outcome_model_write_to_file(const OutcomeModel *model, const char *path,
                                 ErrorStack *error_stack) {
  FILE *fp = fopen(path, "wb");
  if (fp == NULL) {
    error_stack_push(
        error_stack, ERROR_STATUS_CONFIG_LOAD_GAME_DATA_MISSING,
        get_formatted_string("could not open outcome model file '%s' for write",
                             path));
    return;
  }
  fwrite_or_die(OCM_MAGIC, 1, OCM_MAGIC_LEN, fp, "ocm magic");
  const uint32_t n = OUTCOME_MODEL_NUM_FEATURES;
  fwrite_or_die(&n, sizeof(n), 1, fp, "ocm num_features");
  fwrite_or_die(&model->win_bias, sizeof(double), 1, fp, "ocm win_bias");
  fwrite_or_die(model->win_weights, sizeof(double), OUTCOME_MODEL_NUM_FEATURES,
                fp, "ocm win_weights");
  fwrite_or_die(&model->spread_bias, sizeof(double), 1, fp, "ocm spread_bias");
  fwrite_or_die(model->spread_weights, sizeof(double),
                OUTCOME_MODEL_NUM_FEATURES, fp, "ocm spread_weights");
  fclose(fp);
}

void outcome_model_destroy(OutcomeModel *model) { free(model); }

void outcome_model_eval(const OutcomeModel *model,
                        const OutcomeFeatures *features,
                        OutcomePrediction *out) {
  double x[OUTCOME_MODEL_NUM_FEATURES];
  outcome_features_to_array(features, x);
  double win_logit = model->win_bias;
  double spread = model->spread_bias;
  for (int i = 0; i < OUTCOME_MODEL_NUM_FEATURES; i++) {
    win_logit += model->win_weights[i] * x[i];
    spread += model->spread_weights[i] * x[i];
  }
  out->win_prob = 1.0 / (1.0 + exp(-win_logit));
  out->spread = spread;
}
