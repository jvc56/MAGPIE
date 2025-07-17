#include <assert.h>

#include "../../src/def/move_defs.h"

#include "../../src/ent/anchor.h"
#include "../../src/ent/game.h"
#include "../../src/ent/letter_distribution.h"
#include "../../src/ent/move.h"
#include "../../src/ent/player.h"
#include "../../src/ent/rack.h"

#include "../../src/impl/cgp.h"
#include "../../src/impl/config.h"
#include "../../src/impl/gameplay.h"

#include "test_constants.h"
#include "test_util.h"

void load_and_shadow(Game *game, const Player *player, const char *cgp,
                     const char *rack, AnchorHeap *sorted_anchors) {
  const LetterDistribution *ld = game_get_ld(game);
  Rack *player_rack = player_get_rack(player);

  load_cgp_or_die(game, cgp);
  rack_set_to_string(ld, player_rack, rack);
  generate_anchors_for_test(game);
  extract_sorted_anchors_for_test(sorted_anchors);
  Equity previous_equity = EQUITY_MAX_VALUE;
  const int number_of_anchors = sorted_anchors->count;
  for (int i = 0; i < number_of_anchors; i++) {
    const Equity equity = sorted_anchors->anchors[i].highest_possible_equity;
    assert(equity <= previous_equity);
    previous_equity = equity;
  }
}

void load_and_generate_moves(Game *game, MoveList *move_list,
                             const Player *player, const char *cgp,
                             const char *rack) {
  const LetterDistribution *ld = game_get_ld(game);
  Rack *player_rack = player_get_rack(player);
  const MoveGenArgs move_gen_args = {
      .game = game,
      .move_list = move_list,
      .thread_index = 0,
      .max_equity_diff = 0,
  };

  load_cgp_or_die(game, cgp);
  rack_set_to_string(ld, player_rack, rack);
  generate_moves_for_game(&move_gen_args);
}

