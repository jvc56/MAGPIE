#include "../src/ent/kwg.h"
#include "../src/ent/kwg_alpha.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/players_data.h"
#include "../src/ent/rack.h"
#include "../src/impl/config.h"
#include "test_util.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

void assert_kwg_accepts_alpha(const KWG *kwg, const LetterDistribution *ld,
                              const char *rack_string, bool accepts) {
  Rack *rack = rack_create(ld_get_size(ld));
  rack_set_to_string(ld, rack, rack_string);
  bool assert_cond = kwg_accepts_alpha(kwg, rack) == accepts;
  if (!assert_cond) {
    printf("kwg_accepts_alpha failed assertion for >%s< with %d\n", rack_string,
           assert_cond);
    abort();
  }
  rack_destroy(rack);
}

void assert_kwg_accepts_alpha_with_blanks(const KWG *kwg,
                                          const LetterDistribution *ld,
                                          const char *rack_string,
                                          bool accepts) {
  Rack *rack = rack_create(ld_get_size(ld));
  rack_set_to_string(ld, rack, rack_string);
  bool assert_cond = kwg_accepts_alpha_with_blanks(kwg, rack) == accepts;
  if (!assert_cond) {
    printf("kwg_accepts_alpha_with_blanks failed assertion for >%s< with %d\n",
           rack_string, assert_cond);
    abort();
  }
  rack_destroy(rack);
}

void assert_kwg_compute_alpha_cross_set(const KWG *kwg,
                                        const LetterDistribution *ld,
                                        const char *existing_letters,
                                        const char *expected_cross_set_string) {
  Rack *rack = rack_create(ld_get_size(ld));
  rack_set_to_string(ld, rack, existing_letters);
  uint64_t actual_cross_set = kwg_compute_alpha_cross_set(kwg, rack);
  uint64_t expected_cross_set =
      string_to_cross_set(ld, expected_cross_set_string);
  bool assert_cond = actual_cross_set == expected_cross_set;
  if (!assert_cond) {
    char *actual_cross_set_string = cross_set_to_string(ld, actual_cross_set);
    printf("kwg_compute_alpha_cross_set failed assertion:\n>%s<\n>%s<\nare not "
           "equal",
           actual_cross_set_string, expected_cross_set_string);
    free(actual_cross_set_string);
    abort();
  }
  rack_destroy(rack);
}

