#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "../src/ent/anchor.h"
#include "../src/ent/config.h"
#include "../src/ent/game.h"

#include "../src/impl/gameplay.h"

#include "move_gen_pi.h"

#include "test_constants.h"
#include "test_util.h"

void load_and_generate(Game *game, MoveList *move_list, Player *player,
                       const char *cgp, const char *rack) {
  const LetterDistribution *ld = game_get_ld(game);
  Rack *player_rack = player_get_rack(player);

  game_load_cgp(game, cgp);
  rack_set_to_string(ld, player_rack, rack);
  generate_moves_for_game(game, 0, move_list);
  AnchorList *anchor_list = gen_get_anchor_list(0);
  double previous_equity = 10000000;
  int number_of_anchors = anchor_list_get_count(anchor_list);
  for (int i = 0; i < number_of_anchors; i++) {
    double equity = anchor_get_highest_possible_equity(anchor_list, i);
    assert(equity <= previous_equity);
    previous_equity = equity;
  }
}

void test_shadow_score() {
  Config *config = create_config_or_die(
      "setoptions lex CSW21 s1 equity s2 equity r1 all r2 all numplays 1");
  Game *game = game_create(config);
  Player *player = game_get_player(game, 0);
  MoveList *move_list = move_list_create(1000);

  // This test checks scores only, so set the player move sorting
  // to sort by score.
  player_set_move_sort_type(player, MOVE_SORT_SCORE);

  load_and_generate(game, move_list, player, EMPTY_CGP, "OU");

  // Get the anchor list after calling generate
  // so a movegen internal struct is created.
  AnchorList *anchor_list = gen_get_anchor_list(0);

  assert(anchor_list_get_count(anchor_list) == 1);
  assert(within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 4));

  load_and_generate(game, move_list, player, EMPTY_CGP, "ID");
  assert(anchor_list_get_count(anchor_list) == 1);
  assert(within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 6));

  load_and_generate(game, move_list, player, EMPTY_CGP, "AX");
  assert(anchor_list_get_count(anchor_list) == 1);
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 18));

  load_and_generate(game, move_list, player, EMPTY_CGP, "BD");
  assert(anchor_list_get_count(anchor_list) == 1);
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 10));

  load_and_generate(game, move_list, player, EMPTY_CGP, "QK");
  assert(anchor_list_get_count(anchor_list) == 1);
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 30));

  load_and_generate(game, move_list, player, EMPTY_CGP, "AESR");
  assert(anchor_list_get_count(anchor_list) == 1);
  assert(within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 8));

  load_and_generate(game, move_list, player, EMPTY_CGP, "TNCL");
  assert(anchor_list_get_count(anchor_list) == 1);
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 12));

  load_and_generate(game, move_list, player, EMPTY_CGP, "AAAAA");
  assert(anchor_list_get_count(anchor_list) == 1);
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 12));

  load_and_generate(game, move_list, player, EMPTY_CGP, "CAAAA");
  assert(anchor_list_get_count(anchor_list) == 1);
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 20));

  load_and_generate(game, move_list, player, EMPTY_CGP, "CAKAA");
  assert(anchor_list_get_count(anchor_list) == 1);
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 32));

  load_and_generate(game, move_list, player, EMPTY_CGP, "AIERZ");
  assert(anchor_list_get_count(anchor_list) == 1);
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 48));

  load_and_generate(game, move_list, player, EMPTY_CGP, "AIERZN");
  assert(anchor_list_get_count(anchor_list) == 1);
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 50));

  load_and_generate(game, move_list, player, EMPTY_CGP, "AIERZNL");
  assert(anchor_list_get_count(anchor_list) == 1);
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 102));

  load_and_generate(game, move_list, player, EMPTY_CGP, "?");
  assert(anchor_list_get_count(anchor_list) == 1);
  assert(within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 0));

  load_and_generate(game, move_list, player, EMPTY_CGP, "??");
  assert(anchor_list_get_count(anchor_list) == 1);
  assert(within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 0));

  load_and_generate(game, move_list, player, EMPTY_CGP, "??OU");
  assert(anchor_list_get_count(anchor_list) == 1);
  assert(within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 4));

  load_and_generate(game, move_list, player, EMPTY_CGP, "??OUA");
  assert(anchor_list_get_count(anchor_list) == 1);
  assert(within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 8));

  load_and_generate(game, move_list, player, KA_OPENING_CGP, "EE");
  // KAE and EE
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 10));
  // EKE
  assert(within_epsilon(anchor_get_highest_possible_equity(anchor_list, 1), 9));
  // KAEE
  assert(within_epsilon(anchor_get_highest_possible_equity(anchor_list, 2), 8));
  // EE and E(A)
  assert(within_epsilon(anchor_get_highest_possible_equity(anchor_list, 3), 5));
  // EE and E(A)
  assert(within_epsilon(anchor_get_highest_possible_equity(anchor_list, 4), 5));
  // EEE
  assert(within_epsilon(anchor_get_highest_possible_equity(anchor_list, 5), 3));
  // The rest are prevented by invalid cross sets
  assert(within_epsilon(anchor_get_highest_possible_equity(anchor_list, 6), 0));
  assert(within_epsilon(anchor_get_highest_possible_equity(anchor_list, 7), 0));
  assert(within_epsilon(anchor_get_highest_possible_equity(anchor_list, 8), 0));

  load_and_generate(game, move_list, player, KA_OPENING_CGP, "E?");
  // oK, oE, EA
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 10));
  // KA, aE, AE
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 1), 10));
  // KAe, Ee
  assert(within_epsilon(anchor_get_highest_possible_equity(anchor_list, 2), 8));
  // EKA, Ea
  assert(within_epsilon(anchor_get_highest_possible_equity(anchor_list, 3), 8));
  // KAEe
  assert(within_epsilon(anchor_get_highest_possible_equity(anchor_list, 4), 7));
  // E(K)e
  assert(within_epsilon(anchor_get_highest_possible_equity(anchor_list, 5), 7));
  // Ea, EA
  assert(within_epsilon(anchor_get_highest_possible_equity(anchor_list, 6), 3));
  // Ae, eE
  assert(within_epsilon(anchor_get_highest_possible_equity(anchor_list, 7), 3));
  // E(A)a
  assert(within_epsilon(anchor_get_highest_possible_equity(anchor_list, 8), 2));

  load_and_generate(game, move_list, player, KA_OPENING_CGP, "J");
  // J(K) veritcally
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 21));
  // J(KA) or (KA)J
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 1), 14));
  // J(A) horitizontally
  assert(within_epsilon(anchor_get_highest_possible_equity(anchor_list, 2), 9));
  // J(A) vertically
  assert(within_epsilon(anchor_get_highest_possible_equity(anchor_list, 3), 9));
  assert(within_epsilon(anchor_get_highest_possible_equity(anchor_list, 4), 0));
  assert(within_epsilon(anchor_get_highest_possible_equity(anchor_list, 5), 0));
  assert(within_epsilon(anchor_get_highest_possible_equity(anchor_list, 6), 0));
  assert(within_epsilon(anchor_get_highest_possible_equity(anchor_list, 7), 0));
  assert(within_epsilon(anchor_get_highest_possible_equity(anchor_list, 8), 0));

  load_and_generate(game, move_list, player, AA_OPENING_CGP, "JF");
  // JF, JA, and FA
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 42));
  // JA and JF or FA and FJ
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 1), 25));
  // JAF with J and F doubled
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 2), 25));
  // FAA is in cross set, so JAA and JF are used to score.
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 3), 22));
  // AAJF
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 4), 14));
  // AJF
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 5), 13));
  // Remaining anchors are prevented by invalid cross sets
  assert(within_epsilon(anchor_get_highest_possible_equity(anchor_list, 6), 0));
  assert(within_epsilon(anchor_get_highest_possible_equity(anchor_list, 7), 0));
  assert(within_epsilon(anchor_get_highest_possible_equity(anchor_list, 8), 0));

  // Makeing JA, FA, and JFU, doubling the U on the double letter
  load_and_generate(game, move_list, player, AA_OPENING_CGP, "JFU");
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 44));

  // Making KAU (board_is_letter_allowed_in_cross_set by F in rack cross set)
  // and JUF, doubling the F and J.
  load_and_generate(game, move_list, player, KA_OPENING_CGP, "JFU");
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 32));

  load_and_generate(game, move_list, player, AA_OPENING_CGP, "JFUG");
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 47));

  load_and_generate(game, move_list, player, AA_OPENING_CGP, "JFUGX");
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 61));

  // Reaches the triple word
  load_and_generate(game, move_list, player, AA_OPENING_CGP, "JFUGXL");
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 102));

  load_and_generate(game, move_list, player, DOUG_V_EMELY_CGP, "Q");
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 22));

  load_and_generate(game, move_list, player, DOUG_V_EMELY_CGP, "BD");
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 17));

  load_and_generate(game, move_list, player, DOUG_V_EMELY_CGP, "BOH");
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 60));

  load_and_generate(game, move_list, player, DOUG_V_EMELY_CGP, "BOHGX");
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 90));

  load_and_generate(game, move_list, player, DOUG_V_EMELY_CGP, "BOHGXZ");
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 120));

  load_and_generate(game, move_list, player, DOUG_V_EMELY_CGP, "BOHGXZQ");
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 230));

  load_and_generate(game, move_list, player, TRIPLE_LETTERS_CGP, "A");

  // WINDYA
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 13));
  // PROTEANA
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 1), 11));
  // ANY horizontally
  // ANY vertically
  // A(P) vertically
  // A(OW) vertically
  assert(within_epsilon(anchor_get_highest_possible_equity(anchor_list, 2), 6));
  assert(within_epsilon(anchor_get_highest_possible_equity(anchor_list, 3), 6));
  assert(within_epsilon(anchor_get_highest_possible_equity(anchor_list, 4), 6));
  assert(within_epsilon(anchor_get_highest_possible_equity(anchor_list, 5), 6));
  // A(EN)
  // AD(A)
  assert(within_epsilon(anchor_get_highest_possible_equity(anchor_list, 6), 5));
  assert(within_epsilon(anchor_get_highest_possible_equity(anchor_list, 7), 5));

  load_and_generate(game, move_list, player, TRIPLE_LETTERS_CGP, "Z");
  // Z(P) vertically
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 33));
  // Z(EN) vert
  // Z(EN) horiz
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 1), 32));
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 2), 32));
  // (PROTEAN)Z
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 3), 29));

  load_and_generate(game, move_list, player, TRIPLE_LETTERS_CGP, "ZLW");
  // ZEN, ZW, WAD
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 73));
  // ZENLW
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 1), 45));
  // ZLWOW
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 2), 40));

  load_and_generate(game, move_list, player, TRIPLE_LETTERS_CGP, "ZLW?");
  // The blank makes all cross sets valid
  // LZW(WINDY)s
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 99));

  load_and_generate(game, move_list, player, TRIPLE_LETTERS_CGP, "QZLW");
  // ZQ, ZEN, QAD (L and W are in the AD cross set, but scored using the Q)
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 85));

  load_and_generate(game, move_list, player, TRIPLE_DOUBLE_CGP, "K");
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 23));

  load_and_generate(game, move_list, player, TRIPLE_DOUBLE_CGP, "KT");
  // KPAVT
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 26));

  load_and_generate(game, move_list, player, TRIPLE_DOUBLE_CGP, "KT?");
  // The blank makes PAVE, board_is_letter_allowed_in_cross_set all letters in
  // the cross set PAVK, KT?
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 39));

  load_and_generate(game, move_list, player, BOTTOM_LEFT_RE_CGP, "M");
  assert(within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 8));

  load_and_generate(game, move_list, player, BOTTOM_LEFT_RE_CGP, "MN");
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 16));

  load_and_generate(game, move_list, player, BOTTOM_LEFT_RE_CGP, "MNA");
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 20));

  load_and_generate(game, move_list, player, BOTTOM_LEFT_RE_CGP, "MNAU");
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 22));

  load_and_generate(game, move_list, player, BOTTOM_LEFT_RE_CGP, "MNAUT");
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 30));

  load_and_generate(game, move_list, player, BOTTOM_LEFT_RE_CGP, "MNAUTE");
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 39));

  load_and_generate(game, move_list, player, LATER_BETWEEN_DOUBLE_WORDS_CGP,
                    "Z");
  // (L)Z and (R)Z
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 31));
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 1), 31));
  // (LATER)Z
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 2), 30));
  // Z(T)
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 3), 21));

  load_and_generate(game, move_list, player, LATER_BETWEEN_DOUBLE_WORDS_CGP,
                    "ZL");
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 64));

  load_and_generate(game, move_list, player, LATER_BETWEEN_DOUBLE_WORDS_CGP,
                    "ZLI");
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 68));

  load_and_generate(game, move_list, player, LATER_BETWEEN_DOUBLE_WORDS_CGP,
                    "ZLIE");
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 72));

  load_and_generate(game, move_list, player, LATER_BETWEEN_DOUBLE_WORDS_CGP,
                    "ZLIER");
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 77));

  load_and_generate(game, move_list, player, LATER_BETWEEN_DOUBLE_WORDS_CGP,
                    "ZLIERA");
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 80));

  load_and_generate(game, move_list, player, LATER_BETWEEN_DOUBLE_WORDS_CGP,
                    "ZLIERAI");
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 212));

  load_and_generate(game, move_list, player, VS_OXY, "A");
  // APACIFYING
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 63));

  load_and_generate(game, move_list, player, VS_OXY, "PB");
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 156));

  load_and_generate(game, move_list, player, VS_OXY, "PA");
  // Forms DORMPWOOAJ because the A fits in the cross set of T and N.
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 76));

  load_and_generate(game, move_list, player, VS_OXY, "PBA");
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 174));

  load_and_generate(game, move_list, player, VS_OXY, "Z");
  // ZPACIFYING
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 90));

  load_and_generate(game, move_list, player, VS_OXY, "ZE");
  // ZONE
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 160));

  load_and_generate(game, move_list, player, VS_OXY, "AZE");
  // UTAZONE
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 184));

  load_and_generate(game, move_list, player, VS_OXY, "AZEB");
  // HENBUTAZONE
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 484));

  load_and_generate(game, move_list, player, VS_OXY, "AZEBP");
  // YPHENBUTAZONE
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 604));

  load_and_generate(game, move_list, player, VS_OXY, "AZEBPX");
  // A2 A(Y)X(HEN)P(UT)EZ(ON)B
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 740));

  load_and_generate(game, move_list, player, VS_OXY, "AZEBPXO");
  // A1 OA(Y)X(HEN)P(UT)EZ(ON)B
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 1924));

  load_and_generate(game, move_list, player, VS_OXY, "AZEBPQO");
  // A1 OA(Y)Q(HEN)P(UT)EZ(ON)B
  // Only the letters AZEBPO are required to form acceptable
  // plays in all cross sets
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 2036));

  game_destroy(game);
  move_list_destroy(move_list);
  config_destroy(config);
}

void test_shadow_top_move() {
  Config *config = create_config_or_die(
      "setoptions lex CSW21 s1 equity s2 equity r1 all r2 all numplays 1");
  Game *game = game_create(config);
  Player *player = game_get_player(game, 0);
  MoveList *move_list = move_list_create(1);

  player_set_move_sort_type(player, MOVE_SORT_EQUITY);
  player_set_move_record_type(player, MOVE_RECORD_BEST);

  // Top play should be L1 Q(I)
  load_and_generate(game, move_list, player, UEY_CGP, "ACEQOOV");
  assert(within_epsilon(move_get_score(move_list_get_move(move_list, 0)), 21));

  move_list_destroy(move_list);
  game_destroy(game);
  config_destroy(config);
}

void test_shadow() {
  test_shadow_score();
  test_shadow_top_move();
}