#include <assert.h>
#include <stdio.h>

#include "../src/ent/config.h"
#include "../src/ent/letter_distribution.h"

#include "testconfig.h"

void test_letter_distribution(TestConfig *testconfig) {
  const Config *config = get_nwl_config(testconfig);
  const LetterDistribution *ld = config_get_letter_distribution(config);
  int ld_size = letter_distribution_get_size(ld);

  uint32_t previous_score;
  for (uint32_t i = 0; i < ld_size; i++) {
    int score_order_index = letter_distribution_get_score_order(ld, i);
    if (i == 0) {
      previous_score = letter_distribution_get_score(ld, BLANK_MACHINE_LETTER);
    }
    assert(letter_distribution_get_score(ld, score_order_index) <=
           previous_score);
  }
}

void test_str_to_machine_letters(TestConfig *testconfig) {
  const Config *nwl_config = get_nwl_config(testconfig);
  const LetterDistribution *english_ld =
      config_get_letter_distribution(nwl_config);
  const Config *disc_config = get_disc_config(testconfig);
  const LetterDistribution *catalan_ld =
      config_get_letter_distribution(disc_config);
  const Config *osps_config = get_osps_config(testconfig);
  const LetterDistribution *polish_ld =
      config_get_letter_distribution(osps_config);

  uint8_t mls[4];
  int num_mls = str_to_machine_letters(english_ld, "??", false, mls, 4);
  assert(num_mls == 2);
  assert(mls[0] == 0);
  assert(mls[1] == 0);

  // english:
  uint8_t emls[20];
  num_mls = str_to_machine_letters(english_ld, "ABZ", false, emls, 20);
  assert(num_mls == 3);
  assert(emls[0] == 1);
  assert(emls[1] == 2);
  assert(emls[2] == 26);

  // catalan:
  uint8_t cmls[20];
  num_mls = str_to_machine_letters(catalan_ld, "A[l·l]O[QU]IMI[qu]ES", false,
                                   cmls, 20);
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
  num_mls = str_to_machine_letters(catalan_ld, "[L·L]ES[QU][qu]A[QU][qu]",
                                   false, cmls2, 20);
  assert(num_mls == 8);
  assert(cmls2[0] == 13);
  assert(cmls2[1] == 6);
  assert(cmls2[2] == 21);
  assert(cmls2[3] == 19);
  assert(cmls2[4] == get_blanked_machine_letter(19));
  assert(cmls2[5] == 1);
  assert(cmls2[6] == 19);
  assert(cmls2[7] == get_blanked_machine_letter(19));

  // Polish
  uint8_t pmls[20];
  num_mls = str_to_machine_letters(polish_ld, "FGÓIŁHAŃ", false, pmls, 20);
  assert(num_mls == 8);
  assert(pmls[0] == 9);
  assert(pmls[1] == 10);
  assert(pmls[2] == 21);
  assert(pmls[3] == 12);
  assert(pmls[4] == 16);
  assert(pmls[5] == 11);
  assert(pmls[6] == 1);
  assert(pmls[7] == 19);

  num_mls = str_to_machine_letters(polish_ld, "ŻŚŻGÓI", false, pmls, 20);
  assert(num_mls == 6);
  assert(pmls[0] == 32);
  assert(pmls[1] == 25);
  assert(pmls[2] == 32);
  assert(pmls[3] == 10);
  assert(pmls[4] == 21);
  assert(pmls[5] == 12);

  uint8_t imls[40];

  // Ensure allowing playthrough tiles works
  assert(str_to_machine_letters(english_ld, ".BDEF", true, imls, 40) == 5);
  assert(str_to_machine_letters(english_ld, ".B.D.E.F..", true, imls, 40) ==
         10);

  // Ensure invalid racks return -1

  // Invalid multichar strings
  assert(str_to_machine_letters(polish_ld, "ŻŚ[Ż]GÓI", true, imls, 40) == -1);
  assert(str_to_machine_letters(polish_ld, "Ż[ŚŻ]GÓI", true, imls, 40) == -1);
  assert(str_to_machine_letters(catalan_ld, "A[ES]A", true, imls, 40) == -1);
  assert(str_to_machine_letters(catalan_ld, "ABCD[ABCEFGH]EGD", true, imls,
                                40) == -1);
  assert(str_to_machine_letters(catalan_ld, "[", true, imls, 40) == -1);
  assert(str_to_machine_letters(catalan_ld, "]", true, imls, 40) == -1);
  assert(str_to_machine_letters(catalan_ld, "[]", true, imls, 40) == -1);
  assert(str_to_machine_letters(catalan_ld, "ABC[DE", true, imls, 40) == -1);
  assert(str_to_machine_letters(catalan_ld, "ABCD]E", true, imls, 40) == -1);
  assert(str_to_machine_letters(catalan_ld, "ABC[D]E", true, imls, 40) == -1);
  assert(str_to_machine_letters(catalan_ld, "[L·L[QU]]ES", true, imls, 40) ==
         -1);
  assert(str_to_machine_letters(catalan_ld, "[L·L]ES[QU][qu][]A[QU][qu]", true,
                                imls, 40) == -1);
  assert(str_to_machine_letters(catalan_ld, "[L·L]ES[QU][qu]A[QU][qu", true,
                                imls, 40) == -1);

  // Invalid letters
  assert(str_to_machine_letters(english_ld, "2", true, imls, 40) == -1);
  assert(str_to_machine_letters(english_ld, "ABC9EFG", true, imls, 40) == -1);

  // Play through not allowed
  assert(str_to_machine_letters(english_ld, "AB.F", false, imls, 40) == -1);
  assert(str_to_machine_letters(english_ld, "BEHF.", false, imls, 40) == -1);
  assert(str_to_machine_letters(english_ld, ".BDEF", false, imls, 40) == -1);
}