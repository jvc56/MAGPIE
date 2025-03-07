#include <assert.h>
#include <stdlib.h>

#include "../../src/ent/random_variable.h"

#include "../../src/impl/bai.h"
#include "../../src/impl/bai_sampling_rule.h"

void test_bai_track_and_stop(void) {
  int num_rvs = 2;
  RandomVariablesArgs rv_args = {
      .type = RANDOM_VARIABLES_NORMAL,
      // FIXME: num_rvs is repeated and it probably shouldn't be
      .num_rvs = num_rvs,
      .means_and_stdevs = (double[]){10.0, 3.0, 1.0, 2.0},
      .rng_args =
          &(RNGArgs){
              .filename = NULL,
              .seed = 1,
              .num_rngs = num_rvs,
          },
  };

  RandomVariables *rvs = rvs_create(&rv_args);
  int result = bai(BAI_SAMPLING_RULE_TRACK_AND_STOP, rvs, 0.05);
  assert(result == 0);
  rvs_destroy(rvs);
}

void test_bai(void) { test_bai_track_and_stop(); }
