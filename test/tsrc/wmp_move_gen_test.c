
#include <assert.h>

#include "wmp_move_gen_test.h"

#include "../../src/impl/config.h"
#include "../../src/impl/wmp_move_gen.h"

#include "test_util.h"

void test_wmp_move_gen_inactive(void) {
  WMPMoveGen wmg;
  // Only wmp is checked by wmp_move_gen_is_active
  // No wmp -> wmp_move_gen unactive and not used by move_gen
  wmp_move_gen_init(&wmg, /*ld=*/NULL, /*rack=*/NULL, /*wmp=*/NULL);
  assert(!wmp_move_gen_is_active(&wmg));
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
  assert(wmp_move_gen_is_active(&wmg));
  assert(!wmp_move_gen_has_playthrough(&wmg));

  // Values not used for check_leaves=false, but
  // wmp_move_gen_check_nonplaythrough_existence moves the leave_map idx even
  // when not checking leaves.
  set_dummy_leave_values(&leave_map);

  wmp_move_gen_check_nonplaythrough_existence(&wmg, /*check_leaves=*/false,
                                              &leave_map);

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
    assert(best_leaves[len] == 0);
  }

  wmp_move_gen_check_nonplaythrough_existence(&wmg, /*check_leaves=*/true,
                                              &leave_map);
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
  assert(wmp_move_gen_is_active(&wmg));
  assert(!wmp_move_gen_has_playthrough(&wmg));
  // Add a letter, N. In this context we would be shadowing left.
  wmp_move_gen_add_playthrough_letter(&wmg, ld_hl_to_ml(ld, "N"));
  assert(wmp_move_gen_has_playthrough(&wmg));

  // CHEESE? + N = ENCHEErS
  assert(wmp_move_gen_check_playthrough_full_rack_existence(&wmg));

  // Save left-playthrough as N.
  wmp_move_gen_save_playthrough_state(&wmg);
  // Add a letter, P. Now we're shadowing right.
  wmp_move_gen_add_playthrough_letter(&wmg, ld_hl_to_ml(ld, "P"));

  // CHEESE? + NP = NiPCHEESE/PENnEECHS
  assert(wmp_move_gen_check_playthrough_full_rack_existence(&wmg));

  // Add a Q, and then there will be no bingo.
  wmp_move_gen_add_playthrough_letter(&wmg, ld_hl_to_ml(ld, "Q"));
  assert(!wmp_move_gen_check_playthrough_full_rack_existence(&wmg));

  // Restore left-playthrough as N.
  wmp_move_gen_restore_playthrough_state(&wmg);
  // Add an I, as if playing left.
  wmp_move_gen_add_playthrough_letter(&wmg, ld_hl_to_ml(ld, "I"));
  // CHEESE? + NI = NIpCHEESE
  assert(wmp_move_gen_check_playthrough_full_rack_existence(&wmg));

  // Save left-playthrough as NI.
  wmp_move_gen_save_playthrough_state(&wmg);

  // Add a P, as if playing right.
  wmp_move_gen_add_playthrough_letter(&wmg, ld_hl_to_ml(ld, "P"));
  // CHEESE? + NIP = NIPCHEESEs
  assert(wmp_move_gen_check_playthrough_full_rack_existence(&wmg));

  // Add a Q, and then there will be no bingo.
  wmp_move_gen_add_playthrough_letter(&wmg, ld_hl_to_ml(ld, "Q"));

  rack_destroy(rack);
  game_destroy(game);
  config_destroy(config);
}

void test_wmp_move_gen(void) {
  test_wmp_move_gen_inactive();
  test_nonplaythrough_existence();
  test_playthrough_bingo_existence();
}