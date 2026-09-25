#include "wmp_move_gen_test.h"

#include "../src/def/board_defs.h"
#include "../src/def/equity_defs.h"
#include "../src/def/kwg_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/move_defs.h"
#include "../src/def/players_data_defs.h"
#include "../src/def/rack_defs.h"
#include "../src/ent/anchor.h"
#include "../src/ent/bit_rack.h"
#include "../src/ent/board.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/leave_map.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/players_data.h"
#include "../src/ent/rack.h"
#include "../src/ent/wmp.h"
#include "../src/ent/word_info_table.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"
#include "../src/impl/wmp_move_gen.h"
#include "../src/impl/word_info_table_maker.h"
#include "../src/util/io_util.h"
#include "test_constants.h"
#include "test_util.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void test_wmp_move_gen_inactive(void) {
  WMPMoveGen wmg;
  // Only wmp is checked by wmp_move_gen_is_active
  // No wmp -> wmp_move_gen unactive and not used by move_gen
  wmp_move_gen_init(&wmg, /*ld=*/NULL, /*rack=*/NULL, /*wmp=*/NULL);
  assert(!wmp_move_gen_is_active(&wmg));
}

void test_wit_prune_skips_block_longer_than_anchor_word(void) {
  WMPMoveGen wmg = {0};
  Anchor anchor = {
      .playthrough_blocks = 1,
      .word_length = 2,
      .rightmost_start_col = 0,
  };
  Square row_cache[BOARD_DIM] = {0};
  row_cache[0].letter = 1;
  row_cache[1].letter = 2;
  row_cache[2].letter = 3;

  uint32_t block_row[BOARD_DIM] = {0};
  const uint32_t *wit_row_lane[BOARD_DIM] = {0};
  uint8_t wit_len_lane[BOARD_DIM] = {0};
  wit_row_lane[0] = block_row;
  wit_len_lane[0] = 3;

  wmp_move_gen_set_playthrough_bit_rack(&wmg, &anchor, row_cache, wit_row_lane,
                                        wit_len_lane, NULL);

  // The cached block is not wholly contained in this shorter shadow word, so
  // it cannot constrain the optional WIT prune.
  assert(wmg.playthrough_addable == UINT32_MAX);
  assert(wmg.num_tiles_played_through == 3);
  // WIT-disabled callers supply no row lane. Collect playthrough letters
  // without touching any cached table storage.
  wmp_move_gen_set_playthrough_bit_rack(&wmg, &anchor, row_cache, NULL, NULL,
                                        NULL);
  assert(wmg.playthrough_addable == UINT32_MAX);
  assert(wmg.num_tiles_played_through == 3);
}

void test_sparse_anchor_slot_order_and_reset(void) {
  WMPMoveGen wmg = {0};
  WMP fake_wmp = {0};
  fake_wmp.board_dim = BOARD_DIM;
  wmg.wmp = &fake_wmp;
  for (int i = 0; i < MAX_WMP_MOVE_GEN_ANCHORS; i++) {
    wmg.anchors[i].highest_possible_equity = EQUITY_MIN_VALUE;
    wmg.anchors[i].highest_possible_score = EQUITY_MIN_VALUE;
    wmg.anchors[i].leftmost_start_col = BOARD_DIM - 1;
  }

  // Touch slots in descending mask-word/slot order. Emission must retain the
  // old full-array scan's ascending order, including for super boards where
  // the final slot is in a second mask word.
  wmg.playthrough_blocks = MAX_POSSIBLE_PLAYTHROUGH_BLOCKS - 1;
  wmp_move_gen_maybe_update_anchor(&wmg, RACK_SIZE, BOARD_DIM, 3,
                                   int_to_equity(30), int_to_equity(31));
  wmg.playthrough_blocks = 1;
  wmp_move_gen_maybe_update_anchor(&wmg, 1, 2, 2, int_to_equity(20),
                                   int_to_equity(21));
  wmg.playthrough_blocks = 0;
  wmp_move_gen_maybe_update_anchor(&wmg, RACK_SIZE, RACK_SIZE, 1,
                                   int_to_equity(10), int_to_equity(11));

  AnchorHeap anchor_heap = {0};
  wmp_move_gen_add_anchors(&wmg, /*row=*/0, /*col=*/0,
                           /*last_anchor_col=*/0,
                           /*dir=*/BOARD_HORIZONTAL_DIRECTION,
                           /*inference_cutoff_equity=*/EQUITY_MAX_VALUE,
                           &anchor_heap);
  assert(anchor_heap.count == 3);
  assert(anchor_heap.anchors[0].playthrough_blocks == 0);
  assert(anchor_heap.anchors[1].playthrough_blocks == 1);
  assert(anchor_heap.anchors[2].playthrough_blocks ==
         MAX_POSSIBLE_PLAYTHROUGH_BLOCKS - 1);

  wmp_move_gen_reset_anchors(&wmg);
  for (int i = 0; i < WMP_ANCHOR_MASK_WORDS; i++) {
    assert(wmg.touched_anchor_masks[i] == 0);
  }
  for (int i = 0; i < anchor_heap.count; i++) {
    const Anchor *emitted = &anchor_heap.anchors[i];
    const Anchor *reset = wmp_move_gen_get_anchor(
        &wmg, emitted->playthrough_blocks, emitted->tiles_to_play);
    assert(reset->tiles_to_play == 0);
    assert(reset->highest_possible_equity == EQUITY_MIN_VALUE);
    assert(reset->highest_possible_score == EQUITY_MIN_VALUE);
  }
}

// Reusing the generator for a different anchor must replace every mask bit,
// including when the new anchor has no fixed tiles.
static void test_playthrough_positions_reset(void) {
  WMPMoveGen wmg = {0};
  Square row_cache[BOARD_DIM] = {0};
  row_cache[1].letter = 1;
  row_cache[3].letter = get_blanked_machine_letter(2);
  Anchor anchor = {.playthrough_blocks = 2, .rightmost_start_col = 0};
  wmp_move_gen_set_playthrough_bit_rack(&wmg, &anchor, row_cache, NULL, NULL,
                                        NULL);
  assert(wmg.playthrough_positions == ((1U << 1) | (1U << 3)));
  assert(wmg.num_tiles_played_through == 2);
  anchor.playthrough_blocks = 1;
  anchor.rightmost_start_col = BOARD_DIM - 1;
  row_cache[BOARD_DIM - 1].letter = 3;
  wmp_move_gen_set_playthrough_bit_rack(&wmg, &anchor, row_cache, NULL, NULL,
                                        NULL);
  assert(wmg.playthrough_positions == (1U << (BOARD_DIM - 1)));
  assert(wmg.num_tiles_played_through == 1);
  anchor.playthrough_blocks = 0;
  wmp_move_gen_set_playthrough_bit_rack(&wmg, &anchor, row_cache, NULL, NULL,
                                        NULL);
  assert(wmg.playthrough_positions == 0);
  assert(wmg.num_tiles_played_through == 0);
}

