#include <assert.h>
#include <stdio.h>

#include "../src/def/move_gen_defs.h"

#include "../src/ent/config.h"
#include "../src/ent/game.h"
#include "../src/ent/klv.h"

#include "../src/impl/gameplay.h"

#include "../src/util/util.h"

#include "test_constants.h"
#include "test_util.h"

void test_macondo_opening_equity_adjustments() {
  Config *config = create_config_or_die(
      "setoptions lex CSW21 s1 equity s2 equity r1 all r2 all numplays 1");
  Game *game = create_game(config);
  Player *player0 = game_get_player(game, 0);
  Rack *rack = player_get_rack(player0);
  const KLV *klv = player_get_klv(player0);
  const LetterDistribution *ld = game_get_ld(game);
  MoveList *move_list = create_move_list(1);
  set_rack_to_string(ld, rack, "EORSTVX");
  generate_moves_for_game(game, 0, move_list);

  // Should be 8G VORTEX
  SortedMoveList *vortex_sorted_move_list = create_sorted_move_list(move_list);

  const Move *top_move = vortex_sorted_move_list->moves[0];
  assert(get_col_start(top_move) == 6);
  assert(move_get_tiles_played(top_move) == 6);
  assert(get_score(top_move) == 48);
  assert(within_epsilon((double)(get_score(top_move) +
                                 get_leave_value_for_move(klv, top_move, rack)),
                        get_equity(top_move)));

  destroy_sorted_move_list(vortex_sorted_move_list);
  reset_game(game);

  set_rack_to_string(ld, rack, "BDEIIIJ");
  // Should be 8D JIBED
  generate_moves_for_game(game, 0, move_list);

  SortedMoveList *jibed_sorted_move_list = create_sorted_move_list(move_list);

  top_move = jibed_sorted_move_list->moves[0];
  assert(get_col_start(top_move) == 3);
  assert(move_get_tiles_played(top_move) == 5);
  assert(get_score(top_move) == 46);
  assert(within_epsilon((double)(get_score(top_move) +
                                 get_leave_value_for_move(klv, top_move, rack) +
                                 OPENING_HOTSPOT_PENALTY),
                        get_equity(top_move)));

  destroy_sorted_move_list(jibed_sorted_move_list);
  reset_game(game);

  set_rack_to_string(ld, rack, "ACEEEFT");
  generate_moves_for_game(game, 0, move_list);
  // Should be 8D FACETE
  SortedMoveList *facete_sorted_move_list = create_sorted_move_list(move_list);
  top_move = facete_sorted_move_list->moves[0];
  assert(get_col_start(top_move) == 3);
  assert(move_get_tiles_played(top_move) == 6);
  assert(get_score(top_move) == 30);
  assert(within_epsilon((double)(get_score(top_move) +
                                 get_leave_value_for_move(klv, top_move, rack) +
                                 (2 * OPENING_HOTSPOT_PENALTY)),
                        get_equity(top_move)));
  destroy_sorted_move_list(facete_sorted_move_list);
  reset_game(game);

  set_rack_to_string(ld, rack, "AAAALTY");
  generate_moves_for_game(game, 0, move_list);
  // Should be 8G ATALAYA
  SortedMoveList *atalaya_sorted_move_list = create_sorted_move_list(move_list);
  top_move = atalaya_sorted_move_list->moves[0];
  assert(get_col_start(top_move) == 6);
  assert(move_get_tiles_played(top_move) == 7);
  assert(get_score(top_move) == 78);
  assert(within_epsilon((double)(get_score(top_move) +
                                 get_leave_value_for_move(klv, top_move, rack) +
                                 (3 * OPENING_HOTSPOT_PENALTY)),
                        get_equity(top_move)));

  destroy_sorted_move_list(atalaya_sorted_move_list);
  destroy_move_list(move_list);
  destroy_game(game);
  destroy_config(config);
}

void test_macondo_endgame_equity_adjustments() {
  Config *config = create_config_or_die(
      "setoptions lex CSW21 s1 equity s2 equity r1 all r2 all numplays 1");
  Game *game = create_game(config);
  MoveList *move_list = create_move_list(6);

  load_cgp(game,
           "4RUMMAGED2C/7A6A/2H1G2T6V/2O1O2I6E/2WAB2PREBENDS/2ER3O3n3/2SI6COW2/"
           "3L2HUE2KANE/3LI3FILII2/J1TANGENT2T1Z1/A2TA5FA1OP/R2EN5Ok1OU/"
           "VILDE5YEX1D/I3R6SUQS/E13Y INR/OT 440/448 0 lex CSW21;");

  generate_moves_for_game(game, 0, move_list);

  SortedMoveList *endgame_sorted_move_list = create_sorted_move_list(move_list);

  const Move *move0 = endgame_sorted_move_list->moves[0];
  assert(get_score(move0) == 8);
  assert(get_row_start(move0) == 1);
  assert(get_col_start(move0) == 10);
  assert(move_get_tiles_played(move0) == 3);
  assert(within_epsilon(get_equity(move0), 12));

  const Move *move1 = endgame_sorted_move_list->moves[1];
  assert(get_score(move1) == 5);
  assert(get_row_start(move1) == 2);
  assert(get_col_start(move1) == 7);
  assert(move_get_tiles_played(move1) == 3);
  assert(within_epsilon(get_equity(move1), 9));

  const Move *move2 = endgame_sorted_move_list->moves[2];
  assert(get_score(move2) == 13);
  assert(get_row_start(move2) == 1);
  assert(get_col_start(move2) == 5);
  assert(move_get_tiles_played(move2) == 2);
  assert(within_epsilon(get_equity(move2), 1));

  const Move *move3 = endgame_sorted_move_list->moves[3];
  assert(get_score(move3) == 12);
  assert(get_row_start(move3) == 1);
  assert(get_col_start(move3) == 7);
  assert(move_get_tiles_played(move3) == 2);
  assert(within_epsilon(get_equity(move3), 0));

  const Move *move4 = endgame_sorted_move_list->moves[4];
  assert(get_score(move4) == 11);
  assert(get_row_start(move4) == 1);
  assert(get_col_start(move4) == 9);
  assert(move_get_tiles_played(move4) == 2);
  assert(within_epsilon(get_equity(move4), -1));

  const Move *move5 = endgame_sorted_move_list->moves[5];
  assert(get_score(move5) == 10);
  assert(get_row_start(move5) == 9);
  assert(get_col_start(move5) == 2);
  assert(move_get_tiles_played(move5) == 2);
  assert(within_epsilon(get_equity(move5), -2));

  destroy_sorted_move_list(endgame_sorted_move_list);
  destroy_move_list(move_list);
  destroy_game(game);
  destroy_config(config);
}

void test_equity_adjustments() {
  test_macondo_opening_equity_adjustments();
  test_macondo_endgame_equity_adjustments();
}