#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "../src/ent/anchor.h"
#include "../src/ent/config.h"
#include "../src/ent/game.h"

#include "../src/impl/gameplay.h"

#include "test_constants.h"
#include "test_util.h"
#include "testconfig.h"

void load_and_generate(Game *game, MoveGen *gen, Player *player,
                       const char *cgp, const char *rack) {

  LetterDistribution *ld = game_get_ld(game);
  Rack *player_rack = player_get_rack(player);
  AnchorList *anchor_list = gen_get_anchor_list(gen);

  load_cgp(game, cgp);
  set_rack_to_string(ld, player_rack, rack);
  generate_moves_for_game_with_player_move_types(game, gen);
  double previous_equity = 10000000;
  int number_of_anchors = get_number_of_anchors(anchor_list);
  for (int i = 0; i < number_of_anchors; i++) {
    double equity = get_anchor_highest_possible_equity(anchor_list, i);
    assert(equity <= previous_equity);
    previous_equity = equity;
  }
}

void test_shadow_score(TestConfig *testconfig) {
  const Config *config = get_csw_config(testconfig);
  Game *game = create_game(config);
  MoveGen *gen = create_generator(
      config_get_num_plays(config),
      letter_distribution_get_size(config_get_letter_distribution(config)));
  Player *player = game_get_player(game, 0);
  AnchorList *anchor_list = gen_get_anchor_list(gen);

  // This test checks scores only, so set the player move sorting
  // to sort by score.
  player_set_move_sort_type(player, MOVE_SORT_SCORE);

  load_and_generate(game, gen, player, EMPTY_CGP, "OU");
  assert(get_number_of_anchors(anchor_list) == 1);
  assert(within_epsilon(get_anchor_highest_possible_equity(anchor_list, 0), 4));

  load_and_generate(game, gen, player, EMPTY_CGP, "ID");
  assert(get_number_of_anchors(anchor_list) == 1);
  assert(within_epsilon(get_anchor_highest_possible_equity(anchor_list, 0), 6));

  load_and_generate(game, gen, player, EMPTY_CGP, "AX");
  assert(get_number_of_anchors(anchor_list) == 1);
  assert(
      within_epsilon(get_anchor_highest_possible_equity(anchor_list, 0), 18));

  load_and_generate(game, gen, player, EMPTY_CGP, "BD");
  assert(get_number_of_anchors(anchor_list) == 1);
  assert(
      within_epsilon(get_anchor_highest_possible_equity(anchor_list, 0), 10));

  load_and_generate(game, gen, player, EMPTY_CGP, "QK");
  assert(get_number_of_anchors(anchor_list) == 1);
  assert(
      within_epsilon(get_anchor_highest_possible_equity(anchor_list, 0), 30));

  load_and_generate(game, gen, player, EMPTY_CGP, "AESR");
  assert(get_number_of_anchors(anchor_list) == 1);
  assert(within_epsilon(get_anchor_highest_possible_equity(anchor_list, 0), 8));

  load_and_generate(game, gen, player, EMPTY_CGP, "TNCL");
  assert(get_number_of_anchors(anchor_list) == 1);
  assert(
      within_epsilon(get_anchor_highest_possible_equity(anchor_list, 0), 12));

  load_and_generate(game, gen, player, EMPTY_CGP, "AAAAA");
  assert(get_number_of_anchors(anchor_list) == 1);
  assert(
      within_epsilon(get_anchor_highest_possible_equity(anchor_list, 0), 12));

  load_and_generate(game, gen, player, EMPTY_CGP, "CAAAA");
  assert(get_number_of_anchors(anchor_list) == 1);
  assert(
      within_epsilon(get_anchor_highest_possible_equity(anchor_list, 0), 20));

  load_and_generate(game, gen, player, EMPTY_CGP, "CAKAA");
  assert(get_number_of_anchors(anchor_list) == 1);
  assert(
      within_epsilon(get_anchor_highest_possible_equity(anchor_list, 0), 32));

  load_and_generate(game, gen, player, EMPTY_CGP, "AIERZ");
  assert(get_number_of_anchors(anchor_list) == 1);
  assert(
      within_epsilon(get_anchor_highest_possible_equity(anchor_list, 0), 48));

  load_and_generate(game, gen, player, EMPTY_CGP, "AIERZN");
  assert(get_number_of_anchors(anchor_list) == 1);
  assert(
      within_epsilon(get_anchor_highest_possible_equity(anchor_list, 0), 50));

  load_and_generate(game, gen, player, EMPTY_CGP, "AIERZNL");
  assert(get_number_of_anchors(anchor_list) == 1);
  assert(
      within_epsilon(get_anchor_highest_possible_equity(anchor_list, 0), 102));

  load_and_generate(game, gen, player, EMPTY_CGP, "?");
  assert(get_number_of_anchors(anchor_list) == 1);
  assert(within_epsilon(get_anchor_highest_possible_equity(anchor_list, 0), 0));

  load_and_generate(game, gen, player, EMPTY_CGP, "??");
  assert(get_number_of_anchors(anchor_list) == 1);
  assert(within_epsilon(get_anchor_highest_possible_equity(anchor_list, 0), 0));

  load_and_generate(game, gen, player, EMPTY_CGP, "??OU");
  assert(get_number_of_anchors(anchor_list) == 1);
  assert(within_epsilon(get_anchor_highest_possible_equity(anchor_list, 0), 4));

  load_and_generate(game, gen, player, EMPTY_CGP, "??OUA");
  assert(get_number_of_anchors(anchor_list) == 1);
  assert(within_epsilon(get_anchor_highest_possible_equity(anchor_list, 0), 8));

  load_and_generate(game, gen, player, KA_OPENING_CGP, "EE");
  // KAE and EE
  assert(
      within_epsilon(get_anchor_highest_possible_equity(anchor_list, 0), 10));
  // EKE
  assert(within_epsilon(get_anchor_highest_possible_equity(anchor_list, 1), 9));
  // KAEE
  assert(within_epsilon(get_anchor_highest_possible_equity(anchor_list, 2), 8));
  // EE and E(A)
  assert(within_epsilon(get_anchor_highest_possible_equity(anchor_list, 3), 5));
  // EE and E(A)
  assert(within_epsilon(get_anchor_highest_possible_equity(anchor_list, 4), 5));
  // EEE
  assert(within_epsilon(get_anchor_highest_possible_equity(anchor_list, 5), 3));
  // The rest are prevented by invalid cross sets
  assert(within_epsilon(get_anchor_highest_possible_equity(anchor_list, 6), 0));
  assert(within_epsilon(get_anchor_highest_possible_equity(anchor_list, 7), 0));
  assert(within_epsilon(get_anchor_highest_possible_equity(anchor_list, 8), 0));

  load_and_generate(game, gen, player, KA_OPENING_CGP, "E?");
  // oK, oE, EA
  assert(
      within_epsilon(get_anchor_highest_possible_equity(anchor_list, 0), 10));
  // KA, aE, AE
  assert(
      within_epsilon(get_anchor_highest_possible_equity(anchor_list, 1), 10));
  // KAe, Ee
  assert(within_epsilon(get_anchor_highest_possible_equity(anchor_list, 2), 8));
  // EKA, Ea
  assert(within_epsilon(get_anchor_highest_possible_equity(anchor_list, 3), 8));
  // KAEe
  assert(within_epsilon(get_anchor_highest_possible_equity(anchor_list, 4), 7));
  // E(K)e
  assert(within_epsilon(get_anchor_highest_possible_equity(anchor_list, 5), 7));
  // Ea, EA
  assert(within_epsilon(get_anchor_highest_possible_equity(anchor_list, 6), 3));
  // Ae, eE
  assert(within_epsilon(get_anchor_highest_possible_equity(anchor_list, 7), 3));
  // E(A)a
  assert(within_epsilon(get_anchor_highest_possible_equity(anchor_list, 8), 2));

  load_and_generate(game, gen, player, KA_OPENING_CGP, "J");
  // J(K) veritcally
  assert(
      within_epsilon(get_anchor_highest_possible_equity(anchor_list, 0), 21));
  // J(KA) or (KA)J
  assert(
      within_epsilon(get_anchor_highest_possible_equity(anchor_list, 1), 14));
  // J(A) horitizontally
  assert(within_epsilon(get_anchor_highest_possible_equity(anchor_list, 2), 9));
  // J(A) vertically
  assert(within_epsilon(get_anchor_highest_possible_equity(anchor_list, 3), 9));
  assert(within_epsilon(get_anchor_highest_possible_equity(anchor_list, 4), 0));
  assert(within_epsilon(get_anchor_highest_possible_equity(anchor_list, 5), 0));
  assert(within_epsilon(get_anchor_highest_possible_equity(anchor_list, 6), 0));
  assert(within_epsilon(get_anchor_highest_possible_equity(anchor_list, 7), 0));
  assert(within_epsilon(get_anchor_highest_possible_equity(anchor_list, 8), 0));

  load_and_generate(game, gen, player, AA_OPENING_CGP, "JF");
  // JF, JA, and FA
  assert(
      within_epsilon(get_anchor_highest_possible_equity(anchor_list, 0), 42));
  // JA and JF or FA and FJ
  assert(
      within_epsilon(get_anchor_highest_possible_equity(anchor_list, 1), 25));
  // JAF with J and F doubled
  assert(
      within_epsilon(get_anchor_highest_possible_equity(anchor_list, 2), 25));
  // FAA is in cross set, so JAA and JF are used to score.
  assert(
      within_epsilon(get_anchor_highest_possible_equity(anchor_list, 3), 22));
  // AAJF
  assert(
      within_epsilon(get_anchor_highest_possible_equity(anchor_list, 4), 14));
  // AJF
  assert(
      within_epsilon(get_anchor_highest_possible_equity(anchor_list, 5), 13));
  // Remaining anchors are prevented by invalid cross sets
  assert(within_epsilon(get_anchor_highest_possible_equity(anchor_list, 6), 0));
  assert(within_epsilon(get_anchor_highest_possible_equity(anchor_list, 7), 0));
  assert(within_epsilon(get_anchor_highest_possible_equity(anchor_list, 8), 0));

  // Makeing JA, FA, and JFU, doubling the U on the double letter
  load_and_generate(game, gen, player, AA_OPENING_CGP, "JFU");
  assert(
      within_epsilon(get_anchor_highest_possible_equity(anchor_list, 0), 44));

  // Making KAU (allowed by F in rack cross set) and JUF, doubling the F and J.
  load_and_generate(game, gen, player, KA_OPENING_CGP, "JFU");
  assert(
      within_epsilon(get_anchor_highest_possible_equity(anchor_list, 0), 32));

  load_and_generate(game, gen, player, AA_OPENING_CGP, "JFUG");
  assert(
      within_epsilon(get_anchor_highest_possible_equity(anchor_list, 0), 47));

  load_and_generate(game, gen, player, AA_OPENING_CGP, "JFUGX");
  assert(
      within_epsilon(get_anchor_highest_possible_equity(anchor_list, 0), 61));

  // Reaches the triple word
  load_and_generate(game, gen, player, AA_OPENING_CGP, "JFUGXL");
  assert(
      within_epsilon(get_anchor_highest_possible_equity(anchor_list, 0), 102));

  load_and_generate(game, gen, player, DOUG_V_EMELY_CGP, "Q");
  assert(
      within_epsilon(get_anchor_highest_possible_equity(anchor_list, 0), 22));

  load_and_generate(game, gen, player, DOUG_V_EMELY_CGP, "BD");
  assert(
      within_epsilon(get_anchor_highest_possible_equity(anchor_list, 0), 17));

  load_and_generate(game, gen, player, DOUG_V_EMELY_CGP, "BOH");
  assert(
      within_epsilon(get_anchor_highest_possible_equity(anchor_list, 0), 60));

  load_and_generate(game, gen, player, DOUG_V_EMELY_CGP, "BOHGX");
  assert(
      within_epsilon(get_anchor_highest_possible_equity(anchor_list, 0), 90));

  load_and_generate(game, gen, player, DOUG_V_EMELY_CGP, "BOHGXZ");
  assert(
      within_epsilon(get_anchor_highest_possible_equity(anchor_list, 0), 120));

  load_and_generate(game, gen, player, DOUG_V_EMELY_CGP, "BOHGXZQ");
  assert(
      within_epsilon(get_anchor_highest_possible_equity(anchor_list, 0), 230));

  load_and_generate(game, gen, player, TRIPLE_LETTERS_CGP, "A");

  // WINDYA
  assert(
      within_epsilon(get_anchor_highest_possible_equity(anchor_list, 0), 13));
  // PROTEANA
  assert(
      within_epsilon(get_anchor_highest_possible_equity(anchor_list, 1), 11));
  // ANY horizontally
  // ANY vertically
  // A(P) vertically
  // A(OW) vertically
  assert(within_epsilon(get_anchor_highest_possible_equity(anchor_list, 2), 6));
  assert(within_epsilon(get_anchor_highest_possible_equity(anchor_list, 3), 6));
  assert(within_epsilon(get_anchor_highest_possible_equity(anchor_list, 4), 6));
  assert(within_epsilon(get_anchor_highest_possible_equity(anchor_list, 5), 6));
  // A(EN)
  // AD(A)
  assert(within_epsilon(get_anchor_highest_possible_equity(anchor_list, 6), 5));
  assert(within_epsilon(get_anchor_highest_possible_equity(anchor_list, 7), 5));

  load_and_generate(game, gen, player, TRIPLE_LETTERS_CGP, "Z");
  // Z(P) vertically
  assert(
      within_epsilon(get_anchor_highest_possible_equity(anchor_list, 0), 33));
  // Z(EN) vert
  // Z(EN) horiz
  assert(
      within_epsilon(get_anchor_highest_possible_equity(anchor_list, 1), 32));
  assert(
      within_epsilon(get_anchor_highest_possible_equity(anchor_list, 2), 32));
  // (PROTEAN)Z
  assert(
      within_epsilon(get_anchor_highest_possible_equity(anchor_list, 3), 29));

  load_and_generate(game, gen, player, TRIPLE_LETTERS_CGP, "ZLW");
  // ZEN, ZW, WAD
  assert(
      within_epsilon(get_anchor_highest_possible_equity(anchor_list, 0), 73));
  // ZENLW
  assert(
      within_epsilon(get_anchor_highest_possible_equity(anchor_list, 1), 45));
  // ZLWOW
  assert(
      within_epsilon(get_anchor_highest_possible_equity(anchor_list, 2), 40));

  load_and_generate(game, gen, player, TRIPLE_LETTERS_CGP, "ZLW?");
  // The blank makes all cross sets valid
  // LZW(WINDY)s
  assert(
      within_epsilon(get_anchor_highest_possible_equity(anchor_list, 0), 99));

  load_and_generate(game, gen, player, TRIPLE_LETTERS_CGP, "QZLW");
  // ZQ, ZEN, QAD (L and W are in the AD cross set, but scored using the Q)
  assert(
      within_epsilon(get_anchor_highest_possible_equity(anchor_list, 0), 85));

  load_and_generate(game, gen, player, TRIPLE_DOUBLE_CGP, "K");
  assert(
      within_epsilon(get_anchor_highest_possible_equity(anchor_list, 0), 23));

  load_and_generate(game, gen, player, TRIPLE_DOUBLE_CGP, "KT");
  // KPAVT
  assert(
      within_epsilon(get_anchor_highest_possible_equity(anchor_list, 0), 26));

  load_and_generate(game, gen, player, TRIPLE_DOUBLE_CGP, "KT?");
  // The blank makes PAVE, allowed all letters in the cross set
  // PAVK, KT?
  assert(
      within_epsilon(get_anchor_highest_possible_equity(anchor_list, 0), 39));

  load_and_generate(game, gen, player, BOTTOM_LEFT_RE_CGP, "M");
  assert(within_epsilon(get_anchor_highest_possible_equity(anchor_list, 0), 8));

  load_and_generate(game, gen, player, BOTTOM_LEFT_RE_CGP, "MN");
  assert(
      within_epsilon(get_anchor_highest_possible_equity(anchor_list, 0), 16));

  load_and_generate(game, gen, player, BOTTOM_LEFT_RE_CGP, "MNA");
  assert(
      within_epsilon(get_anchor_highest_possible_equity(anchor_list, 0), 20));

  load_and_generate(game, gen, player, BOTTOM_LEFT_RE_CGP, "MNAU");
  assert(
      within_epsilon(get_anchor_highest_possible_equity(anchor_list, 0), 22));

  load_and_generate(game, gen, player, BOTTOM_LEFT_RE_CGP, "MNAUT");
  assert(
      within_epsilon(get_anchor_highest_possible_equity(anchor_list, 0), 30));

  load_and_generate(game, gen, player, BOTTOM_LEFT_RE_CGP, "MNAUTE");
  assert(
      within_epsilon(get_anchor_highest_possible_equity(anchor_list, 0), 39));

  load_and_generate(game, gen, player, LATER_BETWEEN_DOUBLE_WORDS_CGP, "Z");
  // (L)Z and (R)Z
  assert(
      within_epsilon(get_anchor_highest_possible_equity(anchor_list, 0), 31));
  assert(
      within_epsilon(get_anchor_highest_possible_equity(anchor_list, 1), 31));
  // (LATER)Z
  assert(
      within_epsilon(get_anchor_highest_possible_equity(anchor_list, 2), 30));
  // Z(T)
  assert(
      within_epsilon(get_anchor_highest_possible_equity(anchor_list, 3), 21));

  load_and_generate(game, gen, player, LATER_BETWEEN_DOUBLE_WORDS_CGP, "ZL");
  assert(
      within_epsilon(get_anchor_highest_possible_equity(anchor_list, 0), 64));

  load_and_generate(game, gen, player, LATER_BETWEEN_DOUBLE_WORDS_CGP, "ZLI");
  assert(
      within_epsilon(get_anchor_highest_possible_equity(anchor_list, 0), 68));

  load_and_generate(game, gen, player, LATER_BETWEEN_DOUBLE_WORDS_CGP, "ZLIE");
  assert(
      within_epsilon(get_anchor_highest_possible_equity(anchor_list, 0), 72));

  load_and_generate(game, gen, player, LATER_BETWEEN_DOUBLE_WORDS_CGP, "ZLIER");
  assert(
      within_epsilon(get_anchor_highest_possible_equity(anchor_list, 0), 77));

  load_and_generate(game, gen, player, LATER_BETWEEN_DOUBLE_WORDS_CGP,
                    "ZLIERA");
  assert(
      within_epsilon(get_anchor_highest_possible_equity(anchor_list, 0), 80));

  load_and_generate(game, gen, player, LATER_BETWEEN_DOUBLE_WORDS_CGP,
                    "ZLIERAI");
  assert(
      within_epsilon(get_anchor_highest_possible_equity(anchor_list, 0), 212));

  load_and_generate(game, gen, player, VS_OXY, "A");
  // APACIFYING
  assert(
      within_epsilon(get_anchor_highest_possible_equity(anchor_list, 0), 63));

  load_and_generate(game, gen, player, VS_OXY, "PB");
  assert(
      within_epsilon(get_anchor_highest_possible_equity(anchor_list, 0), 156));

  load_and_generate(game, gen, player, VS_OXY, "PA");
  // Forms DORMPWOOAJ because the A fits in the cross set of T and N.
  assert(
      within_epsilon(get_anchor_highest_possible_equity(anchor_list, 0), 76));

  load_and_generate(game, gen, player, VS_OXY, "PBA");
  assert(
      within_epsilon(get_anchor_highest_possible_equity(anchor_list, 0), 174));

  load_and_generate(game, gen, player, VS_OXY, "Z");
  // ZPACIFYING
  assert(
      within_epsilon(get_anchor_highest_possible_equity(anchor_list, 0), 90));

  load_and_generate(game, gen, player, VS_OXY, "ZE");
  // ZONE
  assert(
      within_epsilon(get_anchor_highest_possible_equity(anchor_list, 0), 160));

  load_and_generate(game, gen, player, VS_OXY, "AZE");
  // UTAZONE
  assert(
      within_epsilon(get_anchor_highest_possible_equity(anchor_list, 0), 184));

  load_and_generate(game, gen, player, VS_OXY, "AZEB");
  // HENBUTAZONE
  assert(
      within_epsilon(get_anchor_highest_possible_equity(anchor_list, 0), 484));

  load_and_generate(game, gen, player, VS_OXY, "AZEBP");
  // YPHENBUTAZONE
  assert(
      within_epsilon(get_anchor_highest_possible_equity(anchor_list, 0), 604));

  load_and_generate(game, gen, player, VS_OXY, "AZEBPX");
  // A2 A(Y)X(HEN)P(UT)EZ(ON)B
  assert(
      within_epsilon(get_anchor_highest_possible_equity(anchor_list, 0), 740));

  load_and_generate(game, gen, player, VS_OXY, "AZEBPXO");
  // A1 OA(Y)X(HEN)P(UT)EZ(ON)B
  assert(
      within_epsilon(get_anchor_highest_possible_equity(anchor_list, 0), 1924));

  load_and_generate(game, gen, player, VS_OXY, "AZEBPQO");
  // A1 OA(Y)Q(HEN)P(UT)EZ(ON)B
  // Only the letters AZEBPO are required to form acceptable
  // plays in all cross sets
  assert(
      within_epsilon(get_anchor_highest_possible_equity(anchor_list, 0), 2036));

  destroy_game(game);
  destroy_generator(gen);
}

