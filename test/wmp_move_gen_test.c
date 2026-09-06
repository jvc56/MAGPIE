#include "wmp_move_gen_test.h"

#include "../src/def/board_defs.h"
#include "../src/def/equity_defs.h"
#include "../src/def/kwg_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/move_defs.h"
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
#include "../src/ent/rack.h"
#include "../src/ent/wmp.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"
#include "../src/impl/wmp_move_gen.h"
#include "test_util.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
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

void test_wmp_move_gen(void) {
  test_wmp_move_gen_inactive();
  test_nonplaythrough_subrack_enumeration();
  test_wit_prune_skips_block_longer_than_anchor_word();
  test_nonplaythrough_existence();
  test_playthrough_bingo_existence();
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
      "15/15/15/15/15/15/15/6CAT6/15/15/15/15/15/15/15 ?DEIRTU/ 0/0 0",
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
