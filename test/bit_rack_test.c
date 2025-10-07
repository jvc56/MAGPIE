#include "../src/def/bit_rack_defs.h"
#include "../src/def/board_defs.h"
#include "../src/ent/bit_rack.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/rack.h"
#include "../src/impl/config.h"
#include "test_util.h"
#include <assert.h>
#include <stdint.h>

void test_compatibility(void) {
  assert(bit_rack_type_has_expected_size());

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
    assert(rack_get_letter(rack, ml) ==
           (int8_t)bit_rack_get_letter(&bit_rack, ml));
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
  assert(bit_rack_get_high_64(&bit_rack) ==
         1ULL << (26 * BIT_RACK_BITS_PER_LETTER - 64));
  assert(bit_rack_get_low_64(&bit_rack) == 2);
  rack_destroy(rack);
  config_destroy(config);
}

// Helper to count set bits (popcount)
static int popcount64(uint64_t x) {
  int count = 0;
  while (x) {
    count += x & 1;
    x >>= 1;
  }
  return count;
}

void test_hash_mixing(void) {
  Config *config = config_create_or_die("set -lex NWL20");
  const LetterDistribution *ld = config_get_ld(config);

  // Test 1: Different racks should produce different hashes
  BitRack rack1 = string_to_bit_rack(ld, "AEINRST");
  BitRack rack2 = string_to_bit_rack(ld, "RETINAS");
  BitRack rack3 = string_to_bit_rack(ld, "ZZZZZZZ");

  uint64_t hash1 = bit_rack_mix_to_64(&rack1);
  uint64_t hash2 = bit_rack_mix_to_64(&rack2);
  uint64_t hash3 = bit_rack_mix_to_64(&rack3);

  // Same letters should produce same hash (anagram property)
  assert(hash1 == hash2);
  // Different letters should produce different hash
  assert(hash1 != hash3);

  // Test 2: Avalanche property - flipping one bit should change ~50% of output
  // bits: change one letter count (A from 1 to 2)
  BitRack rack_modified = rack1;
  bit_rack_add_letter(&rack_modified, ld_hl_to_ml(ld, "A"));

  uint64_t hash_modified = bit_rack_mix_to_64(&rack_modified);
  uint64_t diff = hash1 ^ hash_modified;
  int changed_bits = popcount64(diff);

  // Should change roughly 32 bits (25-39 is reasonable range for good mixing)
  assert(changed_bits >= 25 && changed_bits <= 39);

  // Test 3: Changing letters in high bits should affect low bits of hash
  // Z is at position 26*4 = 104 bits (in high 64)
  BitRack high_bits = string_to_bit_rack(ld, "Z");
  BitRack high_bits_modified = string_to_bit_rack(ld, "ZZ");

  uint64_t hash_high1 = bit_rack_mix_to_64(&high_bits);
  uint64_t hash_high2 = bit_rack_mix_to_64(&high_bits_modified);

  // Check that low 32 bits changed significantly
  uint32_t low1 = (uint32_t)hash_high1;
  uint32_t low2 = (uint32_t)hash_high2;
  int low_changed = popcount64((uint64_t)(low1 ^ low2));
  assert(low_changed >= 8); // At least some low bits changed

  // Test 4: Bucket index is within bounds
  const uint32_t num_buckets = 256; // power of 2
  BitRack test_rack = string_to_bit_rack(ld, "TESTING");
  uint32_t bucket = bit_rack_get_bucket_index(&test_rack, num_buckets);
  assert(bucket < num_buckets);

  // Test with different power-of-2 sizes
  assert(bit_rack_get_bucket_index(&test_rack, 64) < 64);
  assert(bit_rack_get_bucket_index(&test_rack, 1024) < 1024);

  config_destroy(config);
}

void test_mul(void) {
  Config *config = config_create_or_die("set -lex NWL20");
  const LetterDistribution *ld = config_get_ld(config);

  // 5 << (4*15)
  BitRack ooooo = string_to_bit_rack(ld, "OOOOO");
  assert(bit_rack_get_high_64(&ooooo) == 0);
  assert(bit_rack_get_low_64(&ooooo) == (5ULL << 60));
  // should shift the 5 value from the highest of the low to the lowest of the
  // high
  BitRack result = bit_rack_mul(&ooooo, 16);
  assert(bit_rack_get_high_64(&result) == 5);
  assert(bit_rack_get_low_64(&result) == 0);

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
  test_hash_mixing();
  test_mul();
}