// Directly exercise signed offsets, all letters of another block, a
// designated board blank, covered-empty cells and optional-data fallback.
static void test_word_plus_floater_positional_intersection(void) {
  WordInfoTable wit = {0};
  uint32_t ordinary[BOARD_DIM];
  for (int index = 0; index < BOARD_DIM; index++) {
    ordinary[index] = UINT32_MAX;
  }
  wit.tries[2].values = ordinary;
  wit.tries[2].num_values = 1;
  uint32_t *positional =
      calloc(word_plus_floater_cells_per_key(2), sizeof(uint32_t));
  assert(positional != NULL);
  wit.word_plus_floater[2] = positional;
  Square row_cache[BOARD_DIM] = {0};
  const uint32_t *rows[BOARD_DIM] = {0};
  uint8_t lengths[BOARD_DIM] = {0};
  WMPMoveGen wmg = {0};
  Anchor anchor = {
      .playthrough_blocks = 2, .word_length = 5, .rightmost_start_col = 1};
  // BC.AT: B is at delta -3 and C at -2 from AT. Their independent
  // addable masks overlap only in H, so both letters must be queried.
  row_cache[1].letter = 2;
  row_cache[2].letter = get_blanked_machine_letter(3);
  row_cache[4].letter = 1;
  row_cache[5].letter = 20;
  rows[4] = ordinary;
  lengths[4] = 2;
  // extension=3: first cell=3*2, left deltas map to indices 0 and 1.
  positional[((6 + 0) * WPF_ALPHABET_SIZE) + 2 - 1] = (1U << 8) | (1U << 19);
  positional[((6 + 1) * WPF_ALPHABET_SIZE) + 3 - 1] = (1U << 8) | (1U << 18);
  wmp_move_gen_set_playthrough_bit_rack(&wmg, &anchor, row_cache, rows, lengths,
                                        &wit);
  assert(wmg.playthrough_addable == (1U << 8));
  // A covered cell with no matching dictionary word excludes the anchor.
  positional[((6 + 1) * WPF_ALPHABET_SIZE) + 3 - 1] = 0;
  wmp_move_gen_set_playthrough_bit_rack(&wmg, &anchor, row_cache, rows, lengths,
                                        &wit);
  assert(wmg.playthrough_addable == 0);
  // Missing optional payload retains the ordinary WIT condition.
  wit.word_plus_floater[2] = NULL;
  wmp_move_gen_set_playthrough_bit_rack(&wmg, &anchor, row_cache, rows, lengths,
                                        &wit);
  assert(wmg.playthrough_addable == UINT32_MAX);
  wit.word_plus_floater[2] = positional;
  // A same-content row borrowed from another WIT allocation must fail open,
  // without subtracting pointers that belong to different objects.
  uint32_t foreign_ordinary[BOARD_DIM];
  memcpy(foreign_ordinary, ordinary, sizeof(ordinary));
  rows[4] = foreign_ordinary;
  wmp_move_gen_set_playthrough_bit_rack(&wmg, &anchor, row_cache, rows, lengths,
                                        &wit);
  assert(wmg.playthrough_addable == UINT32_MAX);
  // AT.C: extension=2, right delta=3 maps to index 3, and first cell=2.
  memset(row_cache, 0, sizeof(row_cache));
  memset(rows, 0, sizeof(rows));
  memset(lengths, 0, sizeof(lengths));
  row_cache[1].letter = 1;
  row_cache[2].letter = 20;
  row_cache[4].letter = 3;
  rows[1] = ordinary;
  lengths[1] = 2;
  anchor.word_length = 4;
  positional[((2 + 3) * WPF_ALPHABET_SIZE) + 3 - 1] = (1U << 19);
  wmp_move_gen_set_playthrough_bit_rack(&wmg, &anchor, row_cache, rows, lengths,
                                        &wit);
  assert(wmg.playthrough_addable == (1U << 19));
  free(positional);
}

// Dense payloads retain the WIT's original value IDs. Choose the longest
// usable base in either scan order, and preserve covered zero masks.
static void test_word_plus_floater_dense_coverage(void) {
  WordInfoTable wit = {0};
  uint32_t ordinary_short[3 * (BOARD_DIM - 1)];
  uint32_t ordinary_long[BOARD_DIM - 3];
  for (size_t index = 0;
       index < sizeof(ordinary_short) / sizeof(ordinary_short[0]); index++) {
    ordinary_short[index] = UINT32_MAX;
  }
  for (size_t index = 0;
       index < sizeof(ordinary_long) / sizeof(ordinary_long[0]); index++) {
    ordinary_long[index] = UINT32_MAX;
  }
  const size_t short_cells = word_plus_floater_cells_per_key(2);
  const size_t long_cells = word_plus_floater_cells_per_key(4);
  uint32_t *short_masks = calloc(3 * short_cells, sizeof(uint32_t));
  uint32_t *long_masks = calloc(long_cells, sizeof(uint32_t));
  assert(short_masks != NULL && long_masks != NULL);
  for (size_t index = 0; index < short_cells; index++) {
    short_masks[short_cells + index] = 1U << 18;
    short_masks[(2 * short_cells) + index] = 1U << 8;
  }
  for (size_t index = 0; index < long_cells; index++) {
    long_masks[index] = 1U << 19;
  }
  wit.tries[2].values = ordinary_short;
  wit.tries[2].num_values = 3;
  wit.tries[4].values = ordinary_long;
  wit.tries[4].num_values = 1;
  wit.word_plus_floater[2] = short_masks;
  wit.word_plus_floater[4] = long_masks;
  WMPMoveGen wmg = {0};
  Anchor anchor = {
      .playthrough_blocks = 2, .word_length = 7, .rightmost_start_col = 1};
  for (int reverse = 0; reverse < 2; reverse++) {
    Square row_cache[BOARD_DIM] = {0};
    const uint32_t *rows[BOARD_DIM] = {0};
    uint8_t lengths[BOARD_DIM] = {0};
    const int short_col = reverse ? 6 : 1;
    const int long_col = reverse ? 1 : 4;
    row_cache[short_col].letter = 1;
    row_cache[short_col + 1].letter = 20;
    for (int index = 0; index < 4; index++) {
      row_cache[long_col + index].letter = (MachineLetter)(3 + index);
    }
    rows[short_col] = ordinary_short + (size_t)2 * wit_stride_for_len(2);
    rows[long_col] = ordinary_long;
    lengths[short_col] = 2;
    lengths[long_col] = 4;
    wmp_move_gen_set_playthrough_bit_rack(&wmg, &anchor, row_cache, rows,
                                          lengths, &wit);
    assert(wmg.playthrough_addable == (1U << 19));

    // A stale cached long row must not hide a valid shorter row. The
    // short lookup uses original value ID 2 rather than payload row 0.
    uint32_t foreign_long[BOARD_DIM - 3];
    memcpy(foreign_long, ordinary_long, sizeof(ordinary_long));
    rows[long_col] = foreign_long;
    wmp_move_gen_set_playthrough_bit_rack(&wmg, &anchor, row_cache, rows,
                                          lengths, &wit);
    assert(wmg.playthrough_addable == (1U << 8));
    rows[short_col] = ordinary_short + wit_stride_for_len(2);
    wmp_move_gen_set_playthrough_bit_rack(&wmg, &anchor, row_cache, rows,
                                          lengths, &wit);
    assert(wmg.playthrough_addable == (1U << 18));
    rows[short_col] = ordinary_short;
    wmp_move_gen_set_playthrough_bit_rack(&wmg, &anchor, row_cache, rows,
                                          lengths, &wit);
    assert(wmg.playthrough_addable == 0);
  }
  free(short_masks);
  free(long_masks);
}

// Set empty leave to 0.0, all one-tile leaves to +1.0, two-tile leaves to +2.0,
// etc.
void set_dummy_leave_values(LeaveMap *leave_map) {
  for (int leave_idx = 0; leave_idx < 1 << RACK_SIZE; leave_idx++) {
    leave_map_set_current_index(leave_map, leave_idx);
    int bits_set = 0;
    for (int i = 0; i < RACK_SIZE; i++) {
      if (leave_idx & (1 << i)) {
        bits_set++;
      }
    }
    const Equity value = int_to_equity(bits_set);
    leave_map_set_current_value(leave_map, value);
  }
}