void test_shadow_score(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  Game *game = config_game_create(config);
  Player *player = game_get_player(game, 0);

  // This test checks scores only, so set the player move sorting
  // to sort by score.
  player_set_move_sort_type(player, MOVE_SORT_SCORE);

  AnchorHeap anchor_list;
  load_and_shadow(game, player, EMPTY_CGP, "OU", &anchor_list);

  assert(anchor_list.count == 1);
  assert_anchor_equity_int(&anchor_list, 0, 4);

  load_and_shadow(game, player, EMPTY_CGP, "ID", &anchor_list);
  assert(anchor_list.count == 1);
  assert_anchor_equity_int(&anchor_list, 0, 6);

  load_and_shadow(game, player, EMPTY_CGP, "AX", &anchor_list);
  assert(anchor_list.count == 1);
  assert_anchor_equity_int(&anchor_list, 0, 18);

  load_and_shadow(game, player, EMPTY_CGP, "BD", &anchor_list);
  assert(anchor_list.count == 1);
  assert_anchor_equity_int(&anchor_list, 0, 10);

  load_and_shadow(game, player, EMPTY_CGP, "QK", &anchor_list);
  assert(anchor_list.count == 1);
  assert_anchor_equity_int(&anchor_list, 0, 30);

  load_and_shadow(game, player, EMPTY_CGP, "AESR", &anchor_list);
  assert(anchor_list.count == 1);
  assert_anchor_equity_int(&anchor_list, 0, 8);

  load_and_shadow(game, player, EMPTY_CGP, "TNCL", &anchor_list);
  assert(anchor_list.count == 1);
  assert_anchor_equity_int(&anchor_list, 0, 12);

  load_and_shadow(game, player, EMPTY_CGP, "AAAAA", &anchor_list);
  assert(anchor_list.count == 1);
  assert_anchor_equity_int(&anchor_list, 0, 12);

  load_and_shadow(game, player, EMPTY_CGP, "CAAAA", &anchor_list);
  assert(anchor_list.count == 1);
  assert_anchor_equity_int(&anchor_list, 0, 20);

  load_and_shadow(game, player, EMPTY_CGP, "CAKAA", &anchor_list);
  assert(anchor_list.count == 1);
  assert_anchor_equity_int(&anchor_list, 0, 32);

  load_and_shadow(game, player, EMPTY_CGP, "AIERZ", &anchor_list);
  assert(anchor_list.count == 1);
  assert_anchor_equity_int(&anchor_list, 0, 48);

  load_and_shadow(game, player, EMPTY_CGP, "AIERZN", &anchor_list);
  assert(anchor_list.count == 1);
  assert_anchor_equity_int(&anchor_list, 0, 50);

  load_and_shadow(game, player, EMPTY_CGP, "AIERZNL", &anchor_list);
  assert(anchor_list.count == 1);
  assert_anchor_equity_int(&anchor_list, 0, 102);

  load_and_shadow(game, player, EMPTY_CGP, "?", &anchor_list);
  assert(anchor_list.count == 1);
  assert_anchor_equity_int(&anchor_list, 0, 0);

  load_and_shadow(game, player, EMPTY_CGP, "??", &anchor_list);
  assert(anchor_list.count == 1);
  assert_anchor_equity_int(&anchor_list, 0, 0);

  load_and_shadow(game, player, EMPTY_CGP, "??OU", &anchor_list);
  assert(anchor_list.count == 1);
  assert_anchor_equity_int(&anchor_list, 0, 4);

  load_and_shadow(game, player, EMPTY_CGP, "??OUA", &anchor_list);
  assert(anchor_list.count == 1);
  assert_anchor_equity_int(&anchor_list, 0, 8);

  load_and_shadow(game, player, KA_OPENING_CGP, "EE", &anchor_list);
  assert(anchor_list.count == 6);

  // KAE and EE
  assert_anchor_equity_int(&anchor_list, 0, 10);
  // EKE
  assert_anchor_equity_int(&anchor_list, 1, 9);
  // KAEE
  assert_anchor_equity_int(&anchor_list, 2, 8);
  // EE and E(A)
  assert_anchor_equity_int(&anchor_list, 3, 5);
  // EE and E(A)
  assert_anchor_equity_int(&anchor_list, 4, 5);
  // EEE
  assert_anchor_equity_int(&anchor_list, 5, 3);
  // The rest are prevented by invalid cross sets

  load_and_shadow(game, player, KA_OPENING_CGP, "E?", &anchor_list);
  // 7G oE
  assert_anchor_equity_int(&anchor_list, 0, 8);
  // 9G aE
  assert_anchor_equity_int(&anchor_list, 1, 8);
  // I7 Ee
  assert_anchor_equity_int(&anchor_list, 2, 8);
  // F7 Es (only the blank can hook to the left with sKA or aKA or oKA)
  assert_anchor_equity_int(&anchor_list, 3, 7);
  // KAEe
  assert_anchor_equity_int(&anchor_list, 4, 7);
  // E(K)e
  assert_anchor_equity_int(&anchor_list, 5, 7);
  // Ea, EA
  assert_anchor_equity_int(&anchor_list, 6, 3);
  // Ae, eE
  assert_anchor_equity_int(&anchor_list, 7, 3);
  // E(A)a
  assert_anchor_equity_int(&anchor_list, 8, 2);

  load_and_shadow(game, player, KA_OPENING_CGP, "J", &anchor_list);
  assert(anchor_list.count == 4);
  // J(K) vertically
  assert_anchor_equity_int(&anchor_list, 0, 21);
  // J(KA) or (KA)J
  assert_anchor_equity_int(&anchor_list, 1, 14);
  // J(A) horitizontally
  assert_anchor_equity_int(&anchor_list, 2, 9);
  // J(A) vertically
  assert_anchor_equity_int(&anchor_list, 3, 9);

  load_and_shadow(game, player, AA_OPENING_CGP, "JF", &anchor_list);
  assert(anchor_list.count == 6);

  // JF, JA, and FA
  assert_anchor_equity_int(&anchor_list, 0, 42);
  // JA and JF or FA and FJ
  assert_anchor_equity_int(&anchor_list, 1, 25);
  // JAF with J and F doubled
  assert_anchor_equity_int(&anchor_list, 2, 25);
  // F7 JF (only F can hook AA)
  assert_anchor_equity_int(&anchor_list, 3, 18);
  // AAJF
  assert_anchor_equity_int(&anchor_list, 4, 14);
  // AJF
  assert_anchor_equity_int(&anchor_list, 5, 13);
  // Remaining anchors are prevented by invalid cross sets

  // Making JA, FA, and JFU, doubling the U on the double letter
  load_and_shadow(game, player, AA_OPENING_CGP, "JFU", &anchor_list);
  assert_anchor_equity_int(&anchor_list, 0, 44);

  // Making KAU (allowed by F in rack cross set)
  // and JUF, doubling the F and J.
  load_and_shadow(game, player, KA_OPENING_CGP, "JFU", &anchor_list);
  assert_anchor_equity_int(&anchor_list, 0, 32);

  load_and_shadow(game, player, AA_OPENING_CGP, "JFUG", &anchor_list);
  assert_anchor_equity_int(&anchor_list, 0, 47);

  load_and_shadow(game, player, AA_OPENING_CGP, "JFUGX", &anchor_list);
  assert_anchor_equity_int(&anchor_list, 0, 61);

  // Reaches the triple word
  load_and_shadow(game, player, AA_OPENING_CGP, "JFUGXL", &anchor_list);
  assert_anchor_equity_int(&anchor_list, 0, 102);

  load_and_shadow(game, player, DOUG_V_EMELY_CGP, "Q", &anchor_list);
  // WINDY is not extendable, so there is no 22 for WINDYQ.
  // The highest anchor is through the W in WINDY, recording 14 for QW, as W is
  // extendable with a Q to the left for Q(W)ERTY. Note there is no 22 for DQ
  // or 14 for YQ despite HINDQUARTER and TRIPTYQUE. Since no on the rack
  // extends D or Y to the left, the right_extension_set is applied, filtering
  // these anchors because no words _begin_ with DQ or YQ.
  assert_anchor_equity_int(&anchor_list, 0, 14);
  // The next highest anchor is through the I in WINDY: 11 for QI.
  assert_anchor_equity_int(&anchor_list, 1, 11);

  load_and_shadow(game, player, DOUG_V_EMELY_CGP, "BD", &anchor_list);
  // WINDY is not extendable, so there is no 17 for WINDYBD.
  // The highest anchor is 14 for 7H BD.
  assert_anchor_equity_int(&anchor_list, 0, 14);

  load_and_shadow(game, player, DOUG_V_EMELY_CGP, "BOH", &anchor_list);
  // WINDY is not extendable, so there is no 60 for BOHWINDY.
  // The highest anchor is 24 for 7G OBH.
  //   only B can hook Y for BY
  //   only O can hook D for OD
  assert_anchor_equity_int(&anchor_list, 0, 24);

  load_and_shadow(game, player, DOUG_V_EMELY_CGP, "BOHGX", &anchor_list);
  // WINDY is not extendable, so there is no 90 for BOHWINDYGX.
  // The highest anchor is 44 for D8 (W)BOHXG.
  //   using restricted hooks the highest horizontal plays are at 7A for 36.
  assert_anchor_equity_int(&anchor_list, 0, 44);

  load_and_shadow(game, player, DOUG_V_EMELY_CGP, "BOHGXZ", &anchor_list);
  // WINDY is not extendable, so there is no 120 for BOHWINDYGXZ.
  // The highest anchor is 116 for the 2x2 E5 BOH(I)GXZ.
  assert_anchor_equity_int(&anchor_list, 0, 116);

  load_and_shadow(game, player, DOUG_V_EMELY_CGP, "BOHGXZQ", &anchor_list);
  // WINDY is not extendable, so there is no 230 for BOHWINDYGXZQ.
  // The highest anchor is 206 for the 2x2 E5 BOH(I)GXZQ.
  assert_anchor_equity_int(&anchor_list, 0, 206);

  load_and_shadow(game, player, TRIPLE_LETTERS_CGP, "A", &anchor_list);

  // WINDY is not extendable, so there is no 13 for WINDYA.
  // H6 A(NY) vertical
  // H6 A(NY) horizontal
  // B6 A(P)
  // D6 A(OW)
  // F6 A(EN)

  assert_anchor_equity_int(&anchor_list, 0, 6);
  assert_anchor_equity_int(&anchor_list, 1, 6);
  assert_anchor_equity_int(&anchor_list, 1, 6);
  assert_anchor_equity_int(&anchor_list, 3, 6);
  assert_anchor_equity_int(&anchor_list, 4, 5);

  load_and_shadow(game, player, TRIPLE_LETTERS_CGP, "Z", &anchor_list);
  // Z(P) vertically
  assert_anchor_equity_int(&anchor_list, 0, 33);
  // Z(EN) vert
  // Z(EN) horiz
  assert_anchor_equity_int(&anchor_list, 1, 32);
  assert_anchor_equity_int(&anchor_list, 2, 32);
  // (AD)Z
  assert_anchor_equity_int(&anchor_list, 3, 23);

  load_and_shadow(game, player, TRIPLE_LETTERS_CGP, "ZLW", &anchor_list);
  // ZEN, ZW, WAD
  assert_anchor_equity_int(&anchor_list, 0, 73);
  // ZENLW
  assert_anchor_equity_int(&anchor_list, 1, 45);
  // ZLWOW
  assert_anchor_equity_int(&anchor_list, 2, 40);

  load_and_shadow(game, player, TRIPLE_LETTERS_CGP, "ZLW?", &anchor_list);
  // 6F ZWaL
  assert_anchor_equity_int(&anchor_list, 0, 79);

  load_and_shadow(game, player, TRIPLE_LETTERS_CGP, "QZLW", &anchor_list);
  // 6G ZQ making ZEN and QAD. Only L and W are in the AD cross set, but score
  // it using the Q. This is a limitation of tile restriction in shadow. L and W
  // give multiple possibilities for this square so we can't restrict it to a
  // single tile.
  assert_anchor_equity_int(&anchor_list, 0, 85);

  load_and_shadow(game, player, TRIPLE_DOUBLE_CGP, "K", &anchor_list);
  assert_anchor_equity_int(&anchor_list, 0, 13);

  load_and_shadow(game, player, TRIPLE_DOUBLE_CGP, "KT", &anchor_list);
  // KPAVT
  assert_anchor_equity_int(&anchor_list, 0, 20);

  load_and_shadow(game, player, TRIPLE_DOUBLE_CGP, "KT?", &anchor_list);
  // 10A TsPAVK. only the blank can left-extend PAV.
  assert_anchor_equity_int(&anchor_list, 0, 24);

  load_and_shadow(game, player, BOTTOM_LEFT_RE_CGP, "M", &anchor_list);
  assert_anchor_equity_int(&anchor_list, 0, 8);

  load_and_shadow(game, player, BOTTOM_LEFT_RE_CGP, "MN", &anchor_list);
  assert_anchor_equity_int(&anchor_list, 0, 16);

  load_and_shadow(game, player, BOTTOM_LEFT_RE_CGP, "MNA", &anchor_list);
  assert_anchor_equity_int(&anchor_list, 0, 20);

  load_and_shadow(game, player, BOTTOM_LEFT_RE_CGP, "MNAU", &anchor_list);
  assert_anchor_equity_int(&anchor_list, 0, 22);

  load_and_shadow(game, player, BOTTOM_LEFT_RE_CGP, "MNAUT", &anchor_list);
  assert_anchor_equity_int(&anchor_list, 0, 30);

  load_and_shadow(game, player, BOTTOM_LEFT_RE_CGP, "MNAUTE", &anchor_list);
  assert_anchor_equity_int(&anchor_list, 0, 39);

  load_and_shadow(game, player, LATER_BETWEEN_DOUBLE_WORDS_CGP, "Z",
                  &anchor_list);
  // Z(T)
  assert_anchor_equity_int(&anchor_list, 0, 21);
  // (A)Z
  assert_anchor_equity_int(&anchor_list, 1, 11);
  // Z(E)
  assert_anchor_equity_int(&anchor_list, 2, 11);
  // Z(A)
  assert_anchor_equity_int(&anchor_list, 3, 11);

  load_and_shadow(game, player, LATER_BETWEEN_DOUBLE_WORDS_CGP, "ZL",
                  &anchor_list);
  assert_anchor_equity_int(&anchor_list, 0, 64);

  load_and_shadow(game, player, LATER_BETWEEN_DOUBLE_WORDS_CGP, "ZLI",
                  &anchor_list);
  assert_anchor_equity_int(&anchor_list, 0, 68);

  load_and_shadow(game, player, LATER_BETWEEN_DOUBLE_WORDS_CGP, "ZLIE",
                  &anchor_list);
  assert_anchor_equity_int(&anchor_list, 0, 72);

  load_and_shadow(game, player, LATER_BETWEEN_DOUBLE_WORDS_CGP, "ZLIER",
                  &anchor_list);
  // 5D ZE(LATER)RIL (not 6F ZLERI: there's no TLS Z hotspots)
  assert_anchor_equity_int(&anchor_list, 0, 76);

  load_and_shadow(game, player, LATER_BETWEEN_DOUBLE_WORDS_CGP, "ZLIERA",
                  &anchor_list);
  assert_anchor_equity_int(&anchor_list, 0, 80);

  load_and_shadow(game, player, LATER_BETWEEN_DOUBLE_WORDS_CGP, "ZL",
                  &anchor_list);
  assert_anchor_equity_int(&anchor_list, 0, 64);

  load_and_shadow(game, player, LATER_BETWEEN_DOUBLE_WORDS_CGP, "ZLI",
                  &anchor_list);
  assert_anchor_equity_int(&anchor_list, 0, 68);

  load_and_shadow(game, player, LATER_BETWEEN_DOUBLE_WORDS_CGP, "ZLIE",
                  &anchor_list);
  assert_anchor_equity_int(&anchor_list, 0, 72);

  load_and_shadow(game, player, LATER_BETWEEN_DOUBLE_WORDS_CGP, "ZLIER",
                  &anchor_list);
  // 5D ZE(LATER)RIL (not 6F ZLERI: there's no TLS Z hotspots)
  assert_anchor_equity_int(&anchor_list, 0, 76);

  load_and_shadow(game, player, LATER_BETWEEN_DOUBLE_WORDS_CGP, "ZLIERA",
                  &anchor_list);
  assert_anchor_equity_int(&anchor_list, 0, 80);

  load_and_shadow(game, player, LATER_BETWEEN_DOUBLE_WORDS_CGP, "ZLIERAI",
                  &anchor_list);
  assert_anchor_equity_int(&anchor_list, 0, 212);

  load_and_shadow(game, player, VS_OXY, "A", &anchor_list);
  // No APACIFYING: PACIFYING not extendable with an A.
  // A(WAKEnERS)/(UT)A
  assert_anchor_equity_int(&anchor_list, 0, 18);

  load_and_shadow(game, player, VS_OXY, "O", &anchor_list);
  // O(PACIFYING)
  assert_anchor_equity_int(&anchor_list, 0, 63);

  load_and_shadow(game, player, VS_OXY, "E", &anchor_list);
  // E(JACULATING)/(ON)E > E(PACIFYING)
  assert_anchor_equity_int(&anchor_list, 0, 72);

  load_and_shadow(game, player, VS_OXY, "PB", &anchor_list);
  assert_anchor_equity_int(&anchor_list, 0, 156);

  load_and_shadow(game, player, VS_OXY, "PA", &anchor_list);
  // No B6 DORMPWOOAJ for 76. Only the A fits in the cross sets of T and N,
  // and it can't be in both of those squares.
  // Best is A3 (Y)P(HEN) hooking P(REQUALIFIED) for 50
  assert_anchor_equity_int(&anchor_list, 0, 50);

  load_and_shadow(game, player, VS_OXY, "PBA", &anchor_list);
  assert_anchor_equity_int(&anchor_list, 0, 174);

  load_and_shadow(game, player, VS_OXY, "Z", &anchor_list);
  // No ZPACIFYING
  // TZ/ZA
  assert_anchor_equity_int(&anchor_list, 0, 42);

  load_and_shadow(game, player, VS_OXY, "ZE", &anchor_list);
  // ZONE
  assert_anchor_equity_int(&anchor_list, 0, 160);

  load_and_shadow(game, player, VS_OXY, "AZE", &anchor_list);
  // UTAZONE
  assert_anchor_equity_int(&anchor_list, 0, 184);

  load_and_shadow(game, player, VS_OXY, "AZEB", &anchor_list);
  // HENBUTAZONE
  assert_anchor_equity_int(&anchor_list, 0, 484);

  load_and_shadow(game, player, VS_OXY, "AZEBP", &anchor_list);
  // YPHENBUTAZONE
  assert_anchor_equity_int(&anchor_list, 0, 604);

  load_and_shadow(game, player, VS_OXY, "AZEBPX", &anchor_list);
  // No A2 A(Y)X(HEN)P(UT)EZ(ON)B for 740
  //  only P hooks REQUALIFIED
  //  only B hooks RAINWASHING
  //  only A hooks WAKENERS
  //  only Z hooks ONETIME
  //  only E hooks JACULATING
  // Best is A2 X(Y)P(HEN)B(UT)AZ(ON)E for 686
  assert_anchor_equity_int(&anchor_list, 0, 686);

  load_and_shadow(game, player, VS_OXY, "AZEBPXO", &anchor_list);
  // No A1 OA(Y)X(HEN)P(UT)EZ(ON)B for 1924
  //  only O hooks PACIFYING
  //  only P hooks REQUALIFIED
  //  only B hooks RAINWASHING
  //  only A hooks WAKENERS
  //  only Z hooks ONETIME
  //  only E hooks JACULATING
  // Best is A1 OX(Y)P(HEN)B(UT)AZ(ON)E for 1780
  assert_anchor_equity_int(&anchor_list, 0, 1780);

  load_and_shadow(game, player, VS_OXY, "AZEBPQO", &anchor_list);
  // A1 OQ(Y)P(HEN)B(UT)AZ(ON)E
  assert_anchor_equity_int(&anchor_list, 0, 1836);

  game_reset(game);
  const char qi_qis[300] =
      "15/15/15/15/15/15/15/6QI7/6I8/6S8/15/15/15/15/15 FRUITED/EGGCUPS 22/12 "
      "0 lex CSW21";
  load_and_shadow(game, player, qi_qis, "FRUITED", &anchor_list);
  // 8g (QI)DURFITE 128
  // h8 (I)DURFITE 103
  // f9 UFTRIDE 88
  // 7g EF(QIS)RTUDI 79
  // 10b FURED(S)IT 74
  // 9c DURT(I)FIE 70
  // 7h FURTIDE 69
  // h10 DUFTIE 45
  // f10 FURIDE 35
  assert(anchor_list.count == 8);
  assert_anchor_equity_int(&anchor_list, 0, 128);
  assert(anchor_list.anchors[0].row == 7);
  assert(anchor_list.anchors[0].col == 7);
  assert(anchor_list.anchors[0].dir == BOARD_HORIZONTAL_DIRECTION);

  assert_anchor_equity_int(&anchor_list, 1, 103);
  assert(anchor_list.anchors[1].row == 7);
  assert(anchor_list.anchors[1].col == 7);
  assert(anchor_list.anchors[1].dir == BOARD_VERTICAL_DIRECTION);

  // f9 UFTRIDE for 88, not h9 DURFITE for 100 (which does not fit)
  assert_anchor_equity_int(&anchor_list, 2, 88);
  assert(anchor_list.anchors[2].row == 5);
  assert(anchor_list.anchors[2].col == 8);
  assert(anchor_list.anchors[2].dir == BOARD_VERTICAL_DIRECTION);

  game_reset(game);
  const char shuttled_ravioli[300] =
      "14Z/5V7NA/5R4P2AN/5O1N1ARGUFY/5TOO2I2F1/4T1COWKS2E1/2REE1UN2A2R1/"
      "1RAVIoLI2G3Q/1EXON1IN2E1P1A/1C1L3GEM2AHI/BEMUD2SHUTTlED/1D1E8AI1/"
      "YET9IS1/ODA9ST1/W1JOLLER7 BII/EO 477/388 0 lex CSW21";
  load_and_shadow(game, player, shuttled_ravioli, "BII", &anchor_list);

  // M3 B(U)I 25
  assert_anchor_equity_int(&anchor_list, 0, 25);

  // 7J I(A)IB(R) 16
  assert_anchor_equity_int(&anchor_list, 1, 16);

  // 9B (EXON)I(IN) 15
  assert_anchor_equity_int(&anchor_list, 2, 15);

  // 4H (N)I(ARGUFY) 15
  assert_anchor_equity_int(&anchor_list, 3, 15);

  // C4 BII(RAX) 15
  assert_anchor_equity_int(&anchor_list, 3, 15);

  // J10 (MU)IIB 15
  assert_anchor_equity_int(&anchor_list, 4, 15);

  // 12K II(AI) 14
  assert_anchor_equity_int(&anchor_list, 5, 14);

  game_reset(game);
  const char toeless[300] =
      "15/15/15/15/15/15/5Q2J6/5UVAE6/5I2U6/5Z9/15/15/15/15/15 TOELESS/EEGIPRW "
      "42/38 0 lex CSW21";
  load_and_shadow(game, player, toeless, "TOELESS", &anchor_list);

  assert_anchor_equity_int(&anchor_list, 0, 86);

  game_reset(game);
  const char addle[300] =
      "5E1p7/1A1C1G1E5S1/1JIZ1G1R3V1P1/HO1APE1A3U1I1/OW1RAD1EXFIL1T1/"
      "MA2W2OI2G1T1/IN2K2N2MOBE1/E2FYRDS2o1ANS/3O6V1N1H/3U6ADD1E/3S6BOO1E/"
      "3T6LOR1L/INCITANT1AYRE2/3E5U5/3REQUITE5 L/I 467/473 0 lex CSW21";
  load_and_shadow(game, player, addle, "L", &anchor_list);

  // 15D (REQUITE)L
  assert_anchor_equity_int(&anchor_list, 0, 17);

  // 5H (EXFIL)L(T)
  assert_anchor_equity_int(&anchor_list, 1, 17);

  // 3L (V)I(P)
  assert_anchor_equity_int(&anchor_list, 2, 16);

  // F13 (A)L(Q)
  assert_anchor_equity_int(&anchor_list, 3, 14);

  // 5A (OW)L(RAD)
  assert_anchor_equity_int(&anchor_list, 4, 10);

  // 10K (ADD)L(E)
  assert_anchor_equity_int(&anchor_list, 5, 9);

  game_reset(game);
  const char magpies[300] =
      "15/15/15/15/15/15/15/MAGPIE2LUMBAGO/15/15/15/15/15/15/15 SS/Q "
      "0/0 0 lex CSW21";
  load_and_shadow(game, player, magpies, "SS", &anchor_list);

  // G7 SS hooking MAGPIES for 15, not (MAGPIES)SS(LUMBAGO) for 50.
  // LUMBAGO is not left-extendable with an S.
  assert_anchor_equity_int(&anchor_list, 0, 15);

  game_destroy(game);
  config_destroy(config);
}

