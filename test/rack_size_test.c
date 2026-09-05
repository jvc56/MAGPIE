#include "rack_size_test.h"

#include "../src/def/bit_rack_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/rack_defs.h"
#include "../src/ent/bit_rack.h"
#include "../src/ent/encoded_rack.h"
#include "../src/ent/equity.h"
#include "../src/ent/leave_map.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/rack.h"
#include "../src/impl/config.h"
#include "test_util.h"
#include <assert.h>
#include <stdint.h>

// Tests for the structures that are sized by RACK_SIZE, written so that
// every expectation is derived from RACK_SIZE rather than from 7-tile
// literals. These run at any RACK_SIZE (2 through 8) and any BOARD_DIM, and
// they are what the reduced test suite for a non-default rack size relies
// on. The standard rack_test.c and leave_map_test.c assume DEFAULT_RACK_SIZE.

// BIT_RACK_COMBINATION_OFFSETS must be the prefix sums of C(RACK_SIZE, k)
// for k in [0, RACK_SIZE], ending exactly at 1 << RACK_SIZE. This is the part
// of the build that is actually conditional on RACK_SIZE, and it is what would
// have caught the broken 4/5/6/8 branches (jvc56/MAGPIE#659).
static void test_rack_size_combination_offsets(void) {
  static const uint8_t offsets[] = {BIT_RACK_COMBINATION_OFFSETS};
  assert(sizeof(offsets) / sizeof(offsets[0]) == RACK_SIZE + 1);
  int expected_offset = 0;
  int binomial = 1;
  for (int size = 0; size <= RACK_SIZE; size++) {
    assert(offsets[size] == expected_offset);
    expected_offset += binomial;
    binomial = binomial * (RACK_SIZE - size) / (size + 1);
  }
  assert(expected_offset == (1 << RACK_SIZE));
}

// Fills the rack with `size` tiles: distinct letters A, B, C, ... when
// `repeat_ml` is 0, otherwise `size` copies of `repeat_ml`.
static void fill_rack(Rack *rack, int size, MachineLetter repeat_ml) {
  rack_reset(rack);
  for (int tile_idx = 0; tile_idx < size; tile_idx++) {
    const MachineLetter ml =
        repeat_ml != 0 ? repeat_ml : (MachineLetter)(tile_idx + 1);
    rack_add_letter(rack, ml);
  }
}

static void test_rack_size_bit_rack_counts(const LetterDistribution *ld,
                                           Rack *rack) {
  const int ld_size = ld_get_size(ld);
  // A full rack whose letters repeat with period 3, so several counts are
  // above one, plus a blank.
  rack_reset(rack);
  for (int tile_idx = 0; tile_idx < RACK_SIZE; tile_idx++) {
    rack_add_letter(rack, tile_idx == 0 ? BLANK_MACHINE_LETTER
                                        : (MachineLetter)(1 + (tile_idx % 3)));
  }
  assert(rack_get_total_letters(rack) == RACK_SIZE);
  const BitRack bit_rack = bit_rack_create_from_rack(ld, rack);
  for (int ml = 0; ml < ld_size; ml++) {
    assert(rack_get_letter(rack, ml) ==
           (int8_t)bit_rack_get_letter(&bit_rack, ml));
  }
}

static void assert_encode_decode(const Rack *rack, Rack *decoded) {
  EncodedRack encoded_rack;
  rack_encode(rack, &encoded_rack);
  rack_decode(&encoded_rack, decoded);
  assert(racks_are_equal(rack, decoded));
}

// Every size from empty to full, for the shapes that stress the encoding
// differently: many distinct letters (one slot each), one letter repeated
// (a count that needs all of BITS_PER_COUNT), the highest machine letter,
// and blanks.
static void test_rack_size_encoded_rack(Rack *rack, Rack *decoded) {
  const MachineLetter z_ml = 26;
  for (int size = 0; size <= RACK_SIZE; size++) {
    fill_rack(rack, size, 0);
    assert_encode_decode(rack, decoded);
    fill_rack(rack, size, 1);
    assert_encode_decode(rack, decoded);
    fill_rack(rack, size, z_ml);
    assert_encode_decode(rack, decoded);
    // fill_rack treats 0 as "distinct", so add the blanks directly.
    rack_reset(rack);
    for (int tile_idx = 0; tile_idx < size; tile_idx++) {
      rack_add_letter(rack, BLANK_MACHINE_LETTER);
    }
    assert_encode_decode(rack, decoded);
  }
  // Mixed: blanks, a repeated low letter, and Z, totalling exactly RACK_SIZE.
  rack_reset(rack);
  for (int tile_idx = 0; tile_idx < RACK_SIZE; tile_idx++) {
    const MachineLetter ml = tile_idx == 0               ? BLANK_MACHINE_LETTER
                             : tile_idx == RACK_SIZE - 1 ? z_ml
                                                         : 1;
    rack_add_letter(rack, ml);
  }
  assert_encode_decode(rack, decoded);
}

