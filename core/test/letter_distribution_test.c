#include <assert.h>

#include "../src/config.h"

#include "superconfig.h"

void test_letter_distribution(SuperConfig *superconfig) {
  Config *config = get_nwl_config(superconfig);

  uint32_t previous_score;
  for (uint32_t i = 0; i < config->letter_distribution->size; i++) {
    int score_order_index = config->letter_distribution->score_order[i];
    if (i == 0) {
      previous_score = config->letter_distribution->scores[score_order_index];
    }
    assert(config->letter_distribution->scores[score_order_index] <=
           previous_score);
  }
}