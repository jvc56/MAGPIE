#include <assert.h>

#include "../../src/def/move_defs.h"

#include "../../src/ent/anchor.h"
#include "../../src/ent/config.h"
#include "../../src/ent/game.h"
#include "../../src/ent/letter_distribution.h"
#include "../../src/ent/move.h"
#include "../../src/ent/player.h"
#include "../../src/ent/rack.h"

#include "../../src/impl/cgp.h"
#include "../../src/impl/gameplay.h"

#include "../pi/move_gen_pi.h"

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
  assert(anchor_list_get_count(anchor_list) == 6);

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

  load_and_generate(game, move_list, player, KA_OPENING_CGP, "E?");
  // 7G oE
  assert(within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 8));
  // 9G aE
  assert(within_epsilon(anchor_get_highest_possible_equity(anchor_list, 1), 8));
  // I7 Ee
  assert(within_epsilon(anchor_get_highest_possible_equity(anchor_list, 2), 8));
  // F7 Es (only the blank can hook to the left with sKA or aKA or oKA)
  assert(within_epsilon(anchor_get_highest_possible_equity(anchor_list, 3), 7));
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
  assert(anchor_list_get_count(anchor_list) == 4);
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

  load_and_generate(game, move_list, player, AA_OPENING_CGP, "JF");
  assert(anchor_list_get_count(anchor_list) == 6);

  // JF, JA, and FA
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 42));
  // JA and JF or FA and FJ
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 1), 25));
  // JAF with J and F doubled
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 2), 25));
  // F7 JF (only F can hook AA)
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 3), 18));
  // AAJF
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 4), 14));
  // AJF
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 5), 13));
  // Remaining anchors are prevented by invalid cross sets

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
  // WINDY is not extendable, so there is no 22 for WINDYQ.
  // The highest anchor is through the W in WINDY, recording 14 for QW, as W is
  // extendable with a Q to the left for Q(W)ERTY. Note there is no 22 for DQ
  // or 14 for YQ despite HINDQUARTER and TRIPTYQUE. Since no on the rack
  // extends D or Y to the left, the right_extension_set is applied, filtering
  // these anchors because no words _begin_ with DQ or YQ.
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 14));
  // The next highest anchor is through the I in WINDY: 11 for QI.
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 1), 11));

  load_and_generate(game, move_list, player, DOUG_V_EMELY_CGP, "BD");
  // WINDY is not extendable, so there is no 17 for WINDYBD.
  // The highest anchor is 14 for 7H BD.
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 14));

  load_and_generate(game, move_list, player, DOUG_V_EMELY_CGP, "BOH");
  // WINDY is not extendable, so there is no 60 for BOHWINDY.
  // The highest anchor is 24 for 7G OBH.
  //   only B can hook Y for BY
  //   only O can hook D for OD
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 24));

  load_and_generate(game, move_list, player, DOUG_V_EMELY_CGP, "BOHGX");
  // WINDY is not extendable, so there is no 90 for BOHWINDYGX.
  // The highest anchor is 44 for D8 (W)BOHXG.
  //   using restricted hooks the highest horizontal plays are at 7A for 36.
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 44));

  load_and_generate(game, move_list, player, DOUG_V_EMELY_CGP, "BOHGXZ");
  // WINDY is not extendable, so there is no 120 for BOHWINDYGXZ.
  // The highest anchor is 116 for the 2x2 E5 BOH(I)GXZ.
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 116));

  load_and_generate(game, move_list, player, DOUG_V_EMELY_CGP, "BOHGXZQ");
  // WINDY is not extendable, so there is no 230 for BOHWINDYGXZQ.
  // The highest anchor is 206 for the 2x2 E5 BOH(I)GXZQ.
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 206));

  load_and_generate(game, move_list, player, TRIPLE_LETTERS_CGP, "A");

  // WINDY is not extendable, so there is no 13 for WINDYA.
  // H6 A(NY) vertical
  // H6 A(NY) horizontal
  // B6 A(P)
  // D6 A(OW)
  // F6 A(EN)

  assert(within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 6));
  assert(within_epsilon(anchor_get_highest_possible_equity(anchor_list, 1), 6));
  assert(within_epsilon(anchor_get_highest_possible_equity(anchor_list, 3), 6));
  assert(within_epsilon(anchor_get_highest_possible_equity(anchor_list, 4), 5));

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
  // (AD)Z
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 3), 23));

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
  // 6F ZWaL
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 79));

  load_and_generate(game, move_list, player, TRIPLE_LETTERS_CGP, "QZLW");
  // 6G ZQ making ZEN and QAD. Only L and W are in the AD cross set, but score
  // it using the Q. This is a limitation of tile restriction in shadow. L and W
  // give multiple possibilities for this square so we can't restrict it to a
  // single tile.
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 85));

  load_and_generate(game, move_list, player, TRIPLE_DOUBLE_CGP, "K");
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 13));

  load_and_generate(game, move_list, player, TRIPLE_DOUBLE_CGP, "KT");
  // KPAVT
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 20));

  load_and_generate(game, move_list, player, TRIPLE_DOUBLE_CGP, "KT?");
  // 10A TsPAVK. only the blank can left-extend PAV.
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 24));

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
  // Z(T)
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 21));
  // (A)Z
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 1), 11));
  // Z(E)
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 2), 11));
  // Z(A)
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 3), 11));

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
  // 5D ZE(LATER)RIL (not 6F ZLERI: there's no TLS Z hotspots)
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 76));

  load_and_generate(game, move_list, player, LATER_BETWEEN_DOUBLE_WORDS_CGP,
                    "ZLIERA");
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 80));

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
  // 5D ZE(LATER)RIL (not 6F ZLERI: there's no TLS Z hotspots)
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 76));

  load_and_generate(game, move_list, player, LATER_BETWEEN_DOUBLE_WORDS_CGP,
                    "ZLIERA");
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 80));

  load_and_generate(game, move_list, player, LATER_BETWEEN_DOUBLE_WORDS_CGP,
                    "ZLIERAI");
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 212));

  load_and_generate(game, move_list, player, VS_OXY, "A");
  // No APACIFYING: PACIFYING not extendable with an A.
  // A(WAKEnERS)/(UT)A
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 18));

  load_and_generate(game, move_list, player, VS_OXY, "O");
  // O(PACIFYING)
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 63));

  load_and_generate(game, move_list, player, VS_OXY, "E");
  // E(JACULATING)/(ON)E > E(PACIFYING)
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 72));

  load_and_generate(game, move_list, player, VS_OXY, "PB");
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 156));

  load_and_generate(game, move_list, player, VS_OXY, "PA");
  // No B6 DORMPWOOAJ for 76. Only the A fits in the cross sets of T and N,
  // and it can't be in both of those squares.
  // Best is A3 (Y)P(HEN) hooking P(REQUALIFIED) for 50
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 50));

  load_and_generate(game, move_list, player, VS_OXY, "PBA");
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 174));

  load_and_generate(game, move_list, player, VS_OXY, "Z");
  // No ZPACIFYING
  // TZ/ZA
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 42));

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
  // No A2 A(Y)X(HEN)P(UT)EZ(ON)B for 740
  //  only P hooks REQUALIFIED
  //  only B hooks RAINWASHING
  //  only A hooks WAKENERS
  //  only Z hooks ONETIME
  //  only E hooks JACULATING
  // Best is A2 X(Y)P(HEN)B(UT)AZ(ON)E for 686
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 686));

  load_and_generate(game, move_list, player, VS_OXY, "AZEBPXO");
  // No A1 OA(Y)X(HEN)P(UT)EZ(ON)B for 1924
  //  only O hooks PACIFYING
  //  only P hooks REQUALIFIED
  //  only B hooks RAINWASHING
  //  only A hooks WAKENERS
  //  only Z hooks ONETIME
  //  only E hooks JACULATING
  // Best is A1 OX(Y)P(HEN)B(UT)AZ(ON)E for 1780
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 1780));

  load_and_generate(game, move_list, player, VS_OXY, "AZEBPQO");
  // A1 OQ(Y)P(HEN)B(UT)AZ(ON)E
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 1836));

  game_reset(game);
  char qi_qis[300] =
      "15/15/15/15/15/15/15/6QI7/6I8/6S8/15/15/15/15/15 FRUITED/EGGCUPS 22/12 "
      "0 lex CSW21";
  load_and_generate(game, move_list, player, qi_qis, "FRUITED");
  AnchorList *al = gen_get_anchor_list(0);
  // 8g (QI)DURFITE 128
  // h8 (I)DURFITE 103
  // f9 UFTRIDE 88
  // 7g EF(QIS)RTUDI 79
  // 10b FURED(S)IT 74
  // 9c DURT(I)FIE 70
  // 7h FURTIDE 69
  // h10 DUFTIE 45
  // f10 FURIDE 35
  assert(anchor_list_get_count(al) == 8);
  assert(within_epsilon(anchor_get_highest_possible_equity(al, 0), 128));
  assert(anchor_get_row(al, 0) == 7);
  assert(anchor_get_col(al, 0) == 7);
  assert(anchor_get_dir(al, 0) == BOARD_HORIZONTAL_DIRECTION);

  assert(within_epsilon(anchor_get_highest_possible_equity(al, 1), 103));
  assert(anchor_get_row(al, 1) == 7);
  assert(anchor_get_col(al, 1) == 7);
  assert(anchor_get_dir(al, 1) == BOARD_VERTICAL_DIRECTION);

  // f9 UFTRIDE for 88, not h9 DURFITE for 100 (which does not fit)
  assert(within_epsilon(anchor_get_highest_possible_equity(al, 2), 88));
  assert(anchor_get_row(al, 2) == 5);
  assert(anchor_get_col(al, 2) == 8);
  assert(anchor_get_dir(al, 2) == BOARD_VERTICAL_DIRECTION);

  game_reset(game);
  char shuttled_ravioli[300] =
      "14Z/5V7NA/5R4P2AN/5O1N1ARGUFY/5TOO2I2F1/4T1COWKS2E1/2REE1UN2A2R1/"
      "1RAVIoLI2G3Q/1EXON1IN2E1P1A/1C1L3GEM2AHI/BEMUD2SHUTTlED/1D1E8AI1/"
      "YET9IS1/ODA9ST1/W1JOLLER7 BII/EO 477/388 0 lex CSW21";
  load_and_generate(game, move_list, player, shuttled_ravioli, "BII");

  // M3 B(U)I 25
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 25));

  // 7J I(A)IB(R) 16
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 1), 16));

  // 9B (EXON)I(IN) 15
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 2), 15));

  // 4H (N)I(ARGUFY) 15
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 3), 15));

  // C4 BII(RAX) 15
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 3), 15));

  // J10 (MU)IIB 15
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 4), 15));

  // 12K II(AI) 14
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 5), 14));

  game_reset(game);
  char toeless[300] =
      "15/15/15/15/15/15/5Q2J6/5UVAE6/5I2U6/5Z9/15/15/15/15/15 TOELESS/EEGIPRW "
      "42/38 0 lex CSW21";
  load_and_generate(game, move_list, player, toeless, "TOELESS");

  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 86));

  game_reset(game);
  char addle[300] =
      "5E1p7/1A1C1G1E5S1/1JIZ1G1R3V1P1/HO1APE1A3U1I1/OW1RAD1EXFIL1T1/"
      "MA2W2OI2G1T1/IN2K2N2MOBE1/E2FYRDS2o1ANS/3O6V1N1H/3U6ADD1E/3S6BOO1E/"
      "3T6LOR1L/INCITANT1AYRE2/3E5U5/3REQUITE5 L/I 467/473 0 lex CSW21";
  load_and_generate(game, move_list, player, addle, "L");

  // 15D (REQUITE)L
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 17));

  // 5H (EXFIL)L(T)
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 1), 17));

  // 3L (V)I(P)
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 2), 16));

  // F13 (A)L(Q)
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 3), 14));

  // 5A (OW)L(RAD)
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 4), 10));

  // 10K (ADD)L(E)
  assert(within_epsilon(anchor_get_highest_possible_equity(anchor_list, 5), 9));

  game_reset(game);
  char magpies[300] =
      "15/15/15/15/15/15/15/MAGPIE2LUMBAGO/15/15/15/15/15/15/15 SS/Q "
      "0/0 0 lex CSW21";
  load_and_generate(game, move_list, player, magpies, "SS");

  // G7 SS hooking MAGPIES for 15, not (MAGPIES)SS(LUMBAGO) for 50.
  // LUMBAGO is not left-extendable with an S.
  assert(
      within_epsilon(anchor_get_highest_possible_equity(anchor_list, 0), 15));

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