#include <assert.h>
#include <float.h>
#include <stdio.h>
#include <stdlib.h>

#include "../src/config.h"
#include "../src/game.h"
#include "superconfig.h"
#include "test_constants.h"
#include "test_util.h"

void load_and_generate(Game *game, Player *player, const char *cgp,
                       const char *rack, int add_exchange) {
  reset_game(game);
  load_cgp(game, cgp);
  set_rack_to_string(player->rack, rack, game->gen->letter_distribution);
  generate_moves(game->gen, player, NULL, add_exchange);
  double previous_equity;
  for (int i = 0; i < game->gen->anchor_list->count; i++) {
    if (i == 0) {
      previous_equity =
          game->gen->anchor_list->anchors[i]->highest_possible_equity;
    }
    assert(game->gen->anchor_list->anchors[i]->highest_possible_equity <=
           previous_equity);
  }
}

void test_shadow_score(SuperConfig *superconfig) {
  Config *config = get_csw_config(superconfig);
  Game *game = create_game(config);
  Player *player = game->players[0];

  // This test checks scores only, so set the player strategy param
  // to move sorting of type score.
  int original_move_sorting = player->strategy_params->move_sorting;
  player->strategy_params->move_sorting = MOVE_SORT_SCORE;

  load_and_generate(game, player, EMPTY_CGP, "OU", 0);
  assert(game->gen->anchor_list->count == 1);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 4));

  load_and_generate(game, player, EMPTY_CGP, "ID", 0);
  assert(game->gen->anchor_list->count == 1);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 6));

  load_and_generate(game, player, EMPTY_CGP, "AX", 0);
  assert(game->gen->anchor_list->count == 1);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 18));

  load_and_generate(game, player, EMPTY_CGP, "BD", 0);
  assert(game->gen->anchor_list->count == 1);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 10));

  load_and_generate(game, player, EMPTY_CGP, "QK", 0);
  assert(game->gen->anchor_list->count == 1);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 30));

  load_and_generate(game, player, EMPTY_CGP, "AESR", 0);
  assert(game->gen->anchor_list->count == 1);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 8));

  load_and_generate(game, player, EMPTY_CGP, "TNCL", 0);
  assert(game->gen->anchor_list->count == 1);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 12));

  load_and_generate(game, player, EMPTY_CGP, "AAAAA", 0);
  assert(game->gen->anchor_list->count == 1);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 12));

  load_and_generate(game, player, EMPTY_CGP, "CAAAA", 0);
  assert(game->gen->anchor_list->count == 1);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 20));

  load_and_generate(game, player, EMPTY_CGP, "CAKAA", 0);
  assert(game->gen->anchor_list->count == 1);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 32));

  load_and_generate(game, player, EMPTY_CGP, "AIERZ", 0);
  assert(game->gen->anchor_list->count == 1);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 48));

  load_and_generate(game, player, EMPTY_CGP, "AIERZN", 0);
  assert(game->gen->anchor_list->count == 1);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 50));

  // Due to bingo lookups and anchor splitting, we know no bingo exists,
  // and the highest possible score for the anchor would be a 6 letter play
  // for fifty like ZANIER, same as the previous test.
  // - The anchor is altered to have max 6 tiles
  // - No seven-tile anchor is added
  load_and_generate(game, player, EMPTY_CGP, "AIERZNL", 0);
  assert(game->gen->anchor_list->count == 1);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 50));

  // This time since there is a bingo with the rack, the anchor is split and
  // we have bingo and nonbingo versions.
  load_and_generate(game, player, EMPTY_CGP, "AEINSTZ", 0);
  assert(game->gen->anchor_list->count == 2);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 102));
  assert(within_epsilon(
      game->gen->anchor_list->anchors[1]->highest_possible_equity, 50));

  load_and_generate(game, player, EMPTY_CGP, "?", 0);
  assert(game->gen->anchor_list->count == 1);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 0));

  load_and_generate(game, player, EMPTY_CGP, "??", 0);
  assert(game->gen->anchor_list->count == 1);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 0));

  load_and_generate(game, player, EMPTY_CGP, "??OU", 0);
  assert(game->gen->anchor_list->count == 1);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 4));

  load_and_generate(game, player, EMPTY_CGP, "??OUA", 0);
  assert(game->gen->anchor_list->count == 1);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 8));

  load_and_generate(game, player, KA_OPENING_CGP, "EE", 0);
  assert(game->gen->anchor_list->count == 6);
  // KAE and EE
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 10));
  // EKE
  assert(within_epsilon(
      game->gen->anchor_list->anchors[1]->highest_possible_equity, 9));
  // KAEE
  assert(within_epsilon(
      game->gen->anchor_list->anchors[2]->highest_possible_equity, 8));
  // EE and E(A)
  assert(within_epsilon(
      game->gen->anchor_list->anchors[3]->highest_possible_equity, 5));
  // EE and E(A)
  assert(within_epsilon(
      game->gen->anchor_list->anchors[4]->highest_possible_equity, 5));
  // EEE
  assert(within_epsilon(
      game->gen->anchor_list->anchors[5]->highest_possible_equity, 3));

  load_and_generate(game, player, KA_OPENING_CGP, "E?", 0);
  assert(game->gen->anchor_list->count == 9);
  // oK, oE, EA
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 10));
  // KA, aE, AE
  assert(within_epsilon(
      game->gen->anchor_list->anchors[1]->highest_possible_equity, 10));
  // KAe, Ee
  assert(within_epsilon(
      game->gen->anchor_list->anchors[2]->highest_possible_equity, 8));
  // EKA, Ea
  assert(within_epsilon(
      game->gen->anchor_list->anchors[3]->highest_possible_equity, 8));
  // KAEe
  assert(within_epsilon(
      game->gen->anchor_list->anchors[4]->highest_possible_equity, 7));
  // E(K)e
  assert(within_epsilon(
      game->gen->anchor_list->anchors[5]->highest_possible_equity, 7));
  // Ea, EA
  assert(within_epsilon(
      game->gen->anchor_list->anchors[6]->highest_possible_equity, 3));
  // Ae, eE
  assert(within_epsilon(
      game->gen->anchor_list->anchors[7]->highest_possible_equity, 3));
  // E(A)a
  assert(within_epsilon(
      game->gen->anchor_list->anchors[8]->highest_possible_equity, 2));

  load_and_generate(game, player, KA_OPENING_CGP, "J", 0);
  assert(game->gen->anchor_list->count == 4);
  // J(K) veritcally
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 21));
  // J(KA) or (KA)J
  assert(within_epsilon(
      game->gen->anchor_list->anchors[1]->highest_possible_equity, 14));
  // J(A) horitizontally
  assert(within_epsilon(
      game->gen->anchor_list->anchors[2]->highest_possible_equity, 9));
  // J(A) vertically
  assert(within_epsilon(
      game->gen->anchor_list->anchors[3]->highest_possible_equity, 9));

  load_and_generate(game, player, AA_OPENING_CGP, "JF", 0);
  assert(game->gen->anchor_list->count == 6);
  // JF, JA, and FA
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 42));
  // JA and JF or FA and FJ
  assert(within_epsilon(
      game->gen->anchor_list->anchors[1]->highest_possible_equity, 25));
  // JAF with J and F doubled
  assert(within_epsilon(
      game->gen->anchor_list->anchors[2]->highest_possible_equity, 25));
  // FAA is in cross set, so JAA and JF are used to score.
  assert(within_epsilon(
      game->gen->anchor_list->anchors[3]->highest_possible_equity, 22));
  // AAJF
  assert(within_epsilon(
      game->gen->anchor_list->anchors[4]->highest_possible_equity, 14));
  // AJF
  assert(within_epsilon(
      game->gen->anchor_list->anchors[5]->highest_possible_equity, 13));

  // Making JA, FA, and JFU, doubling the U on the double letter
  load_and_generate(game, player, AA_OPENING_CGP, "JFU", 0);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 44));

  // Making KAU (allowed by F in rack cross set) and JUF, doubling the F and J.
  load_and_generate(game, player, KA_OPENING_CGP, "JFU", 0);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 32));

  load_and_generate(game, player, AA_OPENING_CGP, "JFUG", 0);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 47));

  load_and_generate(game, player, AA_OPENING_CGP, "JFUGX", 0);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 61));

  // Reaches the triple word
  load_and_generate(game, player, AA_OPENING_CGP, "JFUGXL", 0);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 102));

  load_and_generate(game, player, DOUG_V_EMELY_CGP, "Q", 0);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 22));

  load_and_generate(game, player, DOUG_V_EMELY_CGP, "BD", 0);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 17));

  load_and_generate(game, player, DOUG_V_EMELY_CGP, "BOH", 0);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 60));

  load_and_generate(game, player, DOUG_V_EMELY_CGP, "BOHGX", 0);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 90));

  load_and_generate(game, player, DOUG_V_EMELY_CGP, "BOHGXZ", 0);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 120));

  load_and_generate(game, player, DOUG_V_EMELY_CGP, "BOHGXZQ", 0);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 230));

  load_and_generate(game, player, TRIPLE_LETTERS_CGP, "A", 0);

  // WINDYA
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 13));
  // PROTEANA
  assert(within_epsilon(
      game->gen->anchor_list->anchors[1]->highest_possible_equity, 11));
  // ANY horizontally
  // ANY vertically
  // A(P) vertically
  // A(OW) vertically
  assert(within_epsilon(
      game->gen->anchor_list->anchors[2]->highest_possible_equity, 6));
  assert(within_epsilon(
      game->gen->anchor_list->anchors[3]->highest_possible_equity, 6));
  assert(within_epsilon(
      game->gen->anchor_list->anchors[4]->highest_possible_equity, 6));
  assert(within_epsilon(
      game->gen->anchor_list->anchors[5]->highest_possible_equity, 6));
  // A(EN)
  // AD(A)
  assert(within_epsilon(
      game->gen->anchor_list->anchors[6]->highest_possible_equity, 5));
  assert(within_epsilon(
      game->gen->anchor_list->anchors[7]->highest_possible_equity, 5));

  load_and_generate(game, player, TRIPLE_LETTERS_CGP, "Z", 0);
  // Z(P) vertically
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 33));
  // Z(EN) vert
  // Z(EN) horiz
  assert(within_epsilon(
      game->gen->anchor_list->anchors[1]->highest_possible_equity, 32));
  assert(within_epsilon(
      game->gen->anchor_list->anchors[2]->highest_possible_equity, 32));
  // (PROTEAN)Z
  assert(within_epsilon(
      game->gen->anchor_list->anchors[3]->highest_possible_equity, 29));

  load_and_generate(game, player, TRIPLE_LETTERS_CGP, "ZLW", 0);
  // ZEN, ZW, WAD
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 73));
  // ZENLW
  assert(within_epsilon(
      game->gen->anchor_list->anchors[1]->highest_possible_equity, 45));
  // ZLWOW
  assert(within_epsilon(
      game->gen->anchor_list->anchors[2]->highest_possible_equity, 40));

  load_and_generate(game, player, TRIPLE_LETTERS_CGP, "ZLW?", 0);
  // The blank makes all cross sets valid
  // LZW(WINDY)s
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 99));

  load_and_generate(game, player, TRIPLE_LETTERS_CGP, "QZLW", 0);
  // ZQ, ZEN, QAD (L and W are in the AD cross set, but scored using the Q)
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 85));

  load_and_generate(game, player, TRIPLE_DOUBLE_CGP, "K", 0);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 23));

  load_and_generate(game, player, TRIPLE_DOUBLE_CGP, "KT", 0);
  // KPAVT
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 26));

  load_and_generate(game, player, TRIPLE_DOUBLE_CGP, "KT?", 0);
  // The blank makes PAVE, allowed all letters in the cross set
  // PAVK, KT?
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 39));

  load_and_generate(game, player, BOTTOM_LEFT_RE_CGP, "M", 0);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 8));

  load_and_generate(game, player, BOTTOM_LEFT_RE_CGP, "MN", 0);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 16));

  load_and_generate(game, player, BOTTOM_LEFT_RE_CGP, "MNA", 0);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 20));

  load_and_generate(game, player, BOTTOM_LEFT_RE_CGP, "MNAU", 0);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 22));

  load_and_generate(game, player, BOTTOM_LEFT_RE_CGP, "MNAUT", 0);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 30));

  load_and_generate(game, player, BOTTOM_LEFT_RE_CGP, "MNAUTE", 0);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 39));

  load_and_generate(game, player, LATER_BETWEEN_DOUBLE_WORDS_CGP, "Z", 0);
  // (L)Z and (R)Z
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 31));
  assert(within_epsilon(
      game->gen->anchor_list->anchors[1]->highest_possible_equity, 31));
  // (LATER)Z
  assert(within_epsilon(
      game->gen->anchor_list->anchors[2]->highest_possible_equity, 30));
  // Z(T)
  assert(within_epsilon(
      game->gen->anchor_list->anchors[3]->highest_possible_equity, 21));

  load_and_generate(game, player, LATER_BETWEEN_DOUBLE_WORDS_CGP, "ZL", 0);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 64));

  load_and_generate(game, player, LATER_BETWEEN_DOUBLE_WORDS_CGP, "ZLI", 0);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 68));

  load_and_generate(game, player, LATER_BETWEEN_DOUBLE_WORDS_CGP, "ZLIE", 0);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 72));

  load_and_generate(game, player, LATER_BETWEEN_DOUBLE_WORDS_CGP, "ZLIER", 0);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 77));

  load_and_generate(game, player, LATER_BETWEEN_DOUBLE_WORDS_CGP, "ZLIERA", 0);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 80));

  load_and_generate(game, player, LATER_BETWEEN_DOUBLE_WORDS_CGP, "ZLIERAI", 0);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 212));

  load_and_generate(game, player, VS_OXY, "A", 0);
  // APACIFYING
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 63));

  load_and_generate(game, player, VS_OXY, "PB", 0);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 156));

  load_and_generate(game, player, VS_OXY, "PA", 0);
  // Forms DORMPWOOAJ because the A fits in the cross set of T and N.
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 76));

  load_and_generate(game, player, VS_OXY, "PBA", 0);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 174));

  load_and_generate(game, player, VS_OXY, "Z", 0);
  // ZPACIFYING
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 90));

  load_and_generate(game, player, VS_OXY, "ZE", 0);
  // ZONE
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 160));

  load_and_generate(game, player, VS_OXY, "AZE", 0);
  // UTAZONE
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 184));

  load_and_generate(game, player, VS_OXY, "AZEB", 0);
  // HENBUTAZONE
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 484));

  load_and_generate(game, player, VS_OXY, "AZEBP", 0);
  // YPHENBUTAZONE
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 604));

  load_and_generate(game, player, VS_OXY, "AZEBPX", 0);
  // A2 A(Y)X(HEN)P(UT)EZ(ON)B
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 740));

  load_and_generate(game, player, VS_OXY, "AZEBPXO", 0);
  // A1 OA(Y)X(HEN)P(UT)EZ(ON)B
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 1924));

  load_and_generate(game, player, VS_OXY, "AZEBPQO", 0);
  // A1 OA(Y)Q(HEN)P(UT)EZ(ON)B
  // Only the letters AZEBPO are required to form acceptable
  // plays in all cross sets
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 2036));

  player->strategy_params->move_sorting = original_move_sorting;

  destroy_game(game);
}

