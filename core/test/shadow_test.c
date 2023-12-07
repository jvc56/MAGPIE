#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "../src/config.h"
#include "../src/game.h"

#include "test_constants.h"
#include "test_util.h"
#include "testconfig.h"

void load_and_generate(Game *game, Player *player, const char *cgp,
                       const char *rack, bool add_exchange) {
  load_cgp(game, cgp);
  set_rack_to_string(game->gen->letter_distribution, player->rack, rack);
  generate_moves(NULL, game->gen, player, add_exchange,
                 player->move_record_type, player->move_sort_type, true);
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

void test_shadow_score(TestConfig *testconfig) {
  const Config *config = get_csw_config(testconfig);
  Game *game = create_game(config);
  Player *player = game->players[0];

  // This test checks scores only, so set the player move sorting
  // to sort by score.
  player->move_sort_type = MOVE_SORT_SCORE;

  load_and_generate(game, player, EMPTY_CGP, "OU", false);
  assert(game->gen->anchor_list->count == 1);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 4));

  load_and_generate(game, player, EMPTY_CGP, "ID", false);
  assert(game->gen->anchor_list->count == 1);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 6));

  load_and_generate(game, player, EMPTY_CGP, "AX", false);
  assert(game->gen->anchor_list->count == 1);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 18));

  load_and_generate(game, player, EMPTY_CGP, "BD", false);
  assert(game->gen->anchor_list->count == 1);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 10));

  load_and_generate(game, player, EMPTY_CGP, "QK", false);
  assert(game->gen->anchor_list->count == 1);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 30));

  load_and_generate(game, player, EMPTY_CGP, "AESR", false);
  assert(game->gen->anchor_list->count == 1);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 8));

  load_and_generate(game, player, EMPTY_CGP, "TNCL", false);
  assert(game->gen->anchor_list->count == 1);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 12));

  load_and_generate(game, player, EMPTY_CGP, "AAAAA", false);
  assert(game->gen->anchor_list->count == 1);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 12));

  load_and_generate(game, player, EMPTY_CGP, "CAAAA", false);
  assert(game->gen->anchor_list->count == 1);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 20));

  load_and_generate(game, player, EMPTY_CGP, "CAKAA", false);
  assert(game->gen->anchor_list->count == 1);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 32));

  load_and_generate(game, player, EMPTY_CGP, "AIERZ", false);
  assert(game->gen->anchor_list->count == 1);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 48));

  load_and_generate(game, player, EMPTY_CGP, "AIERZN", false);
  assert(game->gen->anchor_list->count == 1);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 50));

  load_and_generate(game, player, EMPTY_CGP, "AIERZNL", false);
  assert(game->gen->anchor_list->count == 1);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 102));

  load_and_generate(game, player, EMPTY_CGP, "?", false);
  assert(game->gen->anchor_list->count == 1);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 0));

  load_and_generate(game, player, EMPTY_CGP, "??", false);
  assert(game->gen->anchor_list->count == 1);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 0));

  load_and_generate(game, player, EMPTY_CGP, "??OU", false);
  assert(game->gen->anchor_list->count == 1);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 4));

  load_and_generate(game, player, EMPTY_CGP, "??OUA", false);
  assert(game->gen->anchor_list->count == 1);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 8));

  load_and_generate(game, player, KA_OPENING_CGP, "EE", false);
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
  // The rest are prevented by invalid cross sets

  load_and_generate(game, player, KA_OPENING_CGP, "E?", false);
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

  load_and_generate(game, player, KA_OPENING_CGP, "J", false);
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

  load_and_generate(game, player, AA_OPENING_CGP, "JF", false);
  // JF, JA, and FA
  assert(game->gen->anchor_list->count == 6);
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
  // Remaining anchors are prevented by invalid cross sets

  // Makeing JA, FA, and JFU, doubling the U on the double letter
  load_and_generate(game, player, AA_OPENING_CGP, "JFU", false);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 44));

  // Making KAU (allowed by F in rack cross set) and JUF, doubling the F and J.
  load_and_generate(game, player, KA_OPENING_CGP, "JFU", false);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 32));

  load_and_generate(game, player, AA_OPENING_CGP, "JFUG", false);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 47));

  load_and_generate(game, player, AA_OPENING_CGP, "JFUGX", false);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 61));

  // Reaches the triple word
  load_and_generate(game, player, AA_OPENING_CGP, "JFUGXL", false);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 102));

  load_and_generate(game, player, DOUG_V_EMELY_CGP, "Q", false);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 22));

  load_and_generate(game, player, DOUG_V_EMELY_CGP, "BD", false);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 17));

  load_and_generate(game, player, DOUG_V_EMELY_CGP, "BOH", false);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 60));

  load_and_generate(game, player, DOUG_V_EMELY_CGP, "BOHGX", false);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 90));

  load_and_generate(game, player, DOUG_V_EMELY_CGP, "BOHGXZ", false);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 120));

  load_and_generate(game, player, DOUG_V_EMELY_CGP, "BOHGXZQ", false);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 230));

  load_and_generate(game, player, TRIPLE_LETTERS_CGP, "A", false);

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

  load_and_generate(game, player, TRIPLE_LETTERS_CGP, "Z", false);
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

  load_and_generate(game, player, TRIPLE_LETTERS_CGP, "ZLW", false);
  // ZEN, ZW, WAD
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 73));
  // ZENLW
  assert(within_epsilon(
      game->gen->anchor_list->anchors[1]->highest_possible_equity, 45));
  // ZLWOW
  assert(within_epsilon(
      game->gen->anchor_list->anchors[2]->highest_possible_equity, 40));

  load_and_generate(game, player, TRIPLE_LETTERS_CGP, "ZLW?", false);
  // The blank makes all cross sets valid
  // LZW(WINDY)s
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 99));

  load_and_generate(game, player, TRIPLE_LETTERS_CGP, "QZLW", false);
  // ZQ, ZEN, QAD (L and W are in the AD cross set, but scored using the Q)
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 85));

  load_and_generate(game, player, TRIPLE_DOUBLE_CGP, "K", false);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 23));

  load_and_generate(game, player, TRIPLE_DOUBLE_CGP, "KT", false);
  // KPAVT
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 26));

  load_and_generate(game, player, TRIPLE_DOUBLE_CGP, "KT?", false);
  // The blank makes PAVE, allowed all letters in the cross set
  // PAVK, KT?
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 39));

  load_and_generate(game, player, BOTTOM_LEFT_RE_CGP, "M", false);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 8));

  load_and_generate(game, player, BOTTOM_LEFT_RE_CGP, "MN", false);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 16));

  load_and_generate(game, player, BOTTOM_LEFT_RE_CGP, "MNA", false);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 20));

  load_and_generate(game, player, BOTTOM_LEFT_RE_CGP, "MNAU", false);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 22));

  load_and_generate(game, player, BOTTOM_LEFT_RE_CGP, "MNAUT", false);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 30));

  load_and_generate(game, player, BOTTOM_LEFT_RE_CGP, "MNAUTE", false);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 39));

  load_and_generate(game, player, LATER_BETWEEN_DOUBLE_WORDS_CGP, "Z", false);
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

  load_and_generate(game, player, LATER_BETWEEN_DOUBLE_WORDS_CGP, "ZL", false);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 64));

  load_and_generate(game, player, LATER_BETWEEN_DOUBLE_WORDS_CGP, "ZLI", false);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 68));

  load_and_generate(game, player, LATER_BETWEEN_DOUBLE_WORDS_CGP, "ZLIE",
                    false);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 72));

  load_and_generate(game, player, LATER_BETWEEN_DOUBLE_WORDS_CGP, "ZLIER",
                    false);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 77));

  load_and_generate(game, player, LATER_BETWEEN_DOUBLE_WORDS_CGP, "ZLIERA",
                    false);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 80));

  load_and_generate(game, player, LATER_BETWEEN_DOUBLE_WORDS_CGP, "ZLIERAI",
                    false);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 212));

  load_and_generate(game, player, VS_OXY, "A", false);
  // APACIFYING
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 63));

  load_and_generate(game, player, VS_OXY, "PB", false);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 156));

  load_and_generate(game, player, VS_OXY, "PA", false);
  // Forms DORMPWOOAJ because the A fits in the cross set of T and N.
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 76));

  load_and_generate(game, player, VS_OXY, "PBA", false);
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 174));

  load_and_generate(game, player, VS_OXY, "Z", false);
  // ZPACIFYING
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 90));

  load_and_generate(game, player, VS_OXY, "ZE", false);
  // ZONE
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 160));

  load_and_generate(game, player, VS_OXY, "AZE", false);
  // UTAZONE
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 184));

  load_and_generate(game, player, VS_OXY, "AZEB", false);
  // HENBUTAZONE
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 484));

  load_and_generate(game, player, VS_OXY, "AZEBP", false);
  // YPHENBUTAZONE
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 604));

  load_and_generate(game, player, VS_OXY, "AZEBPX", false);
  // A2 A(Y)X(HEN)P(UT)EZ(ON)B
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 740));

  load_and_generate(game, player, VS_OXY, "AZEBPXO", false);
  // A1 OA(Y)X(HEN)P(UT)EZ(ON)B
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 1924));

  load_and_generate(game, player, VS_OXY, "AZEBPQO", false);
  // A1 OA(Y)Q(HEN)P(UT)EZ(ON)B
  // Only the letters AZEBPO are required to form acceptable
  // plays in all cross sets
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity, 2036));

  reset_game(game);
  char qi_qis[300] =
      "15/15/15/15/15/15/15/6QI7/6I8/6S8/15/15/15/15/15 FRUITED/EGGCUPS 22/12 "
      "0 lex CSW21";
  load_and_generate(game, player, qi_qis, "FRUITED", false);
  AnchorList *al = game->gen->anchor_list;
  // 8g (QI)DURFITE 128
  // h8 (I)DURFITE 103
  // f9 UFTRIDE 88
  // 7g EF(QIS)RTUDI 79
  // 10b FURED(S)IT 74
  // 9c DURT(I)FIE 70
  // 7h FURTIDE 69
  // h10 DUFTIE 45
  // f10 FURIDE 35
  assert(al->count == 9);
  assert(within_epsilon(al->anchors[0]->highest_possible_equity, 128));
  assert(al->anchors[0]->row == 7);
  assert(al->anchors[0]->col == 7);
  assert(al->anchors[0]->dir == BOARD_HORIZONTAL_DIRECTION);
  
  assert(within_epsilon(al->anchors[1]->highest_possible_equity, 103));
  assert(al->anchors[1]->row == 7);
  assert(al->anchors[1]->col == 7);
  assert(al->anchors[1]->dir == BOARD_VERTICAL_DIRECTION);

  // f9 UFTRIDE for 88, not h9 DURFITE for 100 (which does not fit)
  assert(within_epsilon(al->anchors[2]->highest_possible_equity, 88));
  assert(al->anchors[2]->row == 5);
  assert(al->anchors[2]->col == 8);
  assert(al->anchors[2]->dir == BOARD_VERTICAL_DIRECTION);
  
  destroy_game(game);
}