// LeaveMap assigns each distinct letter a contiguous block of bits in
// ascending machine-letter order. Taking a letter clears the highest set bit
// of its block; adding it back sets the lowest clear one. A full rack of
// RACK_SIZE tiles therefore starts at index (1 << RACK_SIZE) - 1.
static void test_rack_size_leave_map(Rack *rack, LeaveMap *leave_map) {
  const int full_index = (1 << RACK_SIZE) - 1;

  // (a) RACK_SIZE distinct letters: letter at position p owns bit p.
  fill_rack(rack, RACK_SIZE, 0);
  leave_map_init(rack, leave_map);
  assert(leave_map_get_current_index(leave_map) == full_index);
  for (int pos = 0; pos < RACK_SIZE; pos++) {
    const MachineLetter ml = (MachineLetter)(pos + 1);
    leave_map_take_letter_and_update_current_index(leave_map, rack, ml);
    assert(leave_map_get_current_index(leave_map) ==
           (full_index & ~(1 << pos)));
    leave_map_set_current_value(leave_map, int_to_equity(100 + pos));
    leave_map_add_letter_and_update_current_index(leave_map, rack, ml);
    assert(leave_map_get_current_index(leave_map) == full_index);
  }
  // Values stored at each one-tile-removed index read back unchanged.
  for (int pos = 0; pos < RACK_SIZE; pos++) {
    const MachineLetter ml = (MachineLetter)(pos + 1);
    leave_map_take_letter_and_update_current_index(leave_map, rack, ml);
    assert(leave_map_get_current_value(leave_map) == int_to_equity(100 + pos));
    leave_map_add_letter_and_update_current_index(leave_map, rack, ml);
  }

  // (b) RACK_SIZE copies of one letter: one block of RACK_SIZE bits. Taking
  // k copies leaves index (1 << (RACK_SIZE - k)) - 1, down to 0 for the empty
  // leave; adding them back walks the same indices in reverse.
  const MachineLetter a_ml = 1;
  fill_rack(rack, RACK_SIZE, a_ml);
  leave_map_init(rack, leave_map);
  assert(leave_map_get_current_index(leave_map) == full_index);
  leave_map_set_current_value(leave_map, int_to_equity(1000));
  for (int taken = 1; taken <= RACK_SIZE; taken++) {
    leave_map_take_letter_and_update_current_index(leave_map, rack, a_ml);
    const int expected_index = (1 << (RACK_SIZE - taken)) - 1;
    assert(leave_map_get_current_index(leave_map) == expected_index);
    leave_map_set_current_value(leave_map, int_to_equity(1000 + taken));
  }
  assert(leave_map_get_current_index(leave_map) == 0);
  for (int taken = RACK_SIZE - 1; taken >= 0; taken--) {
    leave_map_add_letter_and_update_current_index(leave_map, rack, a_ml);
    assert(leave_map_get_current_index(leave_map) ==
           (1 << (RACK_SIZE - taken)) - 1);
    assert(leave_map_get_current_value(leave_map) ==
           int_to_equity(1000 + taken));
  }
  assert(leave_map_get_current_index(leave_map) == full_index);
}

void test_rack_size(void) {
  test_rack_size_combination_offsets();

  Config *config = config_create_or_die("set -lex CSW21");
  const LetterDistribution *ld = config_get_ld(config);
  const int ld_size = ld_get_size(ld);
  Rack *rack = rack_create(ld_size);
  Rack *decoded = rack_create(ld_size);
  LeaveMap *leave_map = leave_map_create(ld_size);

  test_rack_size_bit_rack_counts(ld, rack);
  test_rack_size_encoded_rack(rack, decoded);
  test_rack_size_leave_map(rack, leave_map);

  leave_map_destroy(leave_map);
  rack_destroy(decoded);
  rack_destroy(rack);
  config_destroy(config);
}