static void assert_nonplaythrough_subrack_enumeration(
    const LetterDistribution *ld, const WMP *wmp, const char *rack_string,
    const int expected_counts[RACK_SIZE + 1]) {
  Rack *rack = rack_create(ld_get_size(ld));
  const int rack_size = rack_set_to_string(ld, rack, rack_string);
  assert(rack_size >= 0 && rack_size <= RACK_SIZE);

  WMPMoveGen wmg;
  wmp_move_gen_init(&wmg, ld, rack, wmp);
  for (int size = 0; size <= RACK_SIZE; size++) {
    wmg.count_by_size[size] = 0;
  }

  LeaveMap leave_map;
  leave_map_init(rack, &leave_map);
  const int full_rack_index = (1 << rack_size) - 1;
  for (int leave_idx = 0; leave_idx <= full_rack_index; leave_idx++) {
    leave_map_set_current_index(&leave_map, leave_idx);
    leave_map_set_current_value(&leave_map, int_to_equity(leave_idx));
  }
  leave_map_set_current_index(&leave_map, full_rack_index);

  wmp_move_gen_enumerate_nonplaythrough_subracks(&wmg, &leave_map);
  assert(leave_map_get_current_index(&leave_map) == full_rack_index);

  MachineLetter tiles[RACK_SIZE];
  int num_tiles = 0;
  for (int ml = 0; ml < ld_get_size(ld); ml++) {
    const int count = rack_get_letter(rack, ml);
    for (int i = 0; i < count; i++) {
      tiles[num_tiles++] = (MachineLetter)ml;
    }
  }
  assert(num_tiles == rack_size);

  BitRack expected_subracks[1 << RACK_SIZE];
  int expected_sizes[1 << RACK_SIZE];
  bool expected_seen[1 << RACK_SIZE] = {false};
  int num_expected = 0;
  for (int mask = 0; mask < 1 << rack_size; mask++) {
    BitRack subrack = bit_rack_create_empty();
    int size = 0;
    for (int tile_idx = 0; tile_idx < rack_size; tile_idx++) {
      if ((mask & (1 << tile_idx)) != 0) {
        bit_rack_add_letter(&subrack, tiles[tile_idx]);
        size++;
      }
    }

    bool already_expected = false;
    for (int expected_idx = 0; expected_idx < num_expected; expected_idx++) {
      if (bit_rack_equals(&subrack, &expected_subracks[expected_idx])) {
        already_expected = true;
        break;
      }
    }
    if (!already_expected) {
      expected_subracks[num_expected] = subrack;
      expected_sizes[num_expected] = size;
      num_expected++;
    }
  }

  int num_enumerated = 0;
  for (int size = 0; size <= RACK_SIZE; size++) {
    assert(wmg.count_by_size[size] == expected_counts[size]);
    const int offset = subracks_get_combination_offset(size);
    for (int idx_for_size = 0; idx_for_size < wmg.count_by_size[size];
         idx_for_size++) {
      const SubrackInfo *info =
          &wmg.nonplaythrough_infos[offset + idx_for_size];
      int expected_idx = -1;
      for (int i = 0; i < num_expected; i++) {
        if (bit_rack_equals(&info->subrack, &expected_subracks[i])) {
          expected_idx = i;
          break;
        }
      }
      if (idx_for_size > 0) {
        const BitRack *previous =
            &wmg.nonplaythrough_infos[offset + idx_for_size - 1].subrack;
        bool found_difference = false;
        for (int ml = 0; ml < ld_get_size(ld); ml++) {
          const int previous_count = bit_rack_get_letter(previous, ml);
          const int current_count = bit_rack_get_letter(&info->subrack, ml);
          if (previous_count != current_count) {
            assert(previous_count < current_count);
            found_difference = true;
            break;
          }
        }
        assert(found_difference);
      }
      assert(expected_idx >= 0);
      assert(expected_sizes[expected_idx] == size);
      assert(!expected_seen[expected_idx]);
      expected_seen[expected_idx] = true;

      Rack expected_leave;
      rack_copy(&expected_leave, rack);
      LeaveMap expected_leave_map;
      leave_map_init(&expected_leave, &expected_leave_map);
      for (int ml = 0; ml < ld_get_size(ld); ml++) {
        const int count = bit_rack_get_letter(&info->subrack, ml);
        for (int i = 0; i < count; i++) {
          leave_map_take_letter_and_update_current_index(
              &expected_leave_map, &expected_leave, (MachineLetter)ml);
        }
      }
      assert(info->leave_value ==
             int_to_equity(leave_map_get_current_index(&expected_leave_map)));
      num_enumerated++;
    }
  }

  assert(num_enumerated == num_expected);
  for (int expected_idx = 0; expected_idx < num_expected; expected_idx++) {
    assert(expected_seen[expected_idx]);
  }

  rack_destroy(rack);
}

void test_nonplaythrough_subrack_enumeration(void) {
  Config *config = config_create_or_die("set -lex CSW21 -wmp true");
  Game *game = config_game_create(config);
  const Player *player = game_get_player(game, 0);
  const LetterDistribution *ld = game_get_ld(game);
  const WMP *wmp = player_get_wmp(player);

  const int empty_counts[RACK_SIZE + 1] = {1};
  assert_nonplaythrough_subrack_enumeration(ld, wmp, "", empty_counts);
  const int single_counts[RACK_SIZE + 1] = {1, 1};
  assert_nonplaythrough_subrack_enumeration(ld, wmp, "?", single_counts);
  // The racks below have seven tiles and their expected counts are written
  // for RACK_SIZE 7, so they need at least that many slots; the two cases
  // above hold for every rack size.
#if RACK_SIZE >= 7
  const int full_rack_counts[RACK_SIZE + 1] = {1, 5, 12, 18, 18, 12, 5, 1};
  assert_nonplaythrough_subrack_enumeration(ld, wmp, "AABEE?Z",
                                            full_rack_counts);

  const int short_rack_counts[RACK_SIZE + 1] = {1, 3, 4, 3, 1, 0, 0, 0};
  assert_nonplaythrough_subrack_enumeration(ld, wmp, "AA?Z", short_rack_counts);

  const int repeated_counts[RACK_SIZE + 1] = {1, 1, 1, 1, 1, 1, 1, 1};
  assert_nonplaythrough_subrack_enumeration(ld, wmp, "AAAAAAA",
                                            repeated_counts);
  const int distinct_counts[RACK_SIZE + 1] = {1, 7, 21, 35, 35, 21, 7, 1};
  assert_nonplaythrough_subrack_enumeration(ld, wmp, "?AEIOUZ",
                                            distinct_counts);
  const int two_blank_counts[RACK_SIZE + 1] = {1, 6, 16, 25, 25, 16, 6, 1};
  assert_nonplaythrough_subrack_enumeration(ld, wmp, "??AEIOZ",
                                            two_blank_counts);
#endif

  game_destroy(game);
  config_destroy(config);
}

void test_nonplaythrough_existence(void) {
  Config *config = config_create_or_die("set -lex CSW21 -wmp true");
  Game *game = config_game_create(config);
  const Player *player = game_get_player(game, 0);
  const LetterDistribution *ld = game_get_ld(game);
  const WMP *wmp = player_get_wmp(player);

  WMPMoveGen wmg;
  Rack *rack = rack_create(ld_get_size(ld));
  rack_set_to_string(ld, rack, "VIVIFIC");
  LeaveMap leave_map;
  leave_map_init(rack, &leave_map);
  leave_map_set_current_index(&leave_map, 0);

  wmp_move_gen_init(&wmg, ld, rack, wmp);
  wmp_move_gen_reset_playthrough(&wmg);
  assert(wmp_move_gen_is_active(&wmg));
  assert(!wmp_move_gen_has_playthrough(&wmg));

  // Values not used for check_leaves=false, but
  // wmp_move_gen_check_nonplaythrough_existence moves the leave_map idx even
  // when not checking leaves.
  set_dummy_leave_values(&leave_map);

  wmp_move_gen_check_nonplaythrough_existence(
      &wmg, /*check_leaves=*/false, &leave_map,
      /*subracks_precomputed=*/false,
      /*wmp_entries_precomputed=*/false);

  // IF
  assert(wmp_move_gen_nonplaythrough_word_of_length_exists(&wmg, 2));
  // no 3, 4, 5, or 6 letter words
  for (int len = 3; len <= 6; len++) {
    assert(!wmp_move_gen_nonplaythrough_word_of_length_exists(&wmg, len));
  }
  // VIVIFIC
  assert(wmp_move_gen_nonplaythrough_word_of_length_exists(&wmg, 7));
  const Equity *best_leaves =
      wmp_move_gen_get_nonplaythrough_best_leave_values(&wmg);

  for (int len = MINIMUM_WORD_LENGTH; len <= RACK_SIZE; len++) {
    const int leave_size = RACK_SIZE - len;
    assert(best_leaves[leave_size] == 0);
  }

  wmp_move_gen_check_nonplaythrough_existence(
      &wmg, /*check_leaves=*/true, &leave_map,
      /*subracks_precomputed=*/false,
      /*wmp_entries_precomputed=*/false);
  // IF
  assert(wmp_move_gen_nonplaythrough_word_of_length_exists(&wmg, 2));
  // no 3, 4, 5, or 6 letter words
  for (int len = 3; len <= 6; len++) {
    assert(!wmp_move_gen_nonplaythrough_word_of_length_exists(&wmg, len));
  }
  // VIVIFIC
  assert(wmp_move_gen_nonplaythrough_word_of_length_exists(&wmg, 7));
  best_leaves = wmp_move_gen_get_nonplaythrough_best_leave_values(&wmg);
  for (int word_len = MINIMUM_WORD_LENGTH; word_len <= RACK_SIZE; word_len++) {
    if (!wmp_move_gen_nonplaythrough_word_of_length_exists(&wmg, word_len)) {
      continue;
    }
    const int leave_size = RACK_SIZE - word_len;
    assert(best_leaves[leave_size] == int_to_equity(leave_size));
  }

  rack_destroy(rack);
  game_destroy(game);
  config_destroy(config);
}

