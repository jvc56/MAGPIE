#include "rack_size_test.h"

#include "../src/def/bit_rack_defs.h"
#include "../src/def/board_defs.h"
#include "../src/def/equity_defs.h"
#include "../src/def/game_history_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/move_defs.h"
#include "../src/def/rack_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/bit_rack.h"
#include "../src/ent/encoded_rack.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/leave_map.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/impl/cgp.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"
#include "test_util.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    MachineLetter ml = 1;
    if (tile_idx == 0) {
      ml = BLANK_MACHINE_LETTER;
    } else if (tile_idx == RACK_SIZE - 1) {
      ml = z_ml;
    }
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

// ---------------------------------------------------------------------------
// CGP loading. A CGP is where a RACK_SIZE-tile rack first enters the engine
// from the outside, so this is the integration check: racks of exactly
// RACK_SIZE tiles load, the loaded racks and the bag are right, the CGP
// round-trips, and move generation runs on the loaded position with every
// move bounded by RACK_SIZE.
//
// Rejection of a RACK_SIZE + 1 rack is deliberately not asserted here yet:
// that check is added by jvc56/MAGPIE#660 and is not on this branch.
// ---------------------------------------------------------------------------

enum { RACK_SIZE_CGP_BUFFER = 512, RACK_SIZE_RACK_BUFFER = 32 };

// Writes `count` copies of `letters` cycled from the front, so "ABCDEFGH"
// with count 3 is "ABC" and "E" with count 3 is "EEE". Inputs are already in
// machine-letter order, so the result matches what game_get_cgp writes back.
static void make_rack_string(char *out, size_t cap, const char *letters,
                             int count) {
  const size_t len = strlen(letters);
  assert((size_t)count < cap);
  for (int i = 0; i < count; i++) {
    out[i] = letters[len == 1 ? 0 : (size_t)i % len];
  }
  out[count] = '\0';
}

// An empty BOARD_DIM x BOARD_DIM board, the two racks, no score, no zeros.
static void build_cgp(char *out, size_t cap, const char *rack1,
                      const char *rack2) {
  int offset = 0;
  for (int row = 0; row < BOARD_DIM; row++) {
    offset += snprintf(out + offset, cap - (size_t)offset, "%s%d",
                       row == 0 ? "" : "/", BOARD_DIM);
  }
  snprintf(out + offset, cap - (size_t)offset, " %s/%s 0/0 0", rack1, rack2);
}

// Loads the racks into `game`, checks their sizes, then writes the position
// back out with game_get_cgp and loads that into `reloaded`. The racks must
// come through structurally equal. Comparing racks rather than strings keeps
// this independent of serialization details such as where blanks are
// written (the CGP writer puts them after the letters).
static void load_cgp_and_check_racks(Game *game, Game *reloaded,
                                     const char *rack1, const char *rack2) {
  char cgp[RACK_SIZE_CGP_BUFFER];
  build_cgp(cgp, sizeof(cgp), rack1, rack2);
  load_cgp_or_die(game, cgp);

  assert(rack_get_total_letters(player_get_rack(game_get_player(game, 0))) ==
         (int)strlen(rack1));
  assert(rack_get_total_letters(player_get_rack(game_get_player(game, 1))) ==
         (int)strlen(rack2));

  char *written = game_get_cgp(game, false);
  load_cgp_or_die(reloaded, written);
  free(written);
  for (int player_idx = 0; player_idx < 2; player_idx++) {
    assert(racks_are_equal(
        player_get_rack(game_get_player(game, player_idx)),
        player_get_rack(game_get_player(reloaded, player_idx))));
  }
}

static void assert_moves_bounded_by_rack_size(Game *game) {
  MoveList *move_list = move_list_create(50);
  const MoveGenArgs args = {
      .game = game,
      .move_list = move_list,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
  };
  generate_moves_for_game(&args);
  assert(move_list_get_count(move_list) > 0);
  SortedMoveList *sorted = sorted_move_list_create(move_list);
  bool saw_tile_placement = false;
  for (int move_idx = 0; move_idx < sorted->count; move_idx++) {
    const Move *move = sorted->moves[move_idx];
    assert(move_get_tiles_played(move) >= 0);
    assert(move_get_tiles_played(move) <= RACK_SIZE);
    if (move_get_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE) {
      saw_tile_placement = true;
    }
  }
  assert(saw_tile_placement);
  sorted_move_list_destroy(sorted);
  move_list_destroy(move_list);
}

static void test_rack_size_cgp(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 score -s2 score -r1 all -r2 all -numplays 50");
  Game *game = config_game_create(config);
  Game *reloaded = config_game_create(config);
  const Bag *bag = game_get_bag(game);
  char rack1[RACK_SIZE_RACK_BUFFER];
  char rack2[RACK_SIZE_RACK_BUFFER];

  // Empty racks: establishes the full bag size without hard-coding it.
  load_cgp_and_check_racks(game, reloaded, "", "");
  const int full_bag = bag_get_letters(bag);
  assert(full_bag > 2 * RACK_SIZE);

  // Full racks of RACK_SIZE distinct letters, disjoint between the players.
  make_rack_string(rack1, sizeof(rack1), "ABCDEFGH", RACK_SIZE);
  make_rack_string(rack2, sizeof(rack2), "IJKLMNOP", RACK_SIZE);
  load_cgp_and_check_racks(game, reloaded, rack1, rack2);
  assert(bag_get_letters(bag) == full_bag - 2 * RACK_SIZE);
  assert_moves_bounded_by_rack_size(game);

  // Full racks of one repeated letter (E and A both have more than 8 tiles).
  make_rack_string(rack1, sizeof(rack1), "E", RACK_SIZE);
  make_rack_string(rack2, sizeof(rack2), "A", RACK_SIZE);
  load_cgp_and_check_racks(game, reloaded, rack1, rack2);
  assert(bag_get_letters(bag) == full_bag - 2 * RACK_SIZE);

  // Both blanks plus letters on one side; "?" sorts first, so this is still
  // in the order game_get_cgp writes.
  rack1[0] = '?';
  rack1[1] = '?';
  make_rack_string(rack1 + 2, sizeof(rack1) - 2, "ABCDEF", RACK_SIZE - 2);
  make_rack_string(rack2, sizeof(rack2), "IJKLMNOP", RACK_SIZE);
  load_cgp_and_check_racks(game, reloaded, rack1, rack2);
  assert(bag_get_letters(bag) == full_bag - 2 * RACK_SIZE);
  assert_moves_bounded_by_rack_size(game);

  // One tile short of full on both sides.
  make_rack_string(rack1, sizeof(rack1), "ABCDEFGH", RACK_SIZE - 1);
  make_rack_string(rack2, sizeof(rack2), "IJKLMNOP", RACK_SIZE - 1);
  load_cgp_and_check_racks(game, reloaded, rack1, rack2);
  assert(bag_get_letters(bag) == full_bag - 2 * (RACK_SIZE - 1));

  game_destroy(reloaded);
  game_destroy(game);
  config_destroy(config);
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
  test_rack_size_cgp();

  leave_map_destroy(leave_map);
  rack_destroy(decoded);
  rack_destroy(rack);
  config_destroy(config);
}
