// Tests for outcome_model: file round-trip and eval correctness.

#include "outcome_model_test.h"

#include "../src/impl/outcome_features.h"
#include "../src/impl/outcome_model.h"
#include "../src/util/io_util.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_round_trip(void) {
  OutcomeModel m_in;
  m_in.win_bias = 0.123;
  m_in.spread_bias = -2500.0;
  for (int i = 0; i < OUTCOME_MODEL_NUM_FEATURES; i++) {
    m_in.win_weights[i] = 0.01 * (double)(i + 1);
    m_in.spread_weights[i] = 100.0 - 5.0 * (double)i;
  }

  const char *path = "/tmp/test_round_trip.ocm";
  ErrorStack *err = error_stack_create();
  outcome_model_write_to_file(&m_in, path, err);
  assert(error_stack_is_empty(err));

  OutcomeModel *m_out = outcome_model_create_from_file(path, err);
  assert(error_stack_is_empty(err));
  assert(m_out != NULL);

  assert(m_in.win_bias == m_out->win_bias);
  assert(m_in.spread_bias == m_out->spread_bias);
  for (int i = 0; i < OUTCOME_MODEL_NUM_FEATURES; i++) {
    assert(m_in.win_weights[i] == m_out->win_weights[i]);
    assert(m_in.spread_weights[i] == m_out->spread_weights[i]);
  }

  outcome_model_destroy(m_out);
  error_stack_destroy(err);
}

static void test_eval(void) {
  // Hand-construct a model with all-zero weights except win_bias and
  // spread_bias, then eval and check the trivial predictions.
  OutcomeModel m;
  memset(&m, 0, sizeof(m));
  m.win_bias = 0.0;
  m.spread_bias = 0.0;

  OutcomeFeatures f;
  memset(&f, 0, sizeof(f));
  OutcomePrediction p;
  outcome_model_eval(&m, &f, &p);
  // All zeros → win_logit=0 → win_prob=0.5. spread=0.
  assert(fabs(p.win_prob - 0.5) < 1e-12);
  assert(p.spread == 0.0);

  // Now give win_bias = +log(3) so win_prob = 3/4, and spread_bias =
  // 12345.
  m.win_bias = log(3.0);
  m.spread_bias = 12345.0;
  outcome_model_eval(&m, &f, &p);
  assert(fabs(p.win_prob - 0.75) < 1e-12);
  assert(p.spread == 12345.0);

  // Set a specific feature weight and verify the dot product.
  // score_diff is index 10. Weight = 0.001 / point. spread_diff = 50000
  // (50 points) → contribution to win_logit = 50.
  m.win_bias = 0.0;
  m.win_weights[10] = 0.001;
  f.score_diff = 50000;
  outcome_model_eval(&m, &f, &p);
  // win_logit = 50, win_prob ≈ 1.0
  assert(fabs(p.win_prob - 1.0) < 1e-12);
}

void test_outcome_model(void) {
  test_round_trip();
  test_eval();
}
