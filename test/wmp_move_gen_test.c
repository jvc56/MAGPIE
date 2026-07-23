
#include "wmp_move_gen_test.h"

#include "../src/def/board_defs.h"
#include "../src/def/equity_defs.h"
#include "../src/def/kwg_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/rack_defs.h"
#include "../src/ent/anchor.h"
#include "../src/ent/bit_rack.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/leave_map.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/wmp.h"
#include "../src/impl/config.h"
#include "../src/impl/wmp_move_gen.h"
#include "test_util.h"
#include <assert.h>
#include <stdlib.h>

void test_wmp_move_gen_inactive(void) {
  WMPMoveGen wmg;
  // Only wmp is checked by wmp_move_gen_is_active
  // No wmp -> wmp_move_gen unactive and not used by move_gen
  wmp_move_gen_init(&wmg, /*ld=*/NULL, /*rack=*/NULL, /*wmp=*/NULL,
                    /*anchor_slots_initialized=*/NULL);
  assert(!wmp_move_gen_is_active(&wmg));
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

void test_anchor_slot_initialization_lifecycle(void) {
  Config *config = config_create_or_die("set -lex CSW21 -wmp true");
  Game *game = config_game_create(config);
  const Player *player = game_get_player(game, 0);
  const LetterDistribution *ld = game_get_ld(game);
  const Rack *rack = player_get_rack(player);
  const WMP *wmp = player_get_wmp(player);

  WMPMoveGen wmg;
  bool anchor_slots_initialized = false;

  // An inactive first call must not claim to have seeded the slots.
  wmp_move_gen_init(&wmg, ld, rack, /*wmp=*/NULL, &anchor_slots_initialized);
  assert(!anchor_slots_initialized);

  wmp_move_gen_init(&wmg, ld, rack, wmp, &anchor_slots_initialized);
  assert(anchor_slots_initialized);
  for (int i = 0; i < MAX_WMP_MOVE_GEN_ANCHORS; i++) {
    assert(wmg.anchors[i].highest_possible_equity == EQUITY_MIN_VALUE);
    assert(wmg.anchors[i].highest_possible_score == EQUITY_MIN_VALUE);
    assert(wmg.anchors[i].rightmost_start_col == 0);
    assert(wmg.anchors[i].leftmost_start_col == BOARD_DIM - 1);
    assert(wmg.anchors[i].tiles_to_play == 0);
  }

  // Touch the final slot. This occupies a second mask word in BOARD_DIM=21
  // builds and verifies that reinitialization resets every tracker word.
  wmg.playthrough_blocks = MAX_POSSIBLE_PLAYTHROUGH_BLOCKS - 1;
  wmp_move_gen_maybe_update_anchor(&wmg, RACK_SIZE, BOARD_DIM, BOARD_DIM - 1,
                                   int_to_equity(-10), int_to_equity(-9));
  const int final_slot = MAX_WMP_MOVE_GEN_ANCHORS - 1;
  assert((wmg.touched_anchor_masks[final_slot / 64] &
          (1ULL << (final_slot % 64))) != 0);

  // Disabling WMP leaves the tracker intact. The next active initialization
  // must consume it before any mask is cleared.
  wmp_move_gen_init(&wmg, ld, rack, /*wmp=*/NULL, &anchor_slots_initialized);
  assert(!wmp_move_gen_is_active(&wmg));
  assert((wmg.touched_anchor_masks[final_slot / 64] &
          (1ULL << (final_slot % 64))) != 0);

  wmp_move_gen_init(&wmg, ld, rack, wmp, &anchor_slots_initialized);
  assert(wmp_move_gen_is_active(&wmg));
  for (int i = 0; i < WMP_ANCHOR_MASK_WORDS; i++) {
    assert(wmg.touched_anchor_masks[i] == 0);
  }
  const Anchor *reset = &wmg.anchors[final_slot];
  assert(reset->highest_possible_equity == EQUITY_MIN_VALUE);
  assert(reset->highest_possible_score == EQUITY_MIN_VALUE);
  assert(reset->rightmost_start_col == 0);
  assert(reset->leftmost_start_col == BOARD_DIM - 1);
  assert(reset->tiles_to_play == 0);

  game_destroy(game);
  config_destroy(config);
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
  assert(rack_size <= RACK_SIZE);

  WMPMoveGen wmg;
  wmp_move_gen_init(&wmg, ld, rack, wmp,
                    /*anchor_slots_initialized=*/NULL);
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
      assert(expected_idx >= 0);
      assert(expected_sizes[expected_idx] == size);
      assert(!expected_seen[expected_idx]);
      expected_seen[expected_idx] = true;
      assert(!info->wmp_entry_is_set);

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

  const int full_rack_counts[RACK_SIZE + 1] = {1, 5, 12, 18, 18, 12, 5, 1};
  assert_nonplaythrough_subrack_enumeration(ld, wmp, "AABEE?Z",
                                            full_rack_counts);

  const int short_rack_counts[RACK_SIZE + 1] = {1, 3, 4, 3, 1, 0, 0, 0};
  assert_nonplaythrough_subrack_enumeration(ld, wmp, "AA?Z", short_rack_counts);

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

  wmp_move_gen_init(&wmg, ld, rack, wmp,
                    /*anchor_slots_initialized=*/NULL);
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

  wmp_move_gen_init(&wmg, ld, rack, wmp,
                    /*anchor_slots_initialized=*/NULL);
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

void test_wmp_move_gen(void) {
  test_wmp_move_gen_inactive();
  test_sparse_anchor_slot_order_and_reset();
  test_anchor_slot_initialization_lifecycle();
  test_nonplaythrough_subrack_enumeration();
  test_nonplaythrough_existence();
  test_playthrough_bingo_existence();
}