void test_shadow_equity(SuperConfig *superconfig) {
  Config *config = get_csw_config(superconfig);
  Game *game = create_game(config);
  Player *player = game->players[0];

  // This test checks scores only, so set the player strategy param
  // to move sorting of type score.
  int original_move_sorting = player->strategy_params->move_sorting;
  player->strategy_params->move_sorting = MOVE_SORT_EQUITY;

  // Check best leave values for a give rack.
  Rack *leave_rack = create_rack(game->gen->letter_distribution->size);
  load_and_generate(game, player, EMPTY_CGP, "ERSVQUW", 0);

  set_rack_to_string(leave_rack, "", game->gen->letter_distribution);
  assert(within_epsilon(
      game->gen->best_leaves[0],
      get_leave_value_for_rack(player->strategy_params->klv, leave_rack)));

  set_rack_to_string(leave_rack, "S", game->gen->letter_distribution);
  assert(within_epsilon(
      game->gen->best_leaves[1],
      get_leave_value_for_rack(player->strategy_params->klv, leave_rack)));

  set_rack_to_string(leave_rack, "ES", game->gen->letter_distribution);
  assert(within_epsilon(
      game->gen->best_leaves[2],
      get_leave_value_for_rack(player->strategy_params->klv, leave_rack)));

  set_rack_to_string(leave_rack, "ERS", game->gen->letter_distribution);
  assert(within_epsilon(
      game->gen->best_leaves[3],
      get_leave_value_for_rack(player->strategy_params->klv, leave_rack)));

  set_rack_to_string(leave_rack, "EQSU", game->gen->letter_distribution);
  assert(within_epsilon(
      game->gen->best_leaves[4],
      get_leave_value_for_rack(player->strategy_params->klv, leave_rack)));

  set_rack_to_string(leave_rack, "EQRSU", game->gen->letter_distribution);
  assert(within_epsilon(
      game->gen->best_leaves[5],
      get_leave_value_for_rack(player->strategy_params->klv, leave_rack)));

  set_rack_to_string(leave_rack, "EQRSUV", game->gen->letter_distribution);
  assert(within_epsilon(
      game->gen->best_leaves[6],
      get_leave_value_for_rack(player->strategy_params->klv, leave_rack)));

  load_and_generate(game, player, EMPTY_CGP, "ESQW", 1);
  set_rack_to_string(leave_rack, "ES", game->gen->letter_distribution);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity,
      28 + get_leave_value_for_rack(player->strategy_params->klv, leave_rack)));

  player->strategy_params->move_sorting = original_move_sorting;

  destroy_game(game);
  destroy_rack(leave_rack);
}

