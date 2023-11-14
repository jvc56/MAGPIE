#include <assert.h>
#include <stdio.h>

#include "../src/config.h"
#include "../src/letter_distribution.h"

#include "testconfig.h"

void test_letter_distribution(TestConfig *testconfig) {
  const Config *config = get_nwl_config(testconfig);

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

void test_str_to_machine_letters(TestConfig *testconfig) {
  const Config *config = get_nwl_config(testconfig);
  uint8_t mls[4];
  int num_mls =
      str_to_machine_letters(config->letter_distribution, "??", false, mls);
  assert(num_mls == 2);
  assert(mls[0] == 0);
  assert(mls[1] == 0);

  // english:
  uint8_t emls[20];
  num_mls =
      str_to_machine_letters(config->letter_distribution, "ABZ", false, emls);
  assert(num_mls == 3);
  assert(emls[0] == 1);
  assert(emls[1] == 2);
  assert(emls[2] == 26);

  // catalan:
  config = get_disc_config(testconfig);
  uint8_t cmls[20];
  num_mls = str_to_machine_letters(config->letter_distribution,
                                   "A[l·l]O[QU]IMI[qu]ES", false, cmls);
  assert(num_mls == 10);
  assert(cmls[0] == 1);
  assert(cmls[1] == get_blanked_machine_letter(13));
  assert(cmls[2] == 17);
  assert(cmls[3] == 19);
  assert(cmls[4] == 10);
  assert(cmls[5] == 14);
  assert(cmls[6] == 10);
  assert(cmls[7] == get_blanked_machine_letter(19));
  assert(cmls[8] == 6);
  assert(cmls[9] == 21);

  // Test consecutive multichar letters
  uint8_t cmls2[20];
  num_mls = str_to_machine_letters(config->letter_distribution,
                                   "[L·L]ES[QU][qu]A[QU][qu]", false, cmls2);
  assert(num_mls == 8);
  assert(cmls2[0] == 13);
  assert(cmls2[1] == 6);
  assert(cmls2[2] == 21);
  assert(cmls2[3] == 19);
  assert(cmls2[4] == get_blanked_machine_letter(19));
  assert(cmls2[5] == 1);
  assert(cmls2[6] == 19);
  assert(cmls2[7] == get_blanked_machine_letter(19));

  uint8_t imls[40];

  // Ensure allowing playthrough tiles works
  assert(str_to_machine_letters(config->letter_distribution, ".BDEF", true,
                                imls) == 5);
  assert(str_to_machine_letters(config->letter_distribution, ".B.D.E.F..", true,
                                imls) == 10);

  // Ensure invalid racks return -1
  assert(str_to_machine_letters(config->letter_distribution, "2", false,
                                imls) == -1);
  assert(str_to_machine_letters(config->letter_distribution, "ABC9EFG", false,
                                imls) == -1);
  assert(str_to_machine_letters(config->letter_distribution, "ABCD[AB]EGD",
                                true, imls) == -1);
  assert(str_to_machine_letters(config->letter_distribution, "ABCD[ABCEFGH]EGD",
                                true, imls) == -1);
  assert(str_to_machine_letters(config->letter_distribution, "AB.F", false,
                                imls) == -1);
  assert(str_to_machine_letters(config->letter_distribution, "BEHF.", false,
                                imls) == -1);
  assert(str_to_machine_letters(config->letter_distribution, ".BDEF", false,
                                imls) == -1);
}