void test_shadow_wmp_nonplaythrough_existence(void) {
  Config *config = config_create_or_die("set -lex CSW21 -wmp true");
  Game *game = config_game_create(config);
  Player *player = game_get_player(game, 0);
  player_set_move_sort_type(player, MOVE_SORT_SCORE);

  AnchorHeap anchor_list;
  load_and_shadow(game, player, EMPTY_CGP, "MUZJIKS", &anchor_list);
  assert(anchor_list.count == 1);
  assert_anchor_equity_int(&anchor_list, 0, 128);

  load_and_shadow(game, player, EMPTY_CGP, "TRONGLE", &anchor_list);
  assert(anchor_list.count == 1);
  // We know there are sixes with a G, and assume something could put the G on
  // the DWS even though none do.
  assert_anchor_equity_int(&anchor_list, 0, 18);

  load_and_shadow(game, player, EMPTY_CGP, "VVWWXYZ", &anchor_list);
  assert(anchor_list.count == 1);
  // This is an unusual case. The 0 recorded here is as if for a one tile play
  // in the vertical direction. We shadow these as if playing horizontally and
  // do not check for word validity. It scores 0 for main word because the score
  // would actually come from the vertical direction (as a hook). This doesn't
  // make sense with an empty board but might not be worth special handling.
  assert_anchor_equity_int(&anchor_list, 0, 0);

  game_destroy(game);
  config_destroy(config);
}