void test_playthrough_bingo_existence(void) {
  Config *config = config_create_or_die("set -lex CSW21 -wmp true");
  Game *game = config_game_create(config);
  const Player *player = game_get_player(game, 0);
  const LetterDistribution *ld = game_get_ld(game);
  const WMP *wmp = player_get_wmp(player);

  WMPMoveGen wmg;
  Rack *rack = rack_create(ld_get_size(ld));
  rack_set_to_string(ld, rack, "CHEESE?");
  LeaveMap leave_map;
  leave_map_init(rack, &leave_map);
  leave_map_set_current_index(&leave_map, 0);

  wmp_move_gen_init(&wmg, ld, rack, wmp);
  wmp_move_gen_reset_playthrough(&wmg);
  assert(wmp_move_gen_is_active(&wmg));
  assert(!wmp_move_gen_has_playthrough(&wmg));
  // Add a letter, N. In this context we would be shadowing left.
  wmp_move_gen_add_playthrough_letter(&wmg, ld_hl_to_ml(ld, "N"));
  assert(wmp_move_gen_has_playthrough(&wmg));

  // CHEESE? + N = ENCHEErS
  bool entry_exists = wmp_move_gen_check_playthrough_full_rack_existence(&wmg);
  assert(entry_exists);

  // Save left-playthrough as N.
  wmp_move_gen_save_playthrough_state(&wmg);
  // Add a letter, P. Now we're shadowing right.
  wmp_move_gen_add_playthrough_letter(&wmg, ld_hl_to_ml(ld, "P"));

  // CHEESE? + NP = NiPCHEESE/PENnEECHS
  entry_exists = wmp_move_gen_check_playthrough_full_rack_existence(&wmg);
  assert(entry_exists);

  // Add a Q, and then there will be no bingo.
  wmp_move_gen_add_playthrough_letter(&wmg, ld_hl_to_ml(ld, "Q"));
  entry_exists = wmp_move_gen_check_playthrough_full_rack_existence(&wmg);
  assert(!entry_exists);

  // Restore left-playthrough as N.
  wmp_move_gen_restore_playthrough_state(&wmg);
  // Add an I, as if playing left.
  wmp_move_gen_add_playthrough_letter(&wmg, ld_hl_to_ml(ld, "I"));
  // CHEESE? + NI = NIpCHEESE
  entry_exists = wmp_move_gen_check_playthrough_full_rack_existence(&wmg);
  assert(entry_exists);

  // Save left-playthrough as NI.
  wmp_move_gen_save_playthrough_state(&wmg);

  // Add a P, as if playing right.
  wmp_move_gen_add_playthrough_letter(&wmg, ld_hl_to_ml(ld, "P"));
  // CHEESE? + NIP = NIPCHEESEs
  entry_exists = wmp_move_gen_check_playthrough_full_rack_existence(&wmg);
  assert(entry_exists);

  // Add a Q, and then there will be no bingo.
  wmp_move_gen_add_playthrough_letter(&wmg, ld_hl_to_ml(ld, "Q"));

  rack_destroy(rack);
  game_destroy(game);
  config_destroy(config);
}

// A WMP candidate's equity on a nonempty board depends only on its score,
// leave, the racks, and the number of tiles played, so the bounded record modes
// compute it before writing a Move and discard candidates that could not enter
// the list. Whatever they discard must be exactly what the unbounded list would
// have ranked out: the best move has the equity of the head of the full sorted
// list, a list of capacity k has the equities of its first k entries, and a
// within-x list holds every entry strictly inside the margin. Equal-equity
// moves are compared by membership rather than by position, because the
// bounded modes also let shadow skip anchors that cannot beat the running
// cutoff, and that already decides ties by anchor order.
enum { RECORD_MODES_FULL_CAPACITY = 100000 };

// A SortedMoveList borrows its Move objects from the MoveList it was built
// from, so the two live and die together.
typedef struct SortedGeneration {
  MoveList *move_list;
  SortedMoveList *sorted;
} SortedGeneration;

static SortedGeneration
generate_sorted_with_record_type(Game *game, move_record_t record_type,
                                 move_sort_t sort_type, Equity eq_margin,
                                 int capacity) {
  SortedGeneration generation;
  generation.move_list = move_list_create(capacity);
  const MoveGenArgs args = {
      .game = game,
      .move_list = generation.move_list,
      .move_record_type = record_type,
      .move_sort_type = sort_type,
      .eq_margin_movegen = eq_margin,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
  };
  // Not generate_moves_for_game(): that replaces the record type with the
  // on-turn player's configured one.
  generate_moves(&args);
  generation.sorted = sorted_move_list_create(generation.move_list);
  return generation;
}

static void sorted_generation_destroy(SortedGeneration *generation) {
  sorted_move_list_destroy(generation->sorted);
  move_list_destroy(generation->move_list);
}

static bool moves_match(const Move *move_1, const Move *move_2) {
  if (move_get_type(move_1) != move_get_type(move_2) ||
      move_get_row_start(move_1) != move_get_row_start(move_2) ||
      move_get_col_start(move_1) != move_get_col_start(move_2) ||
      move_get_dir(move_1) != move_get_dir(move_2) ||
      move_get_tiles_played(move_1) != move_get_tiles_played(move_2) ||
      move_get_tiles_length(move_1) != move_get_tiles_length(move_2) ||
      move_get_score(move_1) != move_get_score(move_2) ||
      move_get_equity(move_1) != move_get_equity(move_2)) {
    return false;
  }
  for (int tile_idx = 0; tile_idx < move_get_tiles_length(move_1); tile_idx++) {
    if (move_get_tile(move_1, tile_idx) != move_get_tile(move_2, tile_idx)) {
      return false;
    }
  }
  return true;
}

// The bounded move must be a move the full list also holds, at the same
// equity: a shortcut that let through something the regular path would have
// computed differently shows up here.
static void assert_move_in_full_list(const SortedMoveList *full,
                                     const Move *move) {
  for (int move_idx = 0; move_idx < full->count; move_idx++) {
    if (moves_match(full->moves[move_idx], move)) {
      return;
    }
  }
  assert(false);
}

static int count_moves_above(const SortedMoveList *sorted, Equity cutoff) {
  int count = 0;
  while (count < sorted->count &&
         move_get_equity(sorted->moves[count]) > cutoff) {
    count++;
  }
  return count;
}

static void assert_bounded_record_modes_match_full_list(Game *game,
                                                        Game *plain_game,
                                                        move_sort_t sort_type) {
  SortedGeneration full_generation = generate_sorted_with_record_type(
      game, MOVE_RECORD_ALL, sort_type, 0, RECORD_MODES_FULL_CAPACITY);
  const SortedMoveList *full = full_generation.sorted;
  assert(full->count > 10);

  // The full WMP list's own equities come from the precomputed value too, so
  // pin them to the generator without a WMP, whose equities come from the
  // materialized Move.
  SortedGeneration plain_generation = generate_sorted_with_record_type(
      plain_game, MOVE_RECORD_ALL, sort_type, 0, RECORD_MODES_FULL_CAPACITY);
  const SortedMoveList *plain_full = plain_generation.sorted;
  assert(plain_full->count == full->count);
  for (int move_idx = 0; move_idx < full->count; move_idx++) {
    assert(move_get_equity(full->moves[move_idx]) ==
           move_get_equity(plain_full->moves[move_idx]));
    assert_move_in_full_list(plain_full, full->moves[move_idx]);
  }
  sorted_generation_destroy(&plain_generation);

  SortedGeneration best_generation =
      generate_sorted_with_record_type(game, MOVE_RECORD_BEST, sort_type, 0, 1);
  const SortedMoveList *best = best_generation.sorted;
  assert(best->count == 1);
  assert(move_get_equity(best->moves[0]) == move_get_equity(full->moves[0]));
  assert_move_in_full_list(full, best->moves[0]);
  sorted_generation_destroy(&best_generation);

  const int capacities[] = {1, 3, 10};
  for (size_t cap_idx = 0; cap_idx < sizeof(capacities) / sizeof(capacities[0]);
       cap_idx++) {
    const int capacity = capacities[cap_idx];
    SortedGeneration bounded_generation = generate_sorted_with_record_type(
        game, MOVE_RECORD_ALL, sort_type, 0, capacity);
    const SortedMoveList *bounded = bounded_generation.sorted;
    assert(bounded->count == capacity);
    for (int move_idx = 0; move_idx < capacity; move_idx++) {
      assert(move_get_equity(bounded->moves[move_idx]) ==
             move_get_equity(full->moves[move_idx]));
      assert_move_in_full_list(full, bounded->moves[move_idx]);
    }
    sorted_generation_destroy(&bounded_generation);
  }

  // With the WMP, a score-sorted within-x list currently drops some vertical
  // plays, including ties for the best -- a pre-existing gap in that mode,
  // independent of the shortcut under test -- so the within-x check runs for
  // equity sort only.
  if (sort_type != MOVE_SORT_EQUITY) {
    sorted_generation_destroy(&full_generation);
    return;
  }
  const int margins[] = {0, 5, 20, 40};
  for (size_t margin_idx = 0; margin_idx < sizeof(margins) / sizeof(margins[0]);
       margin_idx++) {
    const Equity margin = int_to_equity(margins[margin_idx]);
    SortedGeneration within_generation = generate_sorted_with_record_type(
        game, MOVE_RECORD_WITHIN_X_EQUITY_OF_BEST, sort_type, margin,
        RECORD_MODES_FULL_CAPACITY);
    const SortedMoveList *within = within_generation.sorted;
    const Equity cutoff = move_get_equity(full->moves[0]) - margin;
    assert(within->count > 0);
    assert(move_get_equity(within->moves[0]) ==
           move_get_equity(full->moves[0]));
    assert(count_moves_above(within, cutoff) ==
           count_moves_above(full, cutoff));
    for (int move_idx = 0; move_idx < within->count; move_idx++) {
      assert(move_get_equity(within->moves[move_idx]) >= cutoff);
      assert_move_in_full_list(full, within->moves[move_idx]);
    }
    sorted_generation_destroy(&within_generation);
  }
  sorted_generation_destroy(&full_generation);
}