void test_shadow_equity(TestConfig *testconfig) {
  const Config *config = get_csw_config(testconfig);
  Game *game = create_game(config);
  Player *player = game->players[0];

  // This test checks scores only, so set move sorting
  // to sort by score.
  player->move_sort_type = MOVE_SORT_EQUITY;

  // Check best leave values for a give rack.
  Rack *leave_rack = create_rack(game->gen->letter_distribution->size);
  load_and_generate(game, player, EMPTY_CGP, "ERSVQUW", false);

  set_rack_to_string(game->gen->letter_distribution, leave_rack, "");
  assert(within_epsilon(game->gen->best_leaves[0],
                        get_leave_value_for_rack(player->klv, leave_rack)));

  set_rack_to_string(game->gen->letter_distribution, leave_rack, "S");
  assert(within_epsilon(game->gen->best_leaves[1],
                        get_leave_value_for_rack(player->klv, leave_rack)));

  set_rack_to_string(game->gen->letter_distribution, leave_rack, "ES");
  assert(within_epsilon(game->gen->best_leaves[2],
                        get_leave_value_for_rack(player->klv, leave_rack)));

  set_rack_to_string(game->gen->letter_distribution, leave_rack, "ERS");
  assert(within_epsilon(game->gen->best_leaves[3],
                        get_leave_value_for_rack(player->klv, leave_rack)));

  set_rack_to_string(game->gen->letter_distribution, leave_rack, "EQSU");
  assert(within_epsilon(game->gen->best_leaves[4],
                        get_leave_value_for_rack(player->klv, leave_rack)));

  set_rack_to_string(game->gen->letter_distribution, leave_rack, "EQRSU");
  assert(within_epsilon(game->gen->best_leaves[5],
                        get_leave_value_for_rack(player->klv, leave_rack)));

  set_rack_to_string(game->gen->letter_distribution, leave_rack, "EQRSUV");
  assert(within_epsilon(game->gen->best_leaves[6],
                        get_leave_value_for_rack(player->klv, leave_rack)));

  load_and_generate(game, player, EMPTY_CGP, "ESQW", true);
  set_rack_to_string(game->gen->letter_distribution, leave_rack, "ES");
  assert(within_epsilon(
      game->gen->anchor_list->anchors[0]->highest_possible_equity,
      28 + get_leave_value_for_rack(player->klv, leave_rack)));

  destroy_game(game);
  destroy_rack(leave_rack);
}

void test_shadow_top_move(TestConfig *testconfig) {
  const Config *config = get_csw_config(testconfig);
  Game *game = create_game(config);
  Player *player = game->players[0];

  player->move_sort_type = MOVE_SORT_EQUITY;
  player->move_record_type = MOVE_RECORD_BEST;

  // Top play should be L1 Q(I)
  load_and_generate(game, player, UEY_CGP, "ACEQOOV", true);
  assert(within_epsilon(game->gen->move_list->moves[0]->score, 21));

  destroy_game(game);
}

void test_shadow(TestConfig *testconfig) {
  test_shadow_score(testconfig);
  test_shadow_equity(testconfig);
  test_shadow_top_move(testconfig);
}