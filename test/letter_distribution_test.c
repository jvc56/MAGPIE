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

void test_fast_str_to_mls(void) {
  Config *nwl_config = config_create_or_die(
      "set -lex NWL20 -s1 score -s2 score -r1 all -r2 all -numplays 1");
  load_and_exec_config_or_die(nwl_config, "set -ld EnGlIsH");
  const LetterDistribution *english_ld = config_get_ld(nwl_config);

  Config *disc_config = config_create_or_die(
      "set -lex DISC2 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  const LetterDistribution *catalan_ld = config_get_ld(disc_config);

  Config *osps_config =
      config_create_or_die("set -lex OSPS49 -wmp false -s1 equity -s2 equity "
                           "-r1 all -r2 all -numplays 1");
  const LetterDistribution *polish_ld = config_get_ld(osps_config);

  // Initialize fast converters
  FastStringConverter fc_english, fc_catalan, fc_polish;
  fast_converter_init(&fc_english, english_ld);
  fast_converter_init(&fc_catalan, catalan_ld);
  fast_converter_init(&fc_polish, polish_ld);

  // Verify ASCII lookup table is populated correctly for English
  // A=1, B=2, ..., Z=26, ?=0
  assert(fc_english.ascii_to_ml['A'] == 1);
  assert(fc_english.ascii_to_ml['B'] == 2);
  assert(fc_english.ascii_to_ml['Z'] == 26);
  assert(fc_english.ascii_to_ml['?'] == 0);
  // Lowercase should map to blanked letters
  assert(fc_english.ascii_to_ml['a'] == get_blanked_machine_letter(1));
  assert(fc_english.ascii_to_ml['z'] == get_blanked_machine_letter(26));
  // Invalid characters should be INVALID_LETTER
  assert(fc_english.ascii_to_ml['1'] == INVALID_LETTER);
  assert(fc_english.ascii_to_ml['@'] == INVALID_LETTER);

  MachineLetter mls[4];
  MachineLetter mls_slow[4];
  int num_mls = fast_str_to_mls(&fc_english, "??", false, mls, 4);
  int num_mls_slow = ld_str_to_mls(english_ld, "??", false, mls_slow, 4);
  assert(num_mls == num_mls_slow);
  assert(num_mls == 2);
  assert(mls[0] == mls_slow[0]);
  assert(mls[1] == mls_slow[1]);

  // English words - compare fast vs slow
  MachineLetter emls[20], emls_slow[20];
  num_mls = fast_str_to_mls(&fc_english, "ABZ", false, emls, 20);
  num_mls_slow = ld_str_to_mls(english_ld, "ABZ", false, emls_slow, 20);
  assert(num_mls == num_mls_slow);
  assert(num_mls == 3);
  for (int i = 0; i < num_mls; i++) {
    assert(emls[i] == emls_slow[i]);
  }

  // Test blanked letters (lowercase)
  num_mls = fast_str_to_mls(&fc_english, "AbZ", false, emls, 20);
  num_mls_slow = ld_str_to_mls(english_ld, "AbZ", false, emls_slow, 20);
  assert(num_mls == num_mls_slow);
  assert(num_mls == 3);
  for (int i = 0; i < num_mls; i++) {
    assert(emls[i] == emls_slow[i]);
  }

  // Catalan with multichar letters - fast path falls back to slow for non-ASCII
  MachineLetter cmls[20], cmls_slow[20];
  num_mls =
      fast_str_to_mls(&fc_catalan, "A[l·l]O[QU]IMI[qu]ES", false, cmls, 20);
  num_mls_slow =
      ld_str_to_mls(catalan_ld, "A[l·l]O[QU]IMI[qu]ES", false, cmls_slow, 20);
  assert(num_mls == num_mls_slow);
  assert(num_mls == 10);
  for (int i = 0; i < num_mls; i++) {
    assert(cmls[i] == cmls_slow[i]);
  }

  // Test consecutive multichar letters
  MachineLetter cmls2[20], cmls2_slow[20];
  num_mls = fast_str_to_mls(&fc_catalan, "[L·L]ES[QU][qu]A[QU][qu]", false,
                            cmls2, 20);
  num_mls_slow = ld_str_to_mls(catalan_ld, "[L·L]ES[QU][qu]A[QU][qu]", false,
                               cmls2_slow, 20);
  assert(num_mls == num_mls_slow);
  assert(num_mls == 8);
  for (int i = 0; i < num_mls; i++) {
    assert(cmls2[i] == cmls2_slow[i]);
  }

  // Polish with UTF-8 characters
  MachineLetter pmls[20], pmls_slow[20];
  num_mls = fast_str_to_mls(&fc_polish, "FGÓIŁHAŃ", false, pmls, 20);
  num_mls_slow = ld_str_to_mls(polish_ld, "FGÓIŁHAŃ", false, pmls_slow, 20);
  assert(num_mls == num_mls_slow);
  assert(num_mls == 8);
  for (int i = 0; i < num_mls; i++) {
    assert(pmls[i] == pmls_slow[i]);
  }

  num_mls = fast_str_to_mls(&fc_polish, "ŻŚŻGÓI", false, pmls, 20);
  num_mls_slow = ld_str_to_mls(polish_ld, "ŻŚŻGÓI", false, pmls_slow, 20);
  assert(num_mls == num_mls_slow);
  assert(num_mls == 6);
  for (int i = 0; i < num_mls; i++) {
    assert(pmls[i] == pmls_slow[i]);
  }

  MachineLetter imls[40], imls_slow[40];

  // Playthrough tiles
  num_mls = fast_str_to_mls(&fc_english, ".BDEF", true, imls, 40);
  num_mls_slow = ld_str_to_mls(english_ld, ".BDEF", true, imls_slow, 40);
  assert(num_mls == num_mls_slow);
  assert(num_mls == 5);

  num_mls = fast_str_to_mls(&fc_english, ".B.D.E.F..", true, imls, 40);
  num_mls_slow = ld_str_to_mls(english_ld, ".B.D.E.F..", true, imls_slow, 40);
  assert(num_mls == num_mls_slow);
  assert(num_mls == 10);

  num_mls = fast_str_to_mls(&fc_english, "$BDEF", true, imls, 40);
  num_mls_slow = ld_str_to_mls(english_ld, "$BDEF", true, imls_slow, 40);
  assert(num_mls == num_mls_slow);
  assert(num_mls == 5);

  // Invalid multichar strings - should return -1
  assert(fast_str_to_mls(&fc_polish, "ŻŚ[Ż]GÓI", true, imls, 40) == -1);
  assert(fast_str_to_mls(&fc_polish, "Ż[ŚŻ]GÓI", true, imls, 40) == -1);
  assert(fast_str_to_mls(&fc_catalan, "A[ES]A", true, imls, 40) == -1);
  assert(fast_str_to_mls(&fc_catalan, "ABCD[ABCEFGH]EGD", true, imls, 40) ==
         -1);
  assert(fast_str_to_mls(&fc_catalan, "[", true, imls, 40) == -1);
  assert(fast_str_to_mls(&fc_catalan, "]", true, imls, 40) == -1);
  assert(fast_str_to_mls(&fc_catalan, "[]", true, imls, 40) == -1);
  assert(fast_str_to_mls(&fc_catalan, "ABC[DE", true, imls, 40) == -1);
  assert(fast_str_to_mls(&fc_catalan, "ABCD]E", true, imls, 40) == -1);
  assert(fast_str_to_mls(&fc_catalan, "ABC[D]E", true, imls, 40) == -1);
  assert(fast_str_to_mls(&fc_catalan, "[L·L[QU]]ES", true, imls, 40) == -1);
  assert(fast_str_to_mls(&fc_catalan, "[L·L]ES[QU][qu][]A[QU][qu]", true, imls,
                         40) == -1);
  assert(fast_str_to_mls(&fc_catalan, "[L·L]ES[QU][qu]A[QU][qu", true, imls,
                         40) == -1);

  // Invalid letters
  assert(fast_str_to_mls(&fc_english, "2", true, imls, 40) == -1);
  assert(fast_str_to_mls(&fc_english, "ABC9EFG", true, imls, 40) == -1);

  // Play through not allowed
  assert(fast_str_to_mls(&fc_english, "AB.F", false, imls, 40) == -1);
  assert(fast_str_to_mls(&fc_english, "BEHF.", false, imls, 40) == -1);
  assert(fast_str_to_mls(&fc_english, ".BDEF", false, imls, 40) == -1);

  config_destroy(nwl_config);
  config_destroy(osps_config);
  config_destroy(disc_config);
}

void test_ld(void) {
  test_ld_score_order();
  test_ld_str_to_mls();
  test_fast_str_to_mls();
}