void test_wmp_bounded_record_modes(void) {
  const char *settings = "-s1 equity -s2 equity -r1 all -r2 all -numplays 1 "
                         "-threads 1";
  char cmd[256];
  (void)snprintf(cmd, sizeof(cmd), "set -lex CSW21 -wmp true %s", settings);
  Config *config = config_create_or_die(cmd);
  (void)snprintf(cmd, sizeof(cmd), "set -lex CSW21 -wmp false %s", settings);
  Config *plain_config = config_create_or_die(cmd);
  const char *cgps[] = {
      "cgp " DOUG_V_EMELY_CGP,
      "cgp " GUY_VS_BOT_CGP,
      "cgp " NOAH_VS_MISHU_CGP,
      "cgp " JOSH2_CGP,
      "cgp " SOME_ISC_GAME_CGP,
      "cgp " UTF8_DOS_CGP,
      "cgp " VS_FRENTZ_CGP,
      "cgp " NOAH_VS_PETER_CGP,
      "cgp " DOUG_V_EMELY_DOUBLE_CHALLENGE_CGP,
      "cgp 5U4OHMIC/5N3WREATH/5T4FAX2/5i3B1VIA1/5N3L1E3/5G2VELDT2/5E3S5/"
      "5DREKS1F3/8YELL3/4ABASER1U3/4GYM3ZO3/WAITE5OR2J/10OI2A/3QUOIT1PINNER/"
      "4RENEGADE2P CDIOST?/AIINOOU 450/392 0 -lex CSW21;",
      "cgp 5U4OHMIC/5N3WREATH/5T4FAX2/5i3B1VIA1/5N3L1E3/5G2VELDT2/5E3S5/"
      "5DREKS1F3/8YELL3/4ABASER1U3/4GYM3ZO3/WAITE5OR2J/10OI2A/3QUOIT1PINNER/"
      "4RENEGADE2P AIINOOU/CDIOS 392/450 0 -lex CSW21;",
  };
  for (size_t cgp_idx = 0; cgp_idx < sizeof(cgps) / sizeof(cgps[0]);
       cgp_idx++) {
    load_and_exec_config_or_die(config, cgps[cgp_idx]);
    load_and_exec_config_or_die(plain_config, cgps[cgp_idx]);
    Game *game = config_get_game(config);
    Game *plain_game = config_get_game(plain_config);
    assert_bounded_record_modes_match_full_list(game, plain_game,
                                                MOVE_SORT_EQUITY);
    assert_bounded_record_modes_match_full_list(game, plain_game,
                                                MOVE_SORT_SCORE);
  }
  config_destroy(plain_config);
  config_destroy(config);
}

// Shadowing right must restore the leftward playthrough state before the
// next extension is explored. Compare against the recursive generator on
// separated blocks, blanks, and edges, then reuse the generator on an empty
// board. WIT-on also exercises early exits after entering a board block.
static void test_shadow_playthrough_restoration(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp true -wit false -s1 equity -s2 equity "
      "-r1 all -r2 all -numplays 100000");
  Config *reference_config = config_create_or_die(
      "set -lex CSW21 -wmp false -wit false -s1 equity -s2 equity "
      "-r1 all -r2 all -numplays 100000");
  static const char *const positions[] = {
      "15/15/15/15/15/15/15/6CAT6/15/15/15/15/15/15/15 ??EINRS/ 0/0 0",
      "15/15/15/15/15/15/15/6CaT6/15/15/15/15/15/15/15 ?DEIRTU/ 0/0 0",
      "15/15/15/15/15/15/15/4AT2IN5/15/15/15/15/15/15/15 AEIRST?/ 0/0 0",
      "15/15/15/15/15/15/15/3A1T1I1N5/15/15/15/15/15/15/15 AEIRST?/ 0/0 0",
      "AT13/15/15/15/15/15/15/15/15/15/15/15/15/15/13IN AEIRST?/ 0/0 0",
      // This played position takes a WIT early exit after entering a block.
      ("15/5M9/5I9/5R9/4BI4P4/4ET3JIG3/4AI4N4/4K2TRaNQ1OY/4I5AINEE/4EPAULETS3/"
       "4R5E4/8ENDEmIC/15/15/15 DEEMORS/AHLOORS 195/206 0"),
      "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 AEINRST/ 0/0 0",
  };
  MoveList *list = move_list_create(100000);
  MoveList *reference_list = move_list_create(100000);
  for (int wit_enabled = 0; wit_enabled <= 1; wit_enabled++) {
    if (wit_enabled) {
      PlayersData *players_data = config_get_players_data(config);
      WordInfoTable *wit =
          make_word_info_table_from_kwg(players_data_get_kwg(players_data, 0));
      players_data_set_data(players_data, PLAYERS_DATA_TYPE_WIT, 0, wit);
      players_data_set_data(players_data, PLAYERS_DATA_TYPE_WIT, 1, wit);
    }
    Game *game = config_game_create(config);
    assert((player_get_word_info_table(game_get_player(game, 0)) != NULL) ==
           (wit_enabled != 0));
    Game *reference = config_game_create(reference_config);
    for (size_t position_idx = 0;
         position_idx < sizeof(positions) / sizeof(positions[0]);
         position_idx++) {
      load_cgp_or_die(game, positions[position_idx]);
      load_cgp_or_die(reference, positions[position_idx]);
      const MoveGenArgs args = {
          .game = game,
          .move_list = list,
          .target_equity = EQUITY_MAX_VALUE,
          .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
      };
      MoveGenArgs reference_args = args;
      reference_args.game = reference;
      reference_args.move_list = reference_list;
      generate_moves_for_game(&args);
      generate_moves_for_game(&reference_args);
      SortedMoveList *sorted = sorted_move_list_create(list);
      SortedMoveList *reference_sorted =
          sorted_move_list_create(reference_list);
      assert(reference_sorted->count > 0);
      assert(sorted->count == reference_sorted->count);
      for (int move_idx = 0; move_idx < sorted->count; move_idx++) {
        assert_moves_are_equal(sorted->moves[move_idx],
                               reference_sorted->moves[move_idx]);
      }
      sorted_move_list_destroy(sorted);
      sorted_move_list_destroy(reference_sorted);
    }
    game_destroy(game);
    game_destroy(reference);
  }
  move_list_destroy(list);
  move_list_destroy(reference_list);
  config_destroy(config);
  config_destroy(reference_config);
}

