#include <assert.h>

#include "../../src/ent/bit_rack.h"
#include "../../src/ent/letter_distribution.h"
#include "../../src/ent/rack.h"
#include "../../src/impl/config.h"

#include "test_util.h"

void test_compatibility(void) {
  Config *english_config = config_create_or_die("set -lex NWL20");
  const LetterDistribution *english_ld = config_get_ld(english_config);
  if (BOARD_DIM <= 15) {
    // Could have at most 14 E's in a row (designating 2 blanks)
    // 14 <= 15, fits in 4 bits.
    assert(bit_rack_is_compatible_with_ld(english_ld));
  } else {
    // english_super has enough E's to fill a row of 21
    // 21 > 15, doesn't fit in 4 bits.
    assert(!bit_rack_is_compatible_with_ld(english_ld));
  }
  config_destroy(english_config);

  if (BOARD_DIM > 15) {
    // Super letter distributions not yet available for other languages.
    return;
  }

  Config *french_config = config_create_or_die("set -lex FRA20");
  const LetterDistribution *french_ld = config_get_ld(french_config);
  // French has 15 E's. With blanks more than 15 E's could be placed in a board
  // row, but the board isn't big enough.
  assert(bit_rack_is_compatible_with_ld(french_ld));
  config_destroy(french_config);

  // Alphabet size is 33, 33*4 > 128
  Config *polish_config = config_create_or_die("set -lex OSPS49");
  const LetterDistribution *polish_ld = config_get_ld(polish_config);
  assert(!bit_rack_is_compatible_with_ld(polish_ld));
  config_destroy(polish_config);
}

void test_create_from_rack(void) {
  Config *config = config_create_or_die("set -lex NWL20");
  const LetterDistribution *ld = config_get_ld(config);
  const int ld_size = ld_get_size(ld);
  Rack *rack = rack_create(ld_size);
  rack_set_to_string(ld, rack, "AABBEWW");
  const BitRack bit_rack = bit_rack_create_from_rack(ld, rack);
  for (int ml = 0; ml < ld_size; ml++) {
    assert(rack_get_letter(rack, ml) == bit_rack_get_letter(&bit_rack, ml));
  }
  rack_destroy(rack);
  config_destroy(config);
}

void test_add_bit_rack(void) {
  Config *config = config_create_or_die("set -lex NWL20");
  BitRack bit_rack = bit_rack_create_empty();
  const LetterDistribution *ld = config_get_ld(config);
  const int ld_size = ld_get_size(ld);
  Rack *rack = rack_create(ld_size);
  rack_set_to_string(ld, rack, "ABCDEFGHIJKLMNOPQRSTUVWXYZ?");
  const BitRack bit_rack_to_add = bit_rack_create_from_rack(ld, rack);
  rack_destroy(rack);

  for (int i = 0; i < 15; i++) {
    bit_rack_add_bit_rack(&bit_rack, &bit_rack_to_add);
  }

  for (int ml = 0; ml < ld_size; ml++) {
    assert(bit_rack_get_letter(&bit_rack, ml) == 15);
  }

  config_destroy(config);
}

void test_high_and_low_64(void) {
  Config *config = config_create_or_die("set -lex NWL20");
  const LetterDistribution *ld = config_get_ld(config);
  const int ld_size = ld_get_size(ld);
  Rack *rack = rack_create(ld_size);
  rack_set_to_string(ld, rack, "??Z");
  const BitRack bit_rack = bit_rack_create_from_rack(ld, rack);
  assert(bit_rack_high_64(&bit_rack) == 1ULL << (26 * 4 - 64));
  assert(bit_rack_low_64(&bit_rack) == 2);
  rack_destroy(rack);
  config_destroy(config);
}

void test_div_mod(void) {
  Config *config = config_create_or_die("set -lex NWL20");
  const LetterDistribution *ld = config_get_ld(config);
  const int ld_size = ld_get_size(ld);
  Rack *rack = rack_create(ld_size);
  rack_set_to_string(ld, rack, "Z"); // 1 << (4*26);
  const BitRack bit_rack = bit_rack_create_from_rack(ld, rack);

  BitRack quotient;
  uint32_t remainder;
  bit_rack_div_mod(&bit_rack, 2, &quotient, &remainder);
  assert(bit_rack_high_64(&quotient) == 1ULL << 39);
  assert(bit_rack_low_64(&quotient) == 0);
  assert(remainder == 0);

  bit_rack_div_mod(&bit_rack, 3, &quotient, &remainder);
  assert(bit_rack_high_64(&quotient) == 366503875925);
  assert(bit_rack_low_64(&quotient) == 6148914691236517205);
  assert(remainder == 1);

  rack_set_to_string(ld, rack, "Q"); // 1 << (4*17);
  const BitRack bit_rack2 = bit_rack_create_from_rack(ld, rack);

  bit_rack_div_mod(&bit_rack2, 1000003, &quotient, &remainder);
  assert(bit_rack_high_64(&quotient) == 0);
  assert(bit_rack_low_64(&quotient) == 295147019738293);
  assert(remainder == 610977);

  rack_destroy(rack);
  config_destroy(config);
}

void test_bit_rack(void) {
  test_compatibility();
  if (BOARD_DIM > 15) {
    // No current super config is compatible with bit_rack.
    return;
  }
  test_create_from_rack();
  test_add_bit_rack();
  test_high_and_low_64();
  test_div_mod();
}