#include "../../src/impl/config.h"

#include "../../src/impl/rack_list.h"

#include "test_util.h"

void test_rack_list(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  const LetterDistribution *ld = config_get_ld(config);

  RackList *rack_list = rack_list_create(ld, 3);
  rack_list_destroy(rack_list);
  config_destroy(config);
}