// Compare complete move sets against the independent recursive generator.
// Reuse the same generator across empty boards, split blocks, board blanks,
// edge words, and racks containing one or two blanks.
static void test_playthrough_moves_against_recursive(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 100000");
  Config *reference = config_create_or_die(
      "set -lex CSW21 -wmp false -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 100000");
  static const char *const positions[] = {
      "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 AEINRST/ 0/0 0",
      "15/15/15/15/15/15/15/6CAT6/15/15/15/15/15/15/15 ??EINRS/ 0/0 0",
      "15/15/15/15/15/15/15/6CaT6/15/15/15/15/15/15/15 ?DEIRTU/ 0/0 0",
      "15/15/15/15/15/15/15/4AT1IN6/15/15/15/15/15/15/15 AEIRST?/ 0/0 0",
      "AT13/15/15/15/15/15/15/15/15/15/15/15/15/15/13IN AEIRST?/ 0/0 0",
      "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 AEINRST/ 0/0 0",
  };
  MoveList *list = move_list_create(100000);
  MoveList *reference_list = move_list_create(100000);
  for (size_t position_idx = 0;
       position_idx < sizeof(positions) / sizeof(positions[0]);
       position_idx++) {
    char command[256];
    (void)snprintf(command, sizeof(command), "cgp %s", positions[position_idx]);
    load_and_exec_config_or_die(config, command);
    load_and_exec_config_or_die(reference, command);
    const MoveGenArgs args = {
        .game = config_get_game(config),
        .move_list = list,
        .target_equity = EQUITY_MAX_VALUE,
        .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
    };
    MoveGenArgs reference_args = args;
    reference_args.game = config_get_game(reference);
    reference_args.move_list = reference_list;
    generate_moves_for_game(&args);
    generate_moves_for_game(&reference_args);
    SortedMoveList *sorted = sorted_move_list_create(list);
    SortedMoveList *reference_sorted = sorted_move_list_create(reference_list);
    assert(reference_sorted->count > 0);
    assert(sorted->count == reference_sorted->count);
    for (int move_idx = 0; move_idx < sorted->count; move_idx++) {
      assert_moves_are_equal(sorted->moves[move_idx],
                             reference_sorted->moves[move_idx]);
    }
    sorted_move_list_destroy(sorted);
    sorted_move_list_destroy(reference_sorted);
  }
  move_list_destroy(list);
  move_list_destroy(reference_list);
  config_destroy(config);
  config_destroy(reference);
}

// Under score sort the cutoff is a score, so a subrack's leave value must take
// no part in the bound that decides whether the subrack can still beat the
// cutoff (#673). This drives the WMP generator through the cutoff-based record
// modes on a set of positions in both sort types and checks each result
// against the full list: the best move carries the head's equity, and a
// within-x list holds every play strictly inside the margin. Equal-equity
// ties are compared by membership rather than position, since shadow lets the
// bounded modes skip anchors that cannot beat the running cutoff and that
// decides ties by anchor order.
enum { CUTOFF_MODES_FULL_CAPACITY = 100000 };

// A SortedMoveList borrows its Move objects from the MoveList it was built
// from, so the two live and die together.
typedef struct CutoffModesGeneration {
  MoveList *move_list;
  SortedMoveList *sorted;
} CutoffModesGeneration;

static CutoffModesGeneration
cutoff_modes_generate(Game *game, move_record_t record_type,
                      move_sort_t sort_type, Equity eq_margin, int capacity) {
  CutoffModesGeneration generation;
  generation.move_list = move_list_create(capacity);
  const MoveGenArgs args = {
      .game = game,
      .move_list = generation.move_list,
      .move_record_type = record_type,
      .move_sort_type = sort_type,
      .eq_margin_movegen = eq_margin,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
  };
  // Not generate_moves_for_game(): that replaces the record type with the
  // on-turn player's configured one.
  generate_moves(&args);
  generation.sorted = sorted_move_list_create(generation.move_list);
  return generation;
}

static void cutoff_modes_generation_destroy(CutoffModesGeneration *generation) {
  sorted_move_list_destroy(generation->sorted);
  move_list_destroy(generation->move_list);
}

static bool cutoff_modes_moves_match(const Move *move_1, const Move *move_2) {
  if (move_get_type(move_1) != move_get_type(move_2) ||
      move_get_row_start(move_1) != move_get_row_start(move_2) ||
      move_get_col_start(move_1) != move_get_col_start(move_2) ||
      move_get_dir(move_1) != move_get_dir(move_2) ||
      move_get_tiles_played(move_1) != move_get_tiles_played(move_2) ||
      move_get_tiles_length(move_1) != move_get_tiles_length(move_2) ||
      move_get_score(move_1) != move_get_score(move_2) ||
      move_get_equity(move_1) != move_get_equity(move_2)) {
    return false;
  }
  for (int tile_idx = 0; tile_idx < move_get_tiles_length(move_1); tile_idx++) {
    if (move_get_tile(move_1, tile_idx) != move_get_tile(move_2, tile_idx)) {
      return false;
    }
  }
  return true;
}

static void cutoff_modes_assert_in_list(const SortedMoveList *full,
                                        const Move *move) {
  for (int move_idx = 0; move_idx < full->count; move_idx++) {
    if (cutoff_modes_moves_match(full->moves[move_idx], move)) {
      return;
    }
  }
  assert(false);
}

static int cutoff_modes_count_above(const SortedMoveList *sorted,
                                    Equity cutoff) {
  int count = 0;
  while (count < sorted->count &&
         move_get_equity(sorted->moves[count]) > cutoff) {
    count++;
  }
  return count;
}

static void assert_cutoff_modes_are_complete(Game *game,
                                             move_sort_t sort_type) {
  CutoffModesGeneration full_generation = cutoff_modes_generate(
      game, MOVE_RECORD_ALL, sort_type, 0, CUTOFF_MODES_FULL_CAPACITY);
  const SortedMoveList *full = full_generation.sorted;
  assert(full->count > 10);

  CutoffModesGeneration best_generation =
      cutoff_modes_generate(game, MOVE_RECORD_BEST, sort_type, 0, 1);
  const SortedMoveList *best = best_generation.sorted;
  assert(best->count == 1);
  assert(move_get_equity(best->moves[0]) == move_get_equity(full->moves[0]));
  cutoff_modes_assert_in_list(full, best->moves[0]);
  cutoff_modes_generation_destroy(&best_generation);

  const int margins[] = {0, 5, 20, 40};
  const int num_margins = 4;
  for (int margin_idx = 0; margin_idx < num_margins; margin_idx++) {
    const Equity margin = int_to_equity(margins[margin_idx]);
    CutoffModesGeneration within_generation =
        cutoff_modes_generate(game, MOVE_RECORD_WITHIN_X_EQUITY_OF_BEST,
                              sort_type, margin, CUTOFF_MODES_FULL_CAPACITY);
    const SortedMoveList *within = within_generation.sorted;
    const Equity cutoff = move_get_equity(full->moves[0]) - margin;
    assert(within->count > 0);
    assert(move_get_equity(within->moves[0]) ==
           move_get_equity(full->moves[0]));
    assert(cutoff_modes_count_above(within, cutoff) ==
           cutoff_modes_count_above(full, cutoff));
    for (int move_idx = 0; move_idx < within->count; move_idx++) {
      assert(move_get_equity(within->moves[move_idx]) >= cutoff);
      cutoff_modes_assert_in_list(full, within->moves[move_idx]);
    }
    cutoff_modes_generation_destroy(&within_generation);
  }
  cutoff_modes_generation_destroy(&full_generation);
}

