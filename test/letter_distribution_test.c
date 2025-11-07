#include "../src/def/letter_distribution_defs.h"
#include "../src/ent/equity.h"
#include "../src/ent/letter_distribution.h"
#include "../src/impl/config.h"
#include "test_util.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

void test_ld_score_order(void) {
  Config *config = config_create_or_die(
      "set -lex NWL20 -s1 score -s2 score -r1 all -r2 all -numplays 1");
  const LetterDistribution *ld = config_get_ld(config);
  int ld_size = ld_get_size(ld);

  Equity previous_score = 100000;
  for (int i = 0; i < ld_size; i++) {
    int score_order_index = ld_get_score_order(ld, i);
    assert(ld_get_score(ld, score_order_index) <= previous_score);
    previous_score = ld_get_score(ld, score_order_index);
  }
  config_destroy(config);
}

void test_ld_str_to_mls(void) {
  Config *nwl_config = config_create_or_die(
      "set -lex NWL20 -s1 score -s2 score -r1 all -r2 all -numplays 1");
  // Test case insensitive letter distribution loading
  load_and_exec_config_or_die(nwl_config, "set -ld EnGlIsH");
  const LetterDistribution *english_ld = config_get_ld(nwl_config);

  Config *disc_config = config_create_or_die(
      "set -lex DISC2 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  const LetterDistribution *catalan_ld = config_get_ld(disc_config);

  Config *osps_config =
      config_create_or_die("set -lex OSPS49 -wmp false -s1 equity -s2 equity "
                           "-r1 all -r2 all -numplays 1");
  const LetterDistribution *polish_ld = config_get_ld(osps_config);

  MachineLetter mls[4];
  int num_mls = ld_str_to_mls(english_ld, "??", false, mls, 4);
  assert(num_mls == 2);
  assert(mls[0] == 0);
  assert(mls[1] == 0);

  // english:
  MachineLetter emls[20];
  num_mls = ld_str_to_mls(english_ld, "ABZ", false, emls, 20);
  assert(num_mls == 3);
  assert(emls[0] == 1);
  assert(emls[1] == 2);
  assert(emls[2] == 26);

  // catalan:
  MachineLetter cmls[20];
  num_mls = ld_str_to_mls(catalan_ld, "A[l·l]O[QU]IMI[qu]ES", false, cmls, 20);
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
  MachineLetter cmls2[20];
  num_mls =
      ld_str_to_mls(catalan_ld, "[L·L]ES[QU][qu]A[QU][qu]", false, cmls2, 20);
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
  MachineLetter pmls[20];
  num_mls = ld_str_to_mls(polish_ld, "FGÓIŁHAŃ", false, pmls, 20);
  assert(num_mls == 8);
  assert(pmls[0] == 9);
  assert(pmls[1] == 10);
  assert(pmls[2] == 21);
  assert(pmls[3] == 12);
  assert(pmls[4] == 16);
  assert(pmls[5] == 11);
  assert(pmls[6] == 1);
  assert(pmls[7] == 19);

  num_mls = ld_str_to_mls(polish_ld, "ŻŚŻGÓI", false, pmls, 20);
  assert(num_mls == 6);
  assert(pmls[0] == 32);
  assert(pmls[1] == 25);
  assert(pmls[2] == 32);
  assert(pmls[3] == 10);
  assert(pmls[4] == 21);
  assert(pmls[5] == 12);

  MachineLetter imls[40];

  // Ensure allowing playthrough tiles works
  assert(ld_str_to_mls(english_ld, ".BDEF", true, imls, 40) == 5);
  assert(ld_str_to_mls(english_ld, ".B.D.E.F..", true, imls, 40) == 10);
  assert(ld_str_to_mls(english_ld, "$BDEF", true, imls, 40) == 5);
  assert(ld_str_to_mls(english_ld, "$B$D$E$F$$", true, imls, 40) == 10);
  // Ensure invalid racks return -1

  // Invalid multichar strings
  assert(ld_str_to_mls(polish_ld, "ŻŚ[Ż]GÓI", true, imls, 40) == -1);
  assert(ld_str_to_mls(polish_ld, "Ż[ŚŻ]GÓI", true, imls, 40) == -1);
  assert(ld_str_to_mls(catalan_ld, "A[ES]A", true, imls, 40) == -1);
  assert(ld_str_to_mls(catalan_ld, "ABCD[ABCEFGH]EGD", true, imls, 40) == -1);
  assert(ld_str_to_mls(catalan_ld, "[", true, imls, 40) == -1);
  assert(ld_str_to_mls(catalan_ld, "]", true, imls, 40) == -1);
  assert(ld_str_to_mls(catalan_ld, "[]", true, imls, 40) == -1);
  assert(ld_str_to_mls(catalan_ld, "ABC[DE", true, imls, 40) == -1);
  assert(ld_str_to_mls(catalan_ld, "ABCD]E", true, imls, 40) == -1);
  assert(ld_str_to_mls(catalan_ld, "ABC[D]E", true, imls, 40) == -1);
  assert(ld_str_to_mls(catalan_ld, "[L·L[QU]]ES", true, imls, 40) == -1);
  assert(ld_str_to_mls(catalan_ld, "[L·L]ES[QU][qu][]A[QU][qu]", true, imls,
                       40) == -1);
  assert(ld_str_to_mls(catalan_ld, "[L·L]ES[QU][qu]A[QU][qu", true, imls, 40) ==
         -1);

  // Invalid letters
  assert(ld_str_to_mls(english_ld, "2", true, imls, 40) == -1);
  assert(ld_str_to_mls(english_ld, "ABC9EFG", true, imls, 40) == -1);

  // Play through not allowed
  assert(ld_str_to_mls(english_ld, "AB.F", false, imls, 40) == -1);
  assert(ld_str_to_mls(english_ld, "BEHF.", false, imls, 40) == -1);
  assert(ld_str_to_mls(english_ld, ".BDEF", false, imls, 40) == -1);

  config_destroy(nwl_config);
  config_destroy(osps_config);
  config_destroy(disc_config);
}

void test_ld(void) {
  test_ld_score_order();
  test_ld_str_to_mls();
}