void test_kwg_accepts_alpha(void) {
  Config *config = config_create_or_die("set -lex CSW21_alpha -var wordsmog");
  const LetterDistribution *ld = config_get_ld(config);
  const PlayersData *players_data = config_get_players_data(config);
  const KWG *kwg = players_data_get_kwg(players_data, 0);

  assert_kwg_accepts_alpha(kwg, ld, "A", false);
  assert_kwg_accepts_alpha(kwg, ld, "I", false);

  assert_kwg_accepts_alpha(kwg, ld, "AA", true);
  assert_kwg_accepts_alpha(kwg, ld, "AB", true);
  assert_kwg_accepts_alpha(kwg, ld, "BA", true);
  assert_kwg_accepts_alpha(kwg, ld, "AC", false);
  assert_kwg_accepts_alpha(kwg, ld, "CA", false);
  assert_kwg_accepts_alpha(kwg, ld, "AF", true);
  assert_kwg_accepts_alpha(kwg, ld, "FA", true);
  assert_kwg_accepts_alpha(kwg, ld, "ZZ", false);

  assert_kwg_accepts_alpha(kwg, ld, "ABC", true);
  assert_kwg_accepts_alpha(kwg, ld, "ACB", true);
  assert_kwg_accepts_alpha(kwg, ld, "BCA", true);
  assert_kwg_accepts_alpha(kwg, ld, "BAC", true);
  assert_kwg_accepts_alpha(kwg, ld, "CAB", true);
  assert_kwg_accepts_alpha(kwg, ld, "CBA", true);
  assert_kwg_accepts_alpha(kwg, ld, "ZZZ", true);

  assert_kwg_accepts_alpha(kwg, ld, "ABCD", false);
  assert_kwg_accepts_alpha(kwg, ld, "CDAB", false);
  assert_kwg_accepts_alpha(kwg, ld, "BCAD", false);

  assert_kwg_accepts_alpha(kwg, ld, "WAQF", true);
  assert_kwg_accepts_alpha(kwg, ld, "AWFQ", true);
  assert_kwg_accepts_alpha(kwg, ld, "FAWQ", true);
  assert_kwg_accepts_alpha(kwg, ld, "FWQA", true);

  assert_kwg_accepts_alpha(kwg, ld, "AEINRST", true);
  assert_kwg_accepts_alpha(kwg, ld, "RETINAS", true);
  assert_kwg_accepts_alpha(kwg, ld, "ANESTRI", true);

  assert_kwg_accepts_alpha(kwg, ld, "ANESTRIA", true);
  assert_kwg_accepts_alpha(kwg, ld, "ANESTRIB", true);
  assert_kwg_accepts_alpha(kwg, ld, "ANECTRIB", true);
  assert_kwg_accepts_alpha(kwg, ld, "ANESTRIQ", false);
  assert_kwg_accepts_alpha(kwg, ld, "ANESTRIV", false);
  assert_kwg_accepts_alpha(kwg, ld, "ANESTRIX", false);
  assert_kwg_accepts_alpha(kwg, ld, "ANESTRIY", false);
  assert_kwg_accepts_alpha(kwg, ld, "ANESTRIZ", false);

  assert_kwg_accepts_alpha(kwg, ld, "OXYPHENBUTAZONE", true);
  assert_kwg_accepts_alpha(kwg, ld, "OXYPHENBUTAZOEN", true);
  assert_kwg_accepts_alpha(kwg, ld, "PBAOONUTNHXEEZY", true);
  assert_kwg_accepts_alpha(kwg, ld, "NBOAEPUHTZXOYEN", true);
  assert_kwg_accepts_alpha(kwg, ld, "OEUNPYABZXEHNOT", true);
  assert_kwg_accepts_alpha(kwg, ld, "ENZXONHPOEUYABT", true);
  assert_kwg_accepts_alpha(kwg, ld, "ENZXONHPOEUYABE", false);

  assert_kwg_accepts_alpha_with_blanks(kwg, ld, "??", true);
  assert_kwg_accepts_alpha_with_blanks(kwg, ld, "Z??", true);
  assert_kwg_accepts_alpha_with_blanks(kwg, ld, "EARWIG??", true);
  assert_kwg_accepts_alpha_with_blanks(kwg, ld, "TRONGLE?", false);
  assert_kwg_accepts_alpha_with_blanks(kwg, ld, "QQ??", false);

  config_destroy(config);
}

void test_kwg_compute_alpha_cross_set(void) {
  Config *config = config_create_or_die("set -lex CSW21_alpha -var wordsmog");
  const LetterDistribution *ld = config_get_ld(config);
  const PlayersData *players_data = config_get_players_data(config);
  const KWG *kwg = players_data_get_kwg(players_data, 0);

  assert_kwg_compute_alpha_cross_set(kwg, ld, "A", "ABDEFGHIJKLMNPRSTWXYZ");
  assert_kwg_compute_alpha_cross_set(kwg, ld, "AA", "BCFGHIKLMNSVUW");
  assert_kwg_compute_alpha_cross_set(kwg, ld, "B", "AEIOY");
  assert_kwg_compute_alpha_cross_set(kwg, ld, "BB", "AEIOU");
  assert_kwg_compute_alpha_cross_set(kwg, ld, "C", "H");
  assert_kwg_compute_alpha_cross_set(kwg, ld, "V", "");
  assert_kwg_compute_alpha_cross_set(kwg, ld, "Q", "I");
  assert_kwg_compute_alpha_cross_set(kwg, ld, "X", "AEIOU");
  assert_kwg_compute_alpha_cross_set(kwg, ld, "Z", "AEO");

  assert_kwg_compute_alpha_cross_set(kwg, ld, "ZZ", "IUZ");
  assert_kwg_compute_alpha_cross_set(kwg, ld, "ZZZ", "IS");
  assert_kwg_compute_alpha_cross_set(kwg, ld, "ZZZZ", "");

  assert_kwg_compute_alpha_cross_set(kwg, ld, "ABHIKSU", "Z");
  assert_kwg_compute_alpha_cross_set(kwg, ld, "ENZXONHPOEUYAB", "T");

  config_destroy(config);
}

void test_kwg_alpha(void) {
  test_kwg_accepts_alpha();
  test_kwg_compute_alpha_cross_set();
}