void test_wmp_cutoff_modes_are_complete(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 1 -threads 1");
  // Midgame boards with a full bag, racks holding blanks and fewer than seven
  // tiles, late boards with a short bag, the same endgame board with the bag
  // empty and with two tiles left, and an opening rack on an empty board.
  const char *cgps[] = {
      "cgp " DOUG_V_EMELY_CGP,
      "cgp " GUY_VS_BOT_CGP,
      "cgp " NOAH_VS_MISHU_CGP,
      "cgp " JOSH2_CGP,
      "cgp " SOME_ISC_GAME_CGP,
      "cgp " UTF8_DOS_CGP,
      "cgp " VS_FRENTZ_CGP,
      "cgp " NOAH_VS_PETER_CGP,
      "cgp " DOUG_V_EMELY_DOUBLE_CHALLENGE_CGP,
      "cgp 5U4OHMIC/5N3WREATH/5T4FAX2/5i3B1VIA1/5N3L1E3/5G2VELDT2/5E3S5/"
      "5DREKS1F3/8YELL3/4ABASER1U3/4GYM3ZO3/WAITE5OR2J/10OI2A/3QUOIT1PINNER/"
      "4RENEGADE2P CDIOST?/AIINOOU 450/392 0 -lex CSW21;",
      "cgp 5U4OHMIC/5N3WREATH/5T4FAX2/5i3B1VIA1/5N3L1E3/5G2VELDT2/5E3S5/"
      "5DREKS1F3/8YELL3/4ABASER1U3/4GYM3ZO3/WAITE5OR2J/10OI2A/3QUOIT1PINNER/"
      "4RENEGADE2P AIINOOU/CDIOS 392/450 0 -lex CSW21;",
  };
  const int num_cgps = 11;
  for (int cgp_idx = 0; cgp_idx < num_cgps; cgp_idx++) {
    load_and_exec_config_or_die(config, cgps[cgp_idx]);
    Game *game = config_get_game(config);
    assert_cutoff_modes_are_complete(game, MOVE_SORT_EQUITY);
    assert_cutoff_modes_are_complete(game, MOVE_SORT_SCORE);
  }
  config_destroy(config);
}

void test_wmp_maximum_playthrough_blocks(void) {
  WMPMoveGen wmg = {0};
  for (int block = 0; block < MAX_POSSIBLE_PLAYTHROUGH_BLOCKS; block++) {
    wmp_move_gen_increment_playthrough_blocks(&wmg);
  }
  wmp_move_gen_maybe_update_anchor(&wmg, RACK_SIZE,
                                   MAX_POSSIBLE_PLAYTHROUGH_BLOCKS + RACK_SIZE,
                                   0, int_to_equity(50), int_to_equity(50));
  const Anchor *anchor =
      wmp_move_gen_get_anchor(&wmg, MAX_POSSIBLE_PLAYTHROUGH_BLOCKS, RACK_SIZE);
  assert(anchor->tiles_to_play == RACK_SIZE);
  assert(anchor->playthrough_blocks == MAX_POSSIBLE_PLAYTHROUGH_BLOCKS);
  wmp_move_gen_reset_anchors(&wmg);
  assert(anchor->tiles_to_play == 0);
}

// Metadata remains current even when an anchor is rejected before any
// combined row is built. Model each rejection boundary, then reuse rows for
// a different block/size and compare the words with the direct WMP API.
static void test_playthrough_preparation_after_rejection(void) {
  Config *config = config_create_or_die("set -lex CSW21 -wmp true");
  Game *game = config_game_create(config);
  const LetterDistribution *ld = game_get_ld(game);
  const WMP *wmp = player_get_wmp(game_get_player(game, 0));
  Rack *rack = rack_create(ld_get_size(ld));
  rack_set_to_string(ld, rack, "C??");
  WMPMoveGen wmg = {0};
  wmp_move_gen_init(&wmg, ld, rack, wmp);
  MachineLetter *expected_words = malloc_or_die(wmp->max_word_lookup_bytes);
  const struct {
    const char *block;
    const char *subracks[2];
  } cases[] = {
      {"AT", {"C", "?"}},    {"A", {"C?", "??"}}, {"IT", {"?", "C"}},
      {"ZZ", {"C?", "??"}},  {"AA", {"C", "?"}},  {"AT", {"??", "C?"}},
      {"AT", {"C??", NULL}},
  };
  for (size_t case_idx = 0; case_idx < sizeof(cases) / sizeof(cases[0]);
       case_idx++) {
    // Shadow can populate a scratch row independently of generation.
    wmg.playthrough_bit_rack = string_to_bit_rack(ld, "QI");
    wmg.num_tiles_played_through = 2;
    (void)wmp_move_gen_check_playthrough_full_rack_existence(&wmg);
    for (int rejection = 0; rejection < 2; rejection++) {
      const Anchor skipped = {.tiles_to_play = 3, .word_length = 5};
      wmg.playthrough_bit_rack = string_to_bit_rack(ld, "QI");
      wmg.playthrough_addable = rejection == 0 ? 0 : (1U << 1);
      wmp_move_gen_playthrough_metadata_init(&wmg, &skipped);
      assert(wmg.word_length == 5 && wmg.tiles_to_play == 3 &&
             wmg.num_tiles_played_through == 2);
      if (rejection == 0) {
        assert(wmg.playthrough_addable == 0);
      } else {
        // Only the two rack blanks are usable when A is the sole addable
        // letter, but this rejected anchor needs three tiles.
        assert(bit_rack_get_letter(&wmg.player_bit_rack, 1) == 0);
        assert(bit_rack_get_letter(&wmg.player_bit_rack, BLANK_MACHINE_LETTER) <
               skipped.tiles_to_play);
      }
      // The rejected path omits the builder. A later surviving anchor must
      // not trust any scratch row left by a different length or shadow.
      const int size = (int)strlen(cases[case_idx].subracks[0]);
      const int offset = subracks_get_combination_offset(size);
      const int count = cases[case_idx].subracks[1] != NULL ? 2 : 1;
      wmg.count_by_size[size] = (uint8_t)count;
      for (int idx = 0; idx < count; idx++) {
        wmg.nonplaythrough_infos[offset + idx].subrack =
            string_to_bit_rack(ld, cases[case_idx].subracks[idx]);
      }
      const Anchor anchor = {
          .tiles_to_play = (unsigned int)size,
          .word_length = (unsigned int)(size + strlen(cases[case_idx].block))};
      wmg.playthrough_bit_rack = string_to_bit_rack(ld, cases[case_idx].block);
      wmp_move_gen_playthrough_metadata_init(&wmg, &anchor);
      assert(wmp_move_gen_get_num_subrack_combinations(&wmg) == count);
      wmp_move_gen_build_playthrough_subracks(&wmg);
      for (int idx = count - 1; idx >= 0; idx--) {
        BitRack expected =
            string_to_bit_rack(ld, cases[case_idx].subracks[idx]);
        const BitRack canonical = expected;
        bit_rack_add_bit_rack(&expected, &wmg.playthrough_bit_rack);
        const int expected_bytes = wmp_write_words_to_buffer(
            wmp, &expected, (int)anchor.word_length, expected_words);
        const bool found = wmp_move_gen_get_subrack_words(&wmg, idx, true);
        assert(found == (expected_bytes > 0));
        if (found) {
          assert(wmg.num_words * wmg.word_length == expected_bytes);
          assert(wmg.words != NULL);
          assert(memcmp(wmg.words, expected_words, (size_t)expected_bytes) ==
                 0);
        }
        assert(bit_rack_equals(
            &canonical, &wmg.nonplaythrough_infos[offset + idx].subrack));
      }
      // The original one-call initializer keeps its complete semantics for
      // callers that do not need to put an anchor-level check in between.
      wmp_move_gen_playthrough_subracks_init(&wmg, &anchor);
      for (int idx = 0; idx < count; idx++) {
        BitRack expected =
            string_to_bit_rack(ld, cases[case_idx].subracks[idx]);
        bit_rack_add_bit_rack(&expected, &wmg.playthrough_bit_rack);
        assert(bit_rack_equals(&expected,
                               &wmg.playthrough_infos[offset + idx].subrack));
      }
    }
  }
  free(expected_words);
  rack_destroy(rack);
  game_destroy(game);
  config_destroy(config);
}

void test_wmp_move_gen(void) {
  test_playthrough_preparation_after_rejection();
  test_wmp_maximum_playthrough_blocks();
  test_word_plus_floater_positional_intersection();
  test_word_plus_floater_dense_coverage();
  test_wmp_move_gen_inactive();
  test_shadow_playthrough_restoration();
  test_sparse_anchor_slot_order_and_reset();
  test_playthrough_positions_reset();
  test_playthrough_moves_against_recursive();
  test_nonplaythrough_subrack_enumeration();
  test_wit_prune_skips_block_longer_than_anchor_word();
  test_nonplaythrough_existence();
  test_playthrough_bingo_existence();
  test_wmp_bounded_record_modes();
  test_wmp_cutoff_modes_are_complete();
}

