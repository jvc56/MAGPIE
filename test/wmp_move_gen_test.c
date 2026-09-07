#include "wmp_move_gen_test.h"

#include "../src/def/board_defs.h"
#include "../src/def/kwg_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/rack_defs.h"
#include "../src/ent/anchor.h"
#include "../src/ent/bit_rack.h"
#include "../src/ent/board.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/leave_map.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/wmp.h"
#include "../src/impl/config.h"
#include "../src/impl/move_gen.h"
#include "../src/impl/wmp_move_gen.h"
#include "test_constants.h"
#include "test_util.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

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
                                        wit_len_lane);

  // The cached block is not wholly contained in this shorter shadow word, so
  // it cannot constrain the optional WIT prune.
  assert(wmg.playthrough_addable == UINT32_MAX);
  assert(wmg.num_tiles_played_through == 3);
  // WIT-disabled callers supply no row lane. Collect playthrough letters
  // without touching any cached table storage.
  wmp_move_gen_set_playthrough_bit_rack(&wmg, &anchor, row_cache, NULL, NULL);
  assert(wmg.playthrough_addable == UINT32_MAX);
  assert(wmg.num_tiles_played_through == 3);
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

  const int full_rack_counts[RACK_SIZE + 1] = {1, 5, 12, 18, 18, 12, 5, 1};
  assert_nonplaythrough_subrack_enumeration(ld, wmp, "AABEE?Z",
                                            full_rack_counts);

  const int short_rack_counts[RACK_SIZE + 1] = {1, 3, 4, 3, 1, 0, 0, 0};
  assert_nonplaythrough_subrack_enumeration(ld, wmp, "AA?Z", short_rack_counts);

  const int empty_counts[RACK_SIZE + 1] = {1};
  assert_nonplaythrough_subrack_enumeration(ld, wmp, "", empty_counts);
  const int single_counts[RACK_SIZE + 1] = {1, 1};
  assert_nonplaythrough_subrack_enumeration(ld, wmp, "?", single_counts);
  const int repeated_counts[RACK_SIZE + 1] = {1, 1, 1, 1, 1, 1, 1, 1};
  assert_nonplaythrough_subrack_enumeration(ld, wmp, "AAAAAAA",
                                            repeated_counts);
  const int distinct_counts[RACK_SIZE + 1] = {1, 7, 21, 35, 35, 21, 7, 1};
  assert_nonplaythrough_subrack_enumeration(ld, wmp, "?AEIOUZ",
                                            distinct_counts);
  const int two_blank_counts[RACK_SIZE + 1] = {1, 6, 16, 25, 25, 16, 6, 1};
  assert_nonplaythrough_subrack_enumeration(ld, wmp, "??AEIOZ",
                                            two_blank_counts);

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

void test_wmp_move_gen(void) {
  test_wmp_move_gen_inactive();
  test_nonplaythrough_subrack_enumeration();
  test_wit_prune_skips_block_longer_than_anchor_word();
  test_nonplaythrough_existence();
  test_playthrough_bingo_existence();
  test_wmp_cutoff_modes_are_complete();
}