void test_split_anchors_for_bingos(SuperConfig *superconfig) {
  Config *config = get_csw_config(superconfig);
  Game *game = create_game(config);
  Generator *gen = game->gen;
  AnchorList *al = gen->anchor_list;
  for (int i = 0; i <= 6; i++) {
    gen->max_tiles_starting_left_by[i] = 0;
  }
  const struct ShadowLimit initial_shadow_limit = {0, -DBL_MAX};
  for (int i = 0; i <= RACK_SIZE; i++) {
    for (int j = 0; j <= RACK_SIZE; j++) {
      gen->shadow_limit_table[i][j] = initial_shadow_limit;
    }
  }
  for (int i = 2; i <= 7; i++) {
    double equity = 10 * i;
    gen->highest_equity_by_length[i] = 10 * i;
    gen->max_tiles_starting_left_by[i - 1] = i;
    for (int j = 0; j < i; j++) {
      gen->shadow_limit_table[j][i].highest_equity = equity;
      gen->shadow_limit_table[j][i].num_playthrough = 0;
    }
  }

  add_anchor(al, 7, 7, INITIAL_LAST_ANCHOR_COL, false, false, 0, 0, 2, 7,
             gen->max_tiles_starting_left_by, 70, gen->highest_equity_by_length,
             gen->shadow_limit_table);
  split_anchors_for_bingos(al, true);

  // splits into nonbingo and bingo anchors
  assert(al->count == 2);

  // nonbingo
  assert(within_epsilon(al->anchors[0]->highest_possible_equity, 60));
  assert(al->anchors[0]->row == 7);
  assert(al->anchors[0]->col == 7);
  assert(al->anchors[0]->last_anchor_col == INITIAL_LAST_ANCHOR_COL);
  assert(al->anchors[0]->transpose_state == false);
  assert(al->anchors[0]->vertical == false);
  for (int i = 2; i <= 6; i++) {
    assert(al->anchors[0]->highest_equity_by_length[i] == 10 * i);
  }
  assert(al->anchors[0]->max_num_playthrough == 0);
  assert(al->anchors[0]->min_tiles_to_play == 2);
  assert(al->anchors[0]->max_tiles_to_play == 6);

  // bingo
  assert(within_epsilon(al->anchors[1]->highest_possible_equity, 70));
  assert(al->anchors[0]->row == 7);
  assert(al->anchors[0]->col == 7);
  assert(al->anchors[0]->last_anchor_col == INITIAL_LAST_ANCHOR_COL);
  assert(al->anchors[0]->transpose_state == false);
  assert(al->anchors[0]->vertical == false);
  assert(al->anchors[1]->highest_equity_by_length[7] == 70);
  assert(al->anchors[1]->max_num_playthrough == 0);
  assert(al->anchors[1]->min_tiles_to_play == 7);
  assert(al->anchors[1]->max_tiles_to_play == 7);

  reset_anchor_list(al);
  add_anchor(al, 7, 7, INITIAL_LAST_ANCHOR_COL, false, false, 0, 0, 2, 7,
             gen->max_tiles_starting_left_by, 70, gen->highest_equity_by_length,
             gen->shadow_limit_table);
  split_anchors_for_bingos(al, false);

  // trims the anchor to look for nonbingos but does not create bingo anchor
  assert(al->count == 1);
  assert(within_epsilon(al->anchors[0]->highest_possible_equity, 60));
  assert(al->anchors[0]->row == 7);
  assert(al->anchors[0]->col == 7);
  assert(al->anchors[0]->last_anchor_col == INITIAL_LAST_ANCHOR_COL);
  assert(al->anchors[0]->transpose_state == false);
  assert(al->anchors[0]->vertical == false);
  for (int i = 2; i <= 6; i++) {
    assert(al->anchors[0]->highest_equity_by_length[i] == 10 * i);
  }
  assert(al->anchors[0]->max_num_playthrough == 0);
  assert(al->anchors[0]->min_tiles_to_play == 2);
  assert(al->anchors[0]->max_tiles_to_play == 6);

  reset_anchor_list(al);
  add_anchor(al, 7, 7, 5, true, true, 1, 1, 1, 7,
             gen->max_tiles_starting_left_by, 70, gen->highest_equity_by_length,
             gen->shadow_limit_table);
  split_anchors_for_bingos(al, true);

  // has no effect on this anchor because it always has playthrough
  assert(al->count == 1);
  assert(within_epsilon(al->anchors[0]->highest_possible_equity, 70));
  assert(al->anchors[0]->row == 7);
  assert(al->anchors[0]->col == 7);
  assert(al->anchors[0]->last_anchor_col == 5);
  assert(al->anchors[0]->transpose_state == true);
  assert(al->anchors[0]->vertical == true);
  for (int i = 2; i <= 7; i++) {
    assert(al->anchors[0]->highest_equity_by_length[i] == 10 * i);
  }
  assert(al->anchors[0]->max_num_playthrough == 1);
  assert(al->anchors[0]->min_tiles_to_play == 1);
  assert(al->anchors[0]->max_tiles_to_play == 7);

  reset_anchor_list(al);
  add_anchor(al, 7, 7, 5, true, true, 0, 2, 1, 7,
             gen->max_tiles_starting_left_by, 70, gen->highest_equity_by_length,
             gen->shadow_limit_table);
  split_anchors_for_bingos(al, true);
  assert(al->count == 3);
  assert(within_epsilon(al->anchors[0]->highest_possible_equity, 60));
  for (int i = 2; i <= 6; i++) {
    assert(al->anchors[0]->highest_equity_by_length[i] == 10 * i);
  }
  assert(al->anchors[0]->row == 7);
  assert(al->anchors[0]->col == 7);
  assert(al->anchors[0]->last_anchor_col == 5);
  assert(al->anchors[0]->transpose_state == true);
  assert(al->anchors[0]->vertical == true);
  assert(al->anchors[0]->min_num_playthrough == 0);
  assert(al->anchors[0]->max_num_playthrough == 2);
  assert(al->anchors[0]->min_tiles_to_play == 1);
  assert(al->anchors[0]->max_tiles_to_play == 6);

  assert(al->anchors[1]->highest_equity_by_length[7] == 70);
  assert(al->anchors[1]->row == 7);
  assert(al->anchors[1]->col == 7);
  assert(al->anchors[1]->last_anchor_col == 5);
  assert(al->anchors[1]->transpose_state == true);
  assert(al->anchors[1]->vertical == true);
  assert(al->anchors[1]->min_num_playthrough == 0);
  assert(al->anchors[1]->max_num_playthrough == 0);
  assert(al->anchors[1]->min_tiles_to_play == 7);
  assert(al->anchors[1]->max_tiles_to_play == 7);

  assert(al->anchors[2]->highest_equity_by_length[7] == 70);
  assert(al->anchors[2]->row == 7);
  assert(al->anchors[2]->col == 7);
  assert(al->anchors[2]->last_anchor_col == 5);
  assert(al->anchors[2]->transpose_state == true);
  assert(al->anchors[2]->vertical == true);
  assert(al->anchors[2]->min_num_playthrough == 1);
  assert(al->anchors[2]->max_num_playthrough == 2);
  assert(al->anchors[2]->min_tiles_to_play == 7);
  assert(al->anchors[2]->max_tiles_to_play == 7);

  char kaki_yond[300] =
      "15/15/15/15/15/15/15/6KAkI5/8YOND3/15/15/15/15/15/15 MIRITIS/EEEEEEE "
      "14/22 0 lex CSW21";
  assert(load_cgp(game, kaki_yond) == CGP_PARSE_STATUS_SUCCESS);
  Player *player = game->players[game->player_on_turn_index];
  Rack *opp_rack = game->players[1 - game->player_on_turn_index]->rack;
  /*
    StringBuilder *sb = create_string_builder();
    string_builder_add_game(game, sb);
    printf("%s\n", string_builder_peek(sb));
    destroy_string_builder(sb);
  */
  reset_anchor_list(al);
  gen->current_row_index = 8;
  gen->last_anchor_col = INITIAL_LAST_ANCHOR_COL;
  set_descending_tile_scores(gen, player);
  load_row_letter_cache(gen, gen->current_row_index);
  shadow_play_for_anchor(gen, 6, player, opp_rack);
  assert(al->count == 1);

  // 9c RISIMI(YOND)T, etc
  assert(within_epsilon(al->anchors[0]->highest_possible_equity, 85));

  assert(al->anchors[0]->max_tiles_starting_left_by[0] == 5);
  assert(al->anchors[0]->max_tiles_starting_left_by[1] == 6);
  assert(al->anchors[0]->max_tiles_starting_left_by[2] == 7);
  assert(al->anchors[0]->max_tiles_starting_left_by[3] == 7);
  assert(al->anchors[0]->max_tiles_starting_left_by[4] == 7);
  assert(al->anchors[0]->max_tiles_starting_left_by[5] == 7);
  assert(al->anchors[0]->max_tiles_starting_left_by[6] == 7);

  // Leaves are zero so all equities below are just scores
  ShadowLimit(*shadow_limit_table)[BOARD_DIM][RACK_SIZE + 1] =
      &(al->anchors[0]->shadow_limit_table);

  // ORIGINAL UNSPLIT ANCHOR
  // -----------------------
  // g8 KM
  assert(within_epsilon((*shadow_limit_table)[0][1].highest_equity, 11));
  assert(within_epsilon((*shadow_limit_table)[0][1].num_playthrough, 0));

  // 9g MI(YOND)
  assert(within_epsilon((*shadow_limit_table)[0][2].highest_equity, 28));
  assert(within_epsilon((*shadow_limit_table)[0][2].num_playthrough, 4));

  // 9g MI(YOND)I
  assert(within_epsilon((*shadow_limit_table)[0][3].highest_equity, 30));
  assert(within_epsilon((*shadow_limit_table)[0][3].num_playthrough, 4));

  // 9g MI(YOND)IS
  assert(within_epsilon((*shadow_limit_table)[0][4].highest_equity, 31));
  assert(within_epsilon((*shadow_limit_table)[0][4].num_playthrough, 4));

  // 9g MI(YOND)IST
  assert(within_epsilon((*shadow_limit_table)[0][5].highest_equity, 32));
  assert(within_epsilon((*shadow_limit_table)[0][5].num_playthrough, 4));

  // 9f IM
  assert(within_epsilon((*shadow_limit_table)[1][2].highest_equity, 18));
  assert(within_epsilon((*shadow_limit_table)[1][2].num_playthrough, 0));

  // 9f IMI(YOND)
  assert(within_epsilon((*shadow_limit_table)[1][3].highest_equity, 29));
  assert(within_epsilon((*shadow_limit_table)[1][3].num_playthrough, 4));

  // 9f IMI(YOND)I
  assert(within_epsilon((*shadow_limit_table)[1][4].highest_equity, 31));
  assert(within_epsilon((*shadow_limit_table)[1][4].num_playthrough, 4));

  // 9f IMI(YOND)IS
  assert(within_epsilon((*shadow_limit_table)[1][5].highest_equity, 32));
  assert(within_epsilon((*shadow_limit_table)[1][5].num_playthrough, 4));

  // 9f IMI(YOND)IST
  assert(within_epsilon((*shadow_limit_table)[1][6].highest_equity, 33));
  assert(within_epsilon((*shadow_limit_table)[1][6].num_playthrough, 4));

  // 9e RIM
  assert(within_epsilon((*shadow_limit_table)[2][3].highest_equity, 19));
  assert(within_epsilon((*shadow_limit_table)[2][3].num_playthrough, 0));

  // 9e RIMI(YOND)
  assert(within_epsilon((*shadow_limit_table)[2][4].highest_equity, 30));
  assert(within_epsilon((*shadow_limit_table)[2][4].num_playthrough, 4));

  // 9e RIMI(YOND)I
  assert(within_epsilon((*shadow_limit_table)[2][5].highest_equity, 32));
  assert(within_epsilon((*shadow_limit_table)[2][5].num_playthrough, 4));

  // 9e RIMI(YOND)IS
  assert(within_epsilon((*shadow_limit_table)[2][6].highest_equity, 33));
  assert(within_epsilon((*shadow_limit_table)[2][6].num_playthrough, 4));

  // 9e RIMI(YOND)IST
  assert(within_epsilon((*shadow_limit_table)[2][7].highest_equity, 84));
  assert(within_epsilon((*shadow_limit_table)[2][7].num_playthrough, 4));

  // 9d TRIM
  assert(within_epsilon((*shadow_limit_table)[3][4].highest_equity, 20));
  assert(within_epsilon((*shadow_limit_table)[3][4].num_playthrough, 0));

  // 9d TRIMI(YOND)
  assert(within_epsilon((*shadow_limit_table)[3][5].highest_equity, 31));
  assert(within_epsilon((*shadow_limit_table)[3][5].num_playthrough, 4));

  // 9d TRIMI(YOND)I
  assert(within_epsilon((*shadow_limit_table)[3][6].highest_equity, 33));
  assert(within_epsilon((*shadow_limit_table)[3][6].num_playthrough, 4));

  // 9d TRIMI(YOND)IS
  assert(within_epsilon((*shadow_limit_table)[3][7].highest_equity, 84));
  assert(within_epsilon((*shadow_limit_table)[3][7].num_playthrough, 4));

  // 9c STRIM
  assert(within_epsilon((*shadow_limit_table)[4][5].highest_equity, 22));
  assert(within_epsilon((*shadow_limit_table)[4][5].num_playthrough, 0));

  // 9c STRIMI(YOND)
  assert(within_epsilon((*shadow_limit_table)[4][6].highest_equity, 33));
  assert(within_epsilon((*shadow_limit_table)[4][6].num_playthrough, 4));

  // 9c STRIMI(YOND)I
  assert(within_epsilon((*shadow_limit_table)[4][7].highest_equity, 85));
  assert(within_epsilon((*shadow_limit_table)[4][7].num_playthrough, 4));

  // 9b ISTRIM
  assert(within_epsilon((*shadow_limit_table)[5][6].highest_equity, 23));
  assert(within_epsilon((*shadow_limit_table)[5][6].num_playthrough, 0));

  // 9b ISTRIMI(YOND)
  assert(within_epsilon((*shadow_limit_table)[5][7].highest_equity, 84));
  assert(within_epsilon((*shadow_limit_table)[5][7].num_playthrough, 4));

  // 9a IISTRIM
  assert(within_epsilon((*shadow_limit_table)[6][7].highest_equity, 74));
  assert(within_epsilon((*shadow_limit_table)[6][7].num_playthrough, 0));

  split_anchors_for_bingos(al, true);

  // nonbingo, bingo without playthrough, bingo with playthrough
  assert(al->count == 3);

  // NONBINGO ANCHOR
  assert(within_epsilon(al->anchors[0]->highest_possible_equity, 33));
  assert(al->anchors[0]->min_tiles_to_play == 1);
  assert(al->anchors[0]->max_tiles_to_play == 6);
  assert(al->anchors[0]->max_tiles_starting_left_by[0] == 5);
  assert(al->anchors[0]->max_tiles_starting_left_by[1] == 6);
  assert(al->anchors[0]->max_tiles_starting_left_by[2] == 6);
  assert(al->anchors[0]->max_tiles_starting_left_by[3] == 6);
  assert(al->anchors[0]->max_tiles_starting_left_by[4] == 6);
  assert(al->anchors[0]->max_tiles_starting_left_by[5] == 6);
  assert(al->anchors[0]->max_tiles_starting_left_by[6] == 6);

  // g8 KM
  assert(within_epsilon((*shadow_limit_table)[0][1].highest_equity, 11));
  assert(within_epsilon((*shadow_limit_table)[0][1].num_playthrough, 0));

  // 9g MI(YOND)
  assert(within_epsilon((*shadow_limit_table)[0][2].highest_equity, 28));
  assert(within_epsilon((*shadow_limit_table)[0][2].num_playthrough, 4));

  // 9g MI(YOND)I
  assert(within_epsilon((*shadow_limit_table)[0][3].highest_equity, 30));
  assert(within_epsilon((*shadow_limit_table)[0][3].num_playthrough, 4));

  // 9g MI(YOND)IS
  assert(within_epsilon((*shadow_limit_table)[0][4].highest_equity, 31));
  assert(within_epsilon((*shadow_limit_table)[0][4].num_playthrough, 4));

  // 9g MI(YOND)IST
  assert(within_epsilon((*shadow_limit_table)[0][5].highest_equity, 32));
  assert(within_epsilon((*shadow_limit_table)[0][5].num_playthrough, 4));

  // 9f IM
  assert(within_epsilon((*shadow_limit_table)[1][2].highest_equity, 18));
  assert(within_epsilon((*shadow_limit_table)[1][2].num_playthrough, 0));

  // 9f IMI(YOND)
  assert(within_epsilon((*shadow_limit_table)[1][3].highest_equity, 29));
  assert(within_epsilon((*shadow_limit_table)[1][3].num_playthrough, 4));

  // 9f IMI(YOND)I
  assert(within_epsilon((*shadow_limit_table)[1][4].highest_equity, 31));
  assert(within_epsilon((*shadow_limit_table)[1][4].num_playthrough, 4));

  // 9f IMI(YOND)IS
  assert(within_epsilon((*shadow_limit_table)[1][5].highest_equity, 32));
  assert(within_epsilon((*shadow_limit_table)[1][5].num_playthrough, 4));

  // 9f IMI(YOND)IST
  assert(within_epsilon((*shadow_limit_table)[1][6].highest_equity, 33));
  assert(within_epsilon((*shadow_limit_table)[1][6].num_playthrough, 4));

  // 9e RIM
  assert(within_epsilon((*shadow_limit_table)[2][3].highest_equity, 19));
  assert(within_epsilon((*shadow_limit_table)[2][3].num_playthrough, 0));

  // 9e RIMI(YOND)
  assert(within_epsilon((*shadow_limit_table)[2][4].highest_equity, 30));
  assert(within_epsilon((*shadow_limit_table)[2][4].num_playthrough, 4));

  // 9e RIMI(YOND)I
  assert(within_epsilon((*shadow_limit_table)[2][5].highest_equity, 32));
  assert(within_epsilon((*shadow_limit_table)[2][5].num_playthrough, 4));

  // 9e RIMI(YOND)IS
  assert(within_epsilon((*shadow_limit_table)[2][6].highest_equity, 33));
  assert(within_epsilon((*shadow_limit_table)[2][6].num_playthrough, 4));

  // 9d TRIM
  assert(within_epsilon((*shadow_limit_table)[3][4].highest_equity, 20));
  assert(within_epsilon((*shadow_limit_table)[3][4].num_playthrough, 0));

  // 9d TRIMI(YOND)
  assert(within_epsilon((*shadow_limit_table)[3][5].highest_equity, 31));
  assert(within_epsilon((*shadow_limit_table)[3][5].num_playthrough, 4));

  // 9d TRIMI(YOND)I
  assert(within_epsilon((*shadow_limit_table)[3][6].highest_equity, 33));
  assert(within_epsilon((*shadow_limit_table)[3][6].num_playthrough, 4));

  // 9c STRIM
  assert(within_epsilon((*shadow_limit_table)[4][5].highest_equity, 22));
  assert(within_epsilon((*shadow_limit_table)[4][5].num_playthrough, 0));

  // 9c STRIMI(YOND)
  assert(within_epsilon((*shadow_limit_table)[4][6].highest_equity, 33));
  assert(within_epsilon((*shadow_limit_table)[4][6].num_playthrough, 4));

  // 9b ISTRIM
  assert(within_epsilon((*shadow_limit_table)[5][6].highest_equity, 23));
  assert(within_epsilon((*shadow_limit_table)[5][6].num_playthrough, 0));

  // BINGO WITHOUT PLAYTHROUGH
  // -------------------------
  assert(within_epsilon(al->anchors[1]->highest_possible_equity, 74));
  assert(al->anchors[1]->min_tiles_to_play == 7);
  assert(al->anchors[1]->max_tiles_to_play == 7);
  assert(al->anchors[1]->max_tiles_starting_left_by[0] == 0);
  assert(al->anchors[1]->max_tiles_starting_left_by[1] == 0);
  assert(al->anchors[1]->max_tiles_starting_left_by[2] == 0);
  assert(al->anchors[1]->max_tiles_starting_left_by[3] == 0);
  assert(al->anchors[1]->max_tiles_starting_left_by[4] == 0);
  assert(al->anchors[1]->max_tiles_starting_left_by[5] == 0);
  assert(al->anchors[1]->max_tiles_starting_left_by[6] == 7);

  shadow_limit_table = &(al->anchors[1]->shadow_limit_table);
  // 9a IISTRIM
  assert(within_epsilon((*shadow_limit_table)[6][7].highest_equity, 74));
  assert(within_epsilon((*shadow_limit_table)[6][7].num_playthrough, 0));

  // BINGO WITH PLAYTHROUGH
  // ----------------------
  assert(within_epsilon(al->anchors[2]->highest_possible_equity, 85));
  assert(al->anchors[2]->min_tiles_to_play == 7);
  assert(al->anchors[2]->max_tiles_to_play == 7);
  assert(al->anchors[2]->max_tiles_starting_left_by[0] == 0);
  assert(al->anchors[2]->max_tiles_starting_left_by[1] == 0);
  assert(al->anchors[2]->max_tiles_starting_left_by[2] == 7);
  assert(al->anchors[2]->max_tiles_starting_left_by[3] == 7);
  assert(al->anchors[2]->max_tiles_starting_left_by[4] == 7);
  assert(al->anchors[2]->max_tiles_starting_left_by[5] == 7);
  assert(al->anchors[2]->max_tiles_starting_left_by[6] == 0);

  shadow_limit_table = &(al->anchors[2]->shadow_limit_table);
  // 9e RIMI(YOND)IST
  assert(within_epsilon((*shadow_limit_table)[2][7].highest_equity, 84));
  assert(within_epsilon((*shadow_limit_table)[2][7].num_playthrough, 4));

  // 9d TRIMI(YOND)IS
  assert(within_epsilon((*shadow_limit_table)[3][7].highest_equity, 84));
  assert(within_epsilon((*shadow_limit_table)[3][7].num_playthrough, 4));

  // 9c STRIMI(YOND)I
  assert(within_epsilon((*shadow_limit_table)[4][7].highest_equity, 85));
  assert(within_epsilon((*shadow_limit_table)[4][7].num_playthrough, 4));

  // 9b ISTRIMI(YOND)
  assert(within_epsilon((*shadow_limit_table)[5][7].highest_equity, 84));
  assert(within_epsilon((*shadow_limit_table)[5][7].num_playthrough, 4));

  reset_game(game);
  char qi_qis[300] =
      "15/15/15/15/15/15/15/6QI7/6I8/6S8/15/15/15/15/15 FRUITED/EGGCUPS 22/12 "
      "0 lex CSW21";
  assert(load_cgp(game, qi_qis) == CGP_PARSE_STATUS_SUCCESS);
  player = game->players[game->player_on_turn_index];
  opp_rack = game->players[1 - game->player_on_turn_index]->rack;

  /*
    StringBuilder *sb = create_string_builder();
    string_builder_add_game(game, sb);
    printf("%s\n", string_builder_peek(sb));
    destroy_string_builder(sb);
  */

  reset_anchor_list(al);
  set_descending_tile_scores(gen, player);
  for (int dir = 0; dir < 2; dir++) {
    gen->vertical = dir % 2 != 0;
    shadow_by_orientation(gen, player, dir, opp_rack);
    transpose(gen->board);
  }
  sort_anchor_list(al);

  // 8g (QI)DURFITE 128
  // h8 (I)DURFITE 103
  // f9 UFTRIDE 88 (can split)
  // 7g EF(QIS)RTUDI 79
  // 10b FURED(S)IT 74
  // 9c DURT(I)FIE 70
  // 7h FURTIDE 69 (can split)
  // h10 DUFTIE 45
  // f10 FURIDE 35
  assert(al->count == 9);
  sort_anchor_list(al);

  assert(within_epsilon(al->anchors[0]->highest_possible_equity, 128));
  assert(al->anchors[0]->row == 7);
  assert(al->anchors[0]->col == 7);
  assert(al->anchors[0]->vertical == false);
  assert(al->anchors[0]->min_tiles_to_play == 1);
  assert(al->anchors[0]->max_tiles_to_play == 7);
  assert(al->anchors[0]->max_tiles_starting_left_by[0] == 0);
  assert(al->anchors[0]->max_tiles_starting_left_by[1] == 7);

  assert(within_epsilon(al->anchors[1]->highest_possible_equity, 103));
  assert(al->anchors[1]->row == 7);
  assert(al->anchors[1]->col == 7);
  assert(al->anchors[1]->vertical == true);
  assert(al->anchors[1]->min_tiles_to_play == 1);
  assert(al->anchors[1]->max_tiles_to_play == 7);
  assert(al->anchors[1]->max_tiles_starting_left_by[0] == 7);
  assert(al->anchors[1]->max_tiles_starting_left_by[1] == 7);

  assert(within_epsilon(al->anchors[2]->highest_possible_equity, 88));
  assert(al->anchors[2]->row == 5);
  assert(al->anchors[2]->col == 8);
  assert(al->anchors[2]->vertical == true);
  assert(al->anchors[2]->min_tiles_to_play == 2);
  assert(al->anchors[2]->max_tiles_to_play == 7);
  assert(al->anchors[2]->max_tiles_starting_left_by[0] == 7);
  assert(al->anchors[2]->max_tiles_starting_left_by[1] == 0);

  split_anchors_for_bingos(al, true);
  sort_anchor_list(al);
  assert(al->count == 11);

  reset_game(game);
  char e_t[300] =
      "15/15/15/15/10E4/15/15/15/15/15/15/9T5/15/15/15 AAELNRT/ADJNOPS 311/106 "
      "0 lex CSW21";
  assert(load_cgp(game, e_t) == CGP_PARSE_STATUS_SUCCESS);
  player = game->players[game->player_on_turn_index];
  opp_rack = game->players[1 - game->player_on_turn_index]->rack;

  StringBuilder *sb = create_string_builder();
  string_builder_add_game(game, sb);
  printf("%s\n", string_builder_peek(sb));
  destroy_string_builder(sb);

  reset_anchor_list(al);
  set_descending_tile_scores(gen, player);
  transpose(gen->board);
  shadow_by_orientation(gen, player, 1, opp_rack);

  sort_anchor_list(al);

  // just the vertical plays

  // k5 (E)NTRALEA 68 (doubled, hooking a two)
  // l1 LATERAN 68 (doubled + a DLS, hooking a two)
  // k8 LATERAN 66 (doubled, hooking a two)
  // j5 ALTERAN(T) 64 (two TLS, hooking a two)
  // i7 LATERAN 62 (three DLS, hooking a two)
  // j7 TARLE(T)AN 62 (two TLS)
  assert(al->count == 6);
  assert(within_epsilon(al->anchors[0]->highest_possible_equity, 68));
  assert(within_epsilon(al->anchors[1]->highest_possible_equity, 68));
  assert(within_epsilon(al->anchors[2]->highest_possible_equity, 66));

  assert(within_epsilon(al->anchors[3]->highest_possible_equity, 64));
  assert(al->anchors[3]->row == 9);
  assert(al->anchors[3]->col == 4);
  assert(al->anchors[3]->vertical == true);
  assert(al->anchors[3]->min_tiles_to_play == 2);
  assert(al->anchors[3]->max_tiles_to_play == 7);
  assert(al->anchors[3]->min_num_playthrough == 0);
  assert(al->anchors[3]->max_num_playthrough == 1);

  // word starts at anchor, eight letter bingo reaches the playthrough tile (T).
  assert(within_epsilon(al->anchors[3]->shadow_limit_table[0][7].highest_equity,
                        64));
  assert(al->anchors[3]->shadow_limit_table[0][7].num_playthrough == 1);

  // seven letter bingo, no playthrough
  assert(within_epsilon(al->anchors[3]->shadow_limit_table[1][7].highest_equity,
                        63));
  assert(al->anchors[3]->shadow_limit_table[1][7].num_playthrough == 0);

  assert(within_epsilon(al->anchors[4]->highest_possible_equity, 62));
  assert(within_epsilon(al->anchors[5]->highest_possible_equity, 62));

  destroy_game(game);
}

void test_shadow_top_move(SuperConfig *superconfig) {
  Config *config = get_csw_config(superconfig);
  Game *game = create_game(config);
  Player *player = game->players[0];

  int original_move_sorting = player->strategy_params->move_sorting;
  int original_recorder_type = player->strategy_params->play_recorder_type;
  player->strategy_params->move_sorting = MOVE_SORT_EQUITY;
  player->strategy_params->play_recorder_type = MOVE_RECORDER_BEST;

  // Top play should be L1 Q(I)
  load_and_generate(game, player, UEY_CGP, "ACEQOOV", true);
  /*
  StringBuilder *sb = create_string_builder();
  string_builder_add_game(game, sb);
  printf("%s\n", string_builder_peek(sb));
  destroy_string_builder(sb);
  */
  assert(game->gen->move_list->moves[0]->score == 21);

  player->strategy_params->move_sorting = original_move_sorting;
  player->strategy_params->play_recorder_type = original_recorder_type;

  destroy_game(game);
}

void test_shadow(SuperConfig *superconfig) {
  test_shadow_score(superconfig);
  test_shadow_equity(superconfig);
  test_split_anchors_for_bingos(superconfig);
  test_shadow_top_move(superconfig);
}