void test_shadow_equity(TestConfig *testconfig) {
  const Config *config = get_csw_config(testconfig);
  Game *game = create_game(config);
  MoveGen *gen = create_generator(
      config_get_num_plays(config),
      letter_distribution_get_size(config_get_letter_distribution(config)));
  Player *player = game_get_player(game, 0);
  const KLV *klv = player_get_klv(player);
  AnchorList *anchor_list = gen_get_anchor_list(gen);
  LetterDistribution *ld = game_get_ld(game);
  int ld_size = letter_distribution_get_size(ld);
  // This test checks equity, so set the move sorting
  // to equity.
  player_set_move_sort_type(player, MOVE_SORT_EQUITY);

  // Check best leave values for a give rack.
  Rack *leave_rack = create_rack(ld_size);
  load_and_generate(game, gen, player, EMPTY_CGP, "ERSVQUW");

  double *best_leaves = gen_get_best_leaves(gen);

  set_rack_to_string(ld, leave_rack, "");
  assert(within_epsilon(best_leaves[0], klv_get_leave_value(klv, leave_rack)));

  set_rack_to_string(ld, leave_rack, "S");
  assert(within_epsilon(best_leaves[1], klv_get_leave_value(klv, leave_rack)));

  set_rack_to_string(ld, leave_rack, "ES");
  assert(within_epsilon(best_leaves[2], klv_get_leave_value(klv, leave_rack)));

  set_rack_to_string(ld, leave_rack, "ERS");
  assert(within_epsilon(best_leaves[3], klv_get_leave_value(klv, leave_rack)));

  set_rack_to_string(ld, leave_rack, "EQSU");
  assert(within_epsilon(best_leaves[4], klv_get_leave_value(klv, leave_rack)));

  set_rack_to_string(ld, leave_rack, "EQRSU");
  assert(within_epsilon(best_leaves[5], klv_get_leave_value(klv, leave_rack)));

  set_rack_to_string(ld, leave_rack, "EQRSUV");
  assert(within_epsilon(best_leaves[6], klv_get_leave_value(klv, leave_rack)));

  load_and_generate(game, gen, player, EMPTY_CGP, "ESQW");
  set_rack_to_string(ld, leave_rack, "ES");
  assert(within_epsilon(get_anchor_highest_possible_equity(anchor_list, 0),
                        28 + klv_get_leave_value(klv, leave_rack)));

  destroy_game(game);
  destroy_generator(gen);
  destroy_rack(leave_rack);
}

void test_shadow_top_move(TestConfig *testconfig) {
  const Config *config = get_csw_config(testconfig);
  Game *game = create_game(config);
  MoveGen *gen = create_generator(
      config_get_num_plays(config),
      letter_distribution_get_size(config_get_letter_distribution(config)));
  Player *player = game_get_player(game, 0);
  MoveList *move_list = gen_get_move_list(gen);

  player_set_move_sort_type(player, MOVE_SORT_EQUITY);
  player_set_move_record_type(player, MOVE_RECORD_BEST);

  // Top play should be L1 Q(I)
  load_and_generate(game, gen, player, UEY_CGP, "ACEQOOV");
  assert(within_epsilon(get_score(move_list_get_move(move_list, 0)), 21));

  destroy_game(game);
  destroy_generator(gen);
}

void test_shadow(TestConfig *testconfig) {
  test_shadow_score(testconfig);
  test_shadow_equity(testconfig);
  test_shadow_top_move(testconfig);
}