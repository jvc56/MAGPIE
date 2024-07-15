#include "../../src/ent/bag_bitmaps.h"
#include "../../src/ent/letter_distribution.h"

#include "../../src/impl/config.h"

#include "test_util.h"

void test_bag_bitmaps(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  const LetterDistribution *ld = config_get_ld(config);
  BagBitMaps *bb = bag_bitmaps_create(ld, 1);
  bag_bitmaps_destroy(bb);
  config_destroy(config);
}