// The RIT-backed path resolves nonplaythrough WMP entries lazily at record
// time; the non-RIT path resolves them eagerly during shadow. Move generation
// must not be able to tell the difference. Two games are driven in lockstep
// from identical seeds, one with a RIT and one without, and at every
// position the complete sorted move lists must match move for move.
//
// Needs <lexicon>.rit on the data path. The ap_rit CI shard builds it for
// TWL98 before running this; test_autoplay_rit_correctness removes it after.
// Stops at the first recorded play, as inference-mode generation does when a
// play beats its target. Most subracks are then never asked for their words,
// which is what leaves their lazily resolved entries unresolved.
static void generate_until_first_play(Game *game, MoveList *move_list) {
  const MoveGenArgs args = {
      .game = game,
      .move_list = move_list,
      .move_record_type = MOVE_RECORD_BEST,
      .move_sort_type = MOVE_SORT_EQUITY,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MIN_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
  };
  generate_moves_for_game(&args);
}

static void generate_all_sorted(Game *game, MoveList *move_list,
                                SortedMoveList **sorted_out) {
  const MoveGenArgs args = {
      .game = game,
      .move_list = move_list,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
  };
  generate_moves_for_game(&args);
  *sorted_out = sorted_move_list_create(move_list);
}

// The per-thread subrack cache outlives set commands. A rack generated while
// the RIT was loaded leaves lazily resolved entries in it that the eager path
// must never see; switching the RIT invalidates the cache, and this keeps it
// that way: generating the same rack across `set -rit false` and back must
// match a generator that never had a RIT, from a config whose WMP object --
// the cache's other key -- does not change across the switch.
void test_rit_toggle_subrack_cache(void) {
  const char *settings = "-wmp true -s1 equity -s2 equity -r1 all -r2 all "
                         "-numplays 100000 -threads 1";
  char cmd[256];
  (void)snprintf(cmd, sizeof(cmd), "set -lex TWL98 -rit true %s", settings);
  Config *config = config_create_or_die(cmd);
  (void)snprintf(cmd, sizeof(cmd), "set -lex TWL98 -rit false %s", settings);
  Config *plain_config = config_create_or_die(cmd);
  static const char *const cgps[] = {
      "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 AEINRST/ 0/0 0",
      "15/15/15/15/15/15/15/6CAT6/15/15/15/15/15/15/15 AEINRST/ 0/0 0",
      "15/15/15/15/15/15/15/6CAT6/15/15/15/15/15/15/15 ??EINRS/ 0/0 0",
  };
  // Use each config's own game: `set -rit ...` updates that game's players,
  // and the config only creates it once a command needs one.
  char cgp_cmd[256];
  (void)snprintf(cgp_cmd, sizeof(cgp_cmd), "cgp %s", cgps[0]);
  load_and_exec_config_or_die(config, cgp_cmd);
  load_and_exec_config_or_die(plain_config, cgp_cmd);
  Game *game = config_get_game(config);
  Game *plain_game = config_get_game(plain_config);
  assert(game != NULL && plain_game != NULL);
  assert(player_get_rack_info_table(game_get_player(game, 0)) != NULL);
  assert(player_get_rack_info_table(game_get_player(plain_game, 0)) == NULL);
  MoveList *list = move_list_create(100000);
  MoveList *plain_list = move_list_create(100000);
  for (size_t cgp_idx = 0; cgp_idx < sizeof(cgps) / sizeof(cgps[0]);
       cgp_idx++) {
    (void)snprintf(cgp_cmd, sizeof(cgp_cmd), "cgp %s", cgps[cgp_idx]);
    load_and_exec_config_or_die(config, cgp_cmd);
    load_and_exec_config_or_die(plain_config, cgp_cmd);
    game = config_get_game(config);
    plain_game = config_get_game(plain_config);
    SortedMoveList *sorted = NULL;
    SortedMoveList *plain_sorted = NULL;
    // RIT on, stopping at the first recorded play: most subracks never reach
    // record time, so their cached entries stay unresolved -- the state the
    // switch must not carry over.
    generate_until_first_play(game, list);
    // RIT off, same config and WMP, recording everything. A set command
    // updates players_data only; the next game command refreshes the game's
    // players from it, so reload the position before reading the game.
    load_and_exec_config_or_die(config, "set -rit false");
    load_and_exec_config_or_die(config, cgp_cmd);
    game = config_get_game(config);
    assert(player_get_rack_info_table(game_get_player(game, 0)) == NULL);
    generate_all_sorted(game, list, &sorted);
    generate_all_sorted(plain_game, plain_list, &plain_sorted);
    assert(sorted->count == plain_sorted->count);
    assert(plain_sorted->count > 0);
    for (int move_idx = 0; move_idx < plain_sorted->count; move_idx++) {
      assert_moves_are_equal(sorted->moves[move_idx],
                             plain_sorted->moves[move_idx]);
    }
    sorted_move_list_destroy(sorted);
    // And back on: resolved entries in a lazily resolving generation.
    load_and_exec_config_or_die(config, "set -rit true");
    load_and_exec_config_or_die(config, cgp_cmd);
    game = config_get_game(config);
    assert(player_get_rack_info_table(game_get_player(game, 0)) != NULL);
    generate_all_sorted(game, list, &sorted);
    assert(sorted->count == plain_sorted->count);
    for (int move_idx = 0; move_idx < plain_sorted->count; move_idx++) {
      assert_moves_are_equal(sorted->moves[move_idx],
                             plain_sorted->moves[move_idx]);
    }
    sorted_move_list_destroy(sorted);
    sorted_move_list_destroy(plain_sorted);
  }
  move_list_destroy(list);
  move_list_destroy(plain_list);
  config_destroy(plain_config);
  config_destroy(config);
}

void test_rit_movegen_equality(void) {
  const char *settings = "-wmp true -s1 equity -s2 equity -r1 all -r2 all "
                         "-numplays 100000 -threads 1";
  char cmd[256];
  (void)snprintf(cmd, sizeof(cmd), "set -lex TWL98 -rit true %s", settings);
  Config *rit_config = config_create_or_die(cmd);
  (void)snprintf(cmd, sizeof(cmd), "set -lex TWL98 -rit false %s", settings);
  Config *plain_config = config_create_or_die(cmd);
  Game *rit_game = config_game_create(rit_config);
  Game *plain_game = config_game_create(plain_config);
  // The test is only meaningful if the RIT actually loaded on one side and
  // not the other; assert it so a missing file cannot pass vacuously.
  assert(player_get_rack_info_table(game_get_player(rit_game, 0)) != NULL);
  assert(player_get_rack_info_table(game_get_player(plain_game, 0)) == NULL);

  MoveList *rit_list = move_list_create(100000);
  MoveList *plain_list = move_list_create(100000);
  int positions = 0;
  long moves = 0;
  for (uint64_t seed = 1; seed <= 6; seed++) {
    game_reset(rit_game);
    game_reset(plain_game);
    game_seed(rit_game, seed);
    game_seed(plain_game, seed);
    draw_starting_racks(rit_game);
    draw_starting_racks(plain_game);
    for (int player_idx = 0; player_idx < 2; player_idx++) {
      assert(racks_are_equal(
          player_get_rack(game_get_player(rit_game, player_idx)),
          player_get_rack(game_get_player(plain_game, player_idx))));
    }
    int turn = 0;
    while (!game_over(plain_game)) {
      SortedMoveList *rit_sorted = NULL;
      SortedMoveList *plain_sorted = NULL;
      generate_all_sorted(rit_game, rit_list, &rit_sorted);
      generate_all_sorted(plain_game, plain_list, &plain_sorted);
      assert(rit_sorted->count == plain_sorted->count);
      assert(plain_sorted->count > 0);
      for (int move_idx = 0; move_idx < plain_sorted->count; move_idx++) {
        assert_moves_are_equal(rit_sorted->moves[move_idx],
                               plain_sorted->moves[move_idx]);
      }
      moves += plain_sorted->count;
      positions++;
      // Same (asserted-equal) move on both sides keeps the games in lockstep.
      play_move(rit_sorted->moves[0], rit_game, NULL);
      play_move(plain_sorted->moves[0], plain_game, NULL);
      sorted_move_list_destroy(rit_sorted);
      sorted_move_list_destroy(plain_sorted);
      turn++;
      assert(turn < 200);
    }
    assert(game_over(rit_game));
  }
  assert(positions > 0);
  printf("RIT movegen equality: %d positions, %ld moves identical\n", positions,
         moves);

  move_list_destroy(rit_list);
  move_list_destroy(plain_list);
  game_destroy(rit_game);
  game_destroy(plain_game);
  config_destroy(rit_config);
  config_destroy(plain_config);
}