void test_shadow_wmp_playthrough_bingo_existence(void) {
  Config *config = config_create_or_die("set -lex CSW21 -wmp true");
  Game *game = config_game_create(config);
  Player *player = game_get_player(game, 0);

  player_set_move_sort_type(player, MOVE_SORT_SCORE);
  const char qi_qis[300] =
      "15/15/15/15/15/15/15/6QI7/6I8/6S8/15/15/15/15/15 FRUITED/EGGCUPS 22/12 "
      "0 lex CSW21";

  AnchorHeap anchor_list;
  load_and_shadow(game, player, qi_qis, "FRUITED", &anchor_list);
  assert(anchor_list.count == 8);

  // f9 UFTRIDE for 88, not 8g (QI)DURFITE for 128
  assert_anchor_equity_int(&anchor_list, 0, 88);
  assert(anchor_list.anchors[0].row == 5);
  assert(anchor_list.anchors[0].col == 8);
  assert(anchor_list.anchors[0].dir == BOARD_VERTICAL_DIRECTION);

  load_and_shadow(game, player, qi_qis, "AOUNS??", &anchor_list);
  assert(anchor_list.count == 9);

  // 8g (QI)NghAOSU
  assert_anchor_equity_int(&anchor_list, 0, 101);
  assert(anchor_list.anchors[0].row == 7);
  assert(anchor_list.anchors[0].col == 7);
  assert(anchor_list.anchors[0].dir == BOARD_HORIZONTAL_DIRECTION);

  game_destroy(game);
  config_destroy(config);
}

void test_shadow_top_move(void) {
  Config *config = config_create_or_die(
      "set -lex CSW21 -s1 equity -s2 equity -r1 all -r2 all -numplays 1");
  Game *game = config_game_create(config);
  Player *player = game_get_player(game, 0);
  MoveList *move_list = move_list_create(1);

  player_set_move_sort_type(player, MOVE_SORT_EQUITY);
  player_set_move_record_type(player, MOVE_RECORD_BEST);

  // Top play should be L1 Q(I)
  load_and_generate_moves(game, move_list, player, UEY_CGP, "ACEQOOV");
  assert_move_score(move_list_get_move(move_list, 0), 21);

  move_list_destroy(move_list);
  game_destroy(game);
  config_destroy(config);
}

void test_shadow(void) {
  test_shadow_score();
  test_shadow_wmp_nonplaythrough_existence();
  test_shadow_wmp_playthrough_bingo_existence();
  test_shadow_top_move();
}