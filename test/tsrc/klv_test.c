#include <assert.h>

#include "../../src/ent/klv.h"
#include "../../src/ent/letter_distribution.h"

#include "../../src/impl/config.h"

#include "test_util.h"

void test_small_klv(void) {
  Config *config = config_create_or_die("set -lex CSW21 -ld english_small");
  const LetterDistribution *ld = config_get_ld(config);

  assert(ld_get_size(ld) == 3);

  config_destroy(config);
}

void test_normal_klv(void) { return; }

void test_klv(void) {
  test_small_klv();
  test_normal_klv();
}