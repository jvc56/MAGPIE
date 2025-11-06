#include "../src/def/board_defs.h"
#include "../src/def/equity_defs.h"
#include "../src/def/move_defs.h"
#include "../src/ent/anchor.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"
#include "test_constants.h"
#include "test_util.h"
#include <assert.h>

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
      .eq_margin_movegen = 0,
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
  assert(anchor_list.count == 0);

  load_and_shadow(game, player, EMPTY_CGP, "QK", &anchor_list);
  assert(anchor_list.count == 0);

  load_and_shadow(game, player, EMPTY_CGP, "AESR", &anchor_list);
  assert(anchor_list.count == 3);
  assert_anchor_equity_int(&anchor_list, 0, 8);

  load_and_shadow(game, player, EMPTY_CGP, "TNCL", &anchor_list);
  assert(anchor_list.count == 0);

  load_and_shadow(game, player, EMPTY_CGP, "AAAAA", &anchor_list);
  assert(anchor_list.count == 1);
  assert_anchor_equity_int(&anchor_list, 0, 4);

  load_and_shadow(game, player, EMPTY_CGP, "CAAAA", &anchor_list);
  assert(anchor_list.count == 2);
  assert_anchor_equity_int(&anchor_list, 0, 10);

  load_and_shadow(game, player, EMPTY_CGP, "CAKAA", &anchor_list);
  assert(anchor_list.count == 2);
  // ACK is not in CSW21, but shadow_record does not improve score bounds
  // using subrack info, only leaves
  assert_anchor_equity_int(&anchor_list, 0, 18);

  load_and_shadow(game, player, EMPTY_CGP, "AIERZ", &anchor_list);
  assert(anchor_list.count == 4);
  assert_anchor_equity_int(&anchor_list, 0, 48);

  load_and_shadow(game, player, EMPTY_CGP, "AIERZN", &anchor_list);
  assert(anchor_list.count == 5);
  assert_anchor_equity_int(&anchor_list, 0, 50);

  load_and_shadow(game, player, EMPTY_CGP, "AIERZNL", &anchor_list);
  assert(anchor_list.count == 5);
  assert_anchor_equity_int(&anchor_list, 0, 50);

  load_and_shadow(game, player, EMPTY_CGP, "?", &anchor_list);
  assert(anchor_list.count == 0);

  load_and_shadow(game, player, EMPTY_CGP, "??", &anchor_list);
  assert(anchor_list.count == 1);
  assert_anchor_equity_int(&anchor_list, 0, 0);

  load_and_shadow(game, player, EMPTY_CGP, "??OU", &anchor_list);
  assert(anchor_list.count == 3);
  assert_anchor_equity_int(&anchor_list, 0, 4);

  load_and_shadow(game, player, EMPTY_CGP, "??OUA", &anchor_list);
  assert(anchor_list.count == 4);
  assert_anchor_equity_int(&anchor_list, 0, 8);

  load_and_shadow(game, player, KA_OPENING_CGP, "EE", &anchor_list);
  assert(anchor_list.count == 8);

  // (KA)E and EE
  assert_anchor_equity_int(&anchor_list, 0, 10);
  // E(K)E
  assert_anchor_equity_int(&anchor_list, 1, 9);
  // (KA)EE
  assert_anchor_equity_int(&anchor_list, 2, 8);
  // (KA)E
  assert_anchor_equity_int(&anchor_list, 3, 7);
  // (K)E (playthrough subrack not checked)
  assert_anchor_equity_int(&anchor_list, 4, 7);
  // EE and E(A)
  assert_anchor_equity_int(&anchor_list, 5, 5);
  // EE and (A)E
  assert_anchor_equity_int(&anchor_list, 6, 5);
  // (A)E
  assert_anchor_equity_int(&anchor_list, 7, 2);
  // The rest are prevented by invalid cross sets

  load_and_shadow(game, player, KA_OPENING_CGP, "E?", &anchor_list);
  assert(anchor_list.count == 12);
  // 7G oE
  assert_anchor_equity_int(&anchor_list, 0, 8);
  // 9G aE
  assert_anchor_equity_int(&anchor_list, 1, 8);
  // I7 Ee
  assert_anchor_equity_int(&anchor_list, 2, 8);
  // 8H (KA)Ee
  assert_anchor_equity_int(&anchor_list, 3, 7);
  // G7 E(K) (nonplaythrough subrack not checked)
  assert_anchor_equity_int(&anchor_list, 4, 7);
  // G7 E(K)e
  assert_anchor_equity_int(&anchor_list, 5, 7);
  // F7 Es (only the blank can hook to the left with sKA or aKA or oKA)
  assert_anchor_equity_int(&anchor_list, 6, 7);
  // 8H (KA)E
  assert_anchor_equity_int(&anchor_list, 7, 7);
  // 7H Ee
  assert_anchor_equity_int(&anchor_list, 8, 3);
  // 9H Ee
  assert_anchor_equity_int(&anchor_list, 9, 3);
  // H8 (A)xE
  assert_anchor_equity_int(&anchor_list, 10, 2);
  // H8 (A)E
  assert_anchor_equity_int(&anchor_list, 11, 2);

  load_and_shadow(game, player, KA_OPENING_CGP, "J", &anchor_list);
  assert(anchor_list.count == 2);
  // 8H (KA)J 
  assert_anchor_equity_int(&anchor_list, 0, 14);
  // H7 J(A) 
  assert_anchor_equity_int(&anchor_list, 1, 9);

  load_and_shadow(game, player, AA_OPENING_CGP, "JF", &anchor_list);
  assert(anchor_list.count == 4);
  // G7 J(A)
  assert_anchor_equity_int(&anchor_list, 0, 17);
  // 8H (AA)FJ
  assert_anchor_equity_int(&anchor_list, 1, 14);
  // 9H (AA)J (playthrough subrack not checked)
  assert_anchor_equity_int(&anchor_list, 2, 10);
  // H7 J(A)
  assert_anchor_equity_int(&anchor_list, 3, 9);

  load_and_shadow(game, player, AA_OPENING_CGP, "JFU", &anchor_list);
  // H7 F(A)J
  assert_anchor_equity_int(&anchor_list, 0, 25);

  load_and_shadow(game, player, KA_OPENING_CGP, "JFU", &anchor_list);
  // H7 F(K)J
  assert_anchor_equity_int(&anchor_list, 0, 29);

  load_and_shadow(game, player, AA_OPENING_CGP, "JFUG", &anchor_list);
  // 7G JFG (3 letter nonplaythrough allowed because of JUG/GJU)
  assert_anchor_equity_int(&anchor_list, 0, 46);

  load_and_shadow(game, player, AA_OPENING_CGP, "JFUGX", &anchor_list);
  // 7G JXF (3 letter nonplaythrough allowed because of JUG/GJU)
  assert_anchor_equity_int(&anchor_list, 0, 58);

  load_and_shadow(game, player, AA_OPENING_CGP, "JFUGXL", &anchor_list);
  // 7G JXFG (4 letter nonplaythrough allowed because of GULF)
  assert_anchor_equity_int(&anchor_list, 0, 60);

  load_and_shadow(game, player, DOUG_V_EMELY_CGP, "Q", &anchor_list);
  // E7 Q(I)
  assert_anchor_equity_int(&anchor_list, 0, 11);

  load_and_shadow(game, player, DOUG_V_EMELY_CGP, "BD", &anchor_list);
  // (WINDY)BD is checked (full rack subrack)
  // WINDY is not extendable, so there is no 15 for WINDYB.
  // G7 B(D) (playthrough subrack not checked)
  assert_anchor_equity_int(&anchor_list, 0, 8);

  load_and_shadow(game, player, DOUG_V_EMELY_CGP, "BOH", &anchor_list);
  // The highest anchor is 24 for 7G OBH.
  //   only B can hook Y for BY
  //   only O can hook D for OD
  assert_anchor_equity_int(&anchor_list, 0, 24);

  load_and_shadow(game, player, DOUG_V_EMELY_CGP, "BOHGX", &anchor_list);
  // WINDY is not extendable, so there are no plays to the TWS.
  // 44 for D8 (W)BOHXG not allowed because we check full rack
  // Best is 42 for D8 (W)BHGX
  assert_anchor_equity_int(&anchor_list, 0, 42);

  load_and_shadow(game, player, DOUG_V_EMELY_CGP, "BOHGXZ", &anchor_list);
  // WINDY is not extendable, no plays to the TWS.
  // No 116 for the 2x2 E5 BOH(I)GXZ, we check full rack.
  // Best is 64 for F6 XB(N)GZH
  assert_anchor_equity_int(&anchor_list, 0, 64);

  load_and_shadow(game, player, DOUG_V_EMELY_CGP, "BOHGXZQ", &anchor_list);
  // WINDY is not extendable, no plays to the TWS
  // Full rack is checked (even with playthrough) so no bingo.
  // Best is 152 for 2x2 E5 BGH(I)QXZ
  assert_anchor_equity_int(&anchor_list, 0, 152);

  load_and_shadow(game, player, TRIPLE_LETTERS_CGP, "A", &anchor_list);
  // B6 A(P)
  // H6 A(NY)
  // F6 A(EN)
  // E5 A(TI)
  // C6 A(R)
  assert_anchor_equity_int(&anchor_list, 0, 6);
  assert_anchor_equity_int(&anchor_list, 1, 6);
  assert_anchor_equity_int(&anchor_list, 2, 5);
  assert_anchor_equity_int(&anchor_list, 3, 3);
  assert_anchor_equity_int(&anchor_list, 4, 2);

  load_and_shadow(game, player, TRIPLE_LETTERS_CGP, "Z", &anchor_list);
  // Z(EN)
  assert_anchor_equity_int(&anchor_list, 0, 32);
  // (AD)Z
  assert_anchor_equity_int(&anchor_list, 1, 23);

  load_and_shadow(game, player, TRIPLE_LETTERS_CGP, "ZLW", &anchor_list);
  // B6 Z(P)W
  assert_anchor_equity_int(&anchor_list, 0, 37);
  // F6 Z(EN)W
  assert_anchor_equity_int(&anchor_list, 1, 36);
  // B6 Z(P)
  assert_anchor_equity_int(&anchor_list, 2, 33);

  load_and_shadow(game, player, TRIPLE_LETTERS_CGP, "ZLW?", &anchor_list);
  // 6F ZWa
  assert_anchor_equity_int(&anchor_list, 0, 78);

  load_and_shadow(game, player, TRIPLE_LETTERS_CGP, "QZLW", &anchor_list);
  // F6 Z(EN)WQ
  assert_anchor_equity_int(&anchor_list, 0, 66);

  load_and_shadow(game, player, TRIPLE_DOUBLE_CGP, "K", &anchor_list);
  // D9 K(A)
  assert_anchor_equity_int(&anchor_list, 0, 6);

  load_and_shadow(game, player, TRIPLE_DOUBLE_CGP, "KT", &anchor_list);
  // D10 (A)KT
  assert_anchor_equity_int(&anchor_list, 0, 14);

  load_and_shadow(game, player, TRIPLE_DOUBLE_CGP, "KT?", &anchor_list);
  // Only the blank can left-extend PAV.
  // Full rack makes no word through PAV.
  // 10B sPAVK
  assert_anchor_equity_int(&anchor_list, 0, 23);

  load_and_shadow(game, player, BOTTOM_LEFT_RE_CGP, "M", &anchor_list);
  assert_anchor_equity_int(&anchor_list, 0, 8);

  load_and_shadow(game, player, BOTTOM_LEFT_RE_CGP, "MN", &anchor_list);
  assert_anchor_equity_int(&anchor_list, 0, 10);

  load_and_shadow(game, player, BOTTOM_LEFT_RE_CGP, "MNA", &anchor_list);
  assert_anchor_equity_int(&anchor_list, 0, 20);

  load_and_shadow(game, player, BOTTOM_LEFT_RE_CGP, "MNAU", &anchor_list);
  assert_anchor_equity_int(&anchor_list, 0, 22);

  load_and_shadow(game, player, BOTTOM_LEFT_RE_CGP, "MNAUT", &anchor_list);
  assert_anchor_equity_int(&anchor_list, 0, 28);

  load_and_shadow(game, player, BOTTOM_LEFT_RE_CGP, "MNAUTE", &anchor_list);
  assert_anchor_equity_int(&anchor_list, 0, 39);

  load_and_shadow(game, player, LATER_BETWEEN_DOUBLE_WORDS_CGP, "Z",
                  &anchor_list);
  // (A)Z
  assert_anchor_equity_int(&anchor_list, 0, 11);
  // Z(E)
  assert_anchor_equity_int(&anchor_list, 1, 11);

  load_and_shadow(game, player, LATER_BETWEEN_DOUBLE_WORDS_CGP, "ZL",
                  &anchor_list);
  assert_anchor_equity_int(&anchor_list, 0, 22);

  load_and_shadow(game, player, LATER_BETWEEN_DOUBLE_WORDS_CGP, "ZLI",
                  &anchor_list);
  assert_anchor_equity_int(&anchor_list, 0, 64);

  load_and_shadow(game, player, LATER_BETWEEN_DOUBLE_WORDS_CGP, "ZLIE",
                  &anchor_list);
  assert_anchor_equity_int(&anchor_list, 0, 68);

  load_and_shadow(game, player, LATER_BETWEEN_DOUBLE_WORDS_CGP, "ZLIER",
                  &anchor_list);
  // 5D ZE(LATER)RI (not 6F ZLERI: there's no TLS Z hotspots)
  assert_anchor_equity_int(&anchor_list, 0, 72);

  load_and_shadow(game, player, LATER_BETWEEN_DOUBLE_WORDS_CGP, "ZLIERA",
                  &anchor_list);
  assert_anchor_equity_int(&anchor_list, 0, 78);

  load_and_shadow(game, player, LATER_BETWEEN_DOUBLE_WORDS_CGP, "ZLIERAI",
                  &anchor_list);
  // No 2x2 bingos. This is 4F AIZELIR, we can't restrict the Z out of the DLS
  assert_anchor_equity_int(&anchor_list, 0, 131);

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
  assert_anchor_equity_int(&anchor_list, 0, 54);

  load_and_shadow(game, player, VS_OXY, "PA", &anchor_list);
  assert_anchor_equity_int(&anchor_list, 0, 18);

  load_and_shadow(game, player, VS_OXY, "PBA", &anchor_list);
  assert_anchor_equity_int(&anchor_list, 0, 156);

  load_and_shadow(game, player, VS_OXY, "Z", &anchor_list);
  assert_anchor_equity_int(&anchor_list, 0, 22);

  load_and_shadow(game, player, VS_OXY, "ZE", &anchor_list);
  // ZONE
  assert_anchor_equity_int(&anchor_list, 0, 160);

  load_and_shadow(game, player, VS_OXY, "AZE", &anchor_list);
  assert_anchor_equity_int(&anchor_list, 0, 160);

  load_and_shadow(game, player, VS_OXY, "AZEB", &anchor_list);
  assert_anchor_equity_int(&anchor_list, 0, 208);

  load_and_shadow(game, player, VS_OXY, "AZEBP", &anchor_list);
  assert_anchor_equity_int(&anchor_list, 0, 484);

  load_and_shadow(game, player, VS_OXY, "AZEBPX", &anchor_list);
  assert_anchor_equity_int(&anchor_list, 0, 604);

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
  assert_anchor_equity_int(&anchor_list, 0, 706);

  game_reset(game);
  const char qi_qis[300] =
      "15/15/15/15/15/15/15/6QI7/6I8/6S8/15/15/15/15/15 FRUITED/EGGCUPS 22/12 "
      "0 lex CSW21";
  load_and_shadow(game, player, qi_qis, "FRUITED", &anchor_list);
  assert_anchor_equity_int(&anchor_list, 0, 88);
  assert_anchor_equity_int(&anchor_list, 1, 75);
  assert_anchor_equity_int(&anchor_list, 2, 69);

  game_reset(game);
  const char shuttled_ravioli[300] =
      "14Z/5V7NA/5R4P2AN/5O1N1ARGUFY/5TOO2I2F1/4T1COWKS2E1/2REE1UN2A2R1/"
      "1RAVIoLI2G3Q/1EXON1IN2E1P1A/1C1L3GEM2AHI/BEMUD2SHUTTlED/1D1E8AI1/"
      "YET9IS1/ODA9ST1/W1JOLLER7 BII/EO 477/388 0 lex CSW21";
  load_and_shadow(game, player, shuttled_ravioli, "BII", &anchor_list);

  // M3 B(U)I 25
  assert_anchor_equity_int(&anchor_list, 0, 25);

  // 9B (EXON)I(IN) 15
  assert_anchor_equity_int(&anchor_list, 1, 15);

  // 12K II(AI) 14
  assert_anchor_equity_int(&anchor_list, 2, 14);

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

  // 10K (ADD)L(E)
  assert_anchor_equity_int(&anchor_list, 0, 9);

  game_reset(game);
  const char magpies[300] =
      "15/15/15/15/15/15/15/MAGPIE2LUMBAGO/15/15/15/15/15/15/15 SS/Q "
      "0/0 0 lex CSW21";
  load_and_shadow(game, player, magpies, "SS", &anchor_list);

  // MAGPIES for 12, not (MAGPIES)SS(LUMBAGO) for 50.
  // SS doesn't make a word so we can only play one tile, not (MAGPIE)S/SS
  assert_anchor_equity_int(&anchor_list, 0, 12);

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

  // No one tile anchor. The six anchors are for engths 2 to 7.
  assert(anchor_list.count == 6);

  // 7 tiles: MUZJIKS
  assert_anchor_equity_int(&anchor_list, 0, 128);
  assert_anchor_score(&anchor_list, 0, 128);
  assert(anchor_list.anchors[0].tiles_to_play == 7);
  assert(anchor_list.anchors[0].row == 7);
  assert(anchor_list.anchors[0].col == 7);
  assert(anchor_list.anchors[0].dir == BOARD_HORIZONTAL_DIRECTION);
  assert(anchor_list.anchors[0].playthrough_blocks == 0);

  // 6 tiles, ZJKMIS
  assert_anchor_equity_int(&anchor_list, 1, 76);
  assert_anchor_score(&anchor_list, 1, 76);
  assert(anchor_list.anchors[1].tiles_to_play == 6);
  assert(anchor_list.anchors[1].row == 7);
  assert(anchor_list.anchors[1].col == 7);
  assert(anchor_list.anchors[1].dir == BOARD_HORIZONTAL_DIRECTION);
  assert(anchor_list.anchors[1].playthrough_blocks == 0);

  // 5 tiles, ZJKMI
  assert_anchor_equity_int(&anchor_list, 2, 74);
  assert_anchor_score(&anchor_list, 2, 74);
  assert(anchor_list.anchors[2].tiles_to_play == 5);
  assert(anchor_list.anchors[2].row == 7);
  assert(anchor_list.anchors[2].col == 7);
  assert(anchor_list.anchors[2].dir == BOARD_HORIZONTAL_DIRECTION);
  assert(anchor_list.anchors[2].playthrough_blocks == 0);

  // 4 tiles, ZJKM
  assert_anchor_equity_int(&anchor_list, 3, 52);
  assert_anchor_score(&anchor_list, 3, 52);
  assert(anchor_list.anchors[3].tiles_to_play == 4);
  assert(anchor_list.anchors[3].row == 7);
  assert(anchor_list.anchors[3].col == 7);
  assert(anchor_list.anchors[3].dir == BOARD_HORIZONTAL_DIRECTION);
  assert(anchor_list.anchors[3].playthrough_blocks == 0);

  // 3 tiles, ZJK
  assert_anchor_equity_int(&anchor_list, 4, 46);
  assert_anchor_score(&anchor_list, 4, 46);
  assert(anchor_list.anchors[4].tiles_to_play == 3);
  assert(anchor_list.anchors[4].row == 7);
  assert(anchor_list.anchors[4].col == 7);
  assert(anchor_list.anchors[4].dir == BOARD_HORIZONTAL_DIRECTION);
  assert(anchor_list.anchors[4].playthrough_blocks == 0);

  // 2 tiles, ZJ
  assert_anchor_equity_int(&anchor_list, 5, 36);
  assert_anchor_score(&anchor_list, 5, 36);
  assert(anchor_list.anchors[5].tiles_to_play == 2);
  assert(anchor_list.anchors[5].row == 7);
  assert(anchor_list.anchors[5].col == 7);
  assert(anchor_list.anchors[5].dir == BOARD_HORIZONTAL_DIRECTION);
  assert(anchor_list.anchors[5].playthrough_blocks == 0);

  load_and_shadow(game, player, EMPTY_CGP, "TRONGLE", &anchor_list);

  // There are no sevens. We check full rack existence to avoid creating the
  // bingo anchor. We do not create a 1-tile anchor.
  assert(anchor_list.count == 5);

  // 6 tiles, Gxxxxx
  assert_anchor_equity_int(&anchor_list, 0, 18);
  assert_anchor_score(&anchor_list, 0, 18);
  assert(anchor_list.anchors[0].tiles_to_play == 6);
  assert(anchor_list.anchors[0].row == 7);
  assert(anchor_list.anchors[0].col == 7);
  assert(anchor_list.anchors[0].dir == BOARD_HORIZONTAL_DIRECTION);
  assert(anchor_list.anchors[0].playthrough_blocks == 0);

  // 5 tiles, Gxxxx
  assert_anchor_equity_int(&anchor_list, 1, 16);
  assert_anchor_score(&anchor_list, 1, 16);
  assert(anchor_list.anchors[1].tiles_to_play == 5);
  assert(anchor_list.anchors[1].row == 7);
  assert(anchor_list.anchors[1].col == 7);
  assert(anchor_list.anchors[1].dir == BOARD_HORIZONTAL_DIRECTION);
  assert(anchor_list.anchors[1].playthrough_blocks == 0);

  // 4 tiles, Gxxx
  assert_anchor_equity_int(&anchor_list, 2, 10);
  assert_anchor_score(&anchor_list, 2, 10);
  assert(anchor_list.anchors[2].tiles_to_play == 4);
  assert(anchor_list.anchors[2].row == 7);
  assert(anchor_list.anchors[2].col == 7);
  assert(anchor_list.anchors[2].dir == BOARD_HORIZONTAL_DIRECTION);
  assert(anchor_list.anchors[2].playthrough_blocks == 0);

  // 3 tiles, Gxx
  assert_anchor_equity_int(&anchor_list, 3, 8);
  assert_anchor_score(&anchor_list, 3, 8);
  assert(anchor_list.anchors[3].tiles_to_play == 3);
  assert(anchor_list.anchors[3].row == 7);
  assert(anchor_list.anchors[3].col == 7);
  assert(anchor_list.anchors[3].dir == BOARD_HORIZONTAL_DIRECTION);
  assert(anchor_list.anchors[3].playthrough_blocks == 0);

  // 2 tiles, Gx
  assert_anchor_equity_int(&anchor_list, 4, 6);
  assert_anchor_score(&anchor_list, 4, 6);
  assert(anchor_list.anchors[4].tiles_to_play == 2);
  assert(anchor_list.anchors[4].row == 7);
  assert(anchor_list.anchors[4].col == 7);
  assert(anchor_list.anchors[4].dir == BOARD_HORIZONTAL_DIRECTION);
  assert(anchor_list.anchors[4].playthrough_blocks == 0);

  load_and_shadow(game, player, EMPTY_CGP, "VVWWXYZ", &anchor_list);
  // No words
  assert(anchor_list.count == 0);

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
  assert(anchor_list.count == 46);

  // f9 UFTRIDE for 88, not 8g (QI)DURFITE for 128
  assert_anchor_equity_int(&anchor_list, 0, 88);
  assert_anchor_score(&anchor_list, 0, 88);
  assert(anchor_list.anchors[0].tiles_to_play == 7);
  assert(anchor_list.anchors[0].row == 5);
  assert(anchor_list.anchors[0].col == 8);
  assert(anchor_list.anchors[0].dir == BOARD_VERTICAL_DIRECTION);
  assert(anchor_list.anchors[0].playthrough_blocks == 0);

  load_and_shadow(game, player, qi_qis, "AOUNS??", &anchor_list);
  assert(anchor_list.count == 56);

  // 8g (QI)NghAOSU
  assert_anchor_equity_int(&anchor_list, 0, 101);
  assert_anchor_score(&anchor_list, 0, 101);
  assert(anchor_list.anchors[0].tiles_to_play == 7);
  assert(anchor_list.anchors[0].row == 7);
  assert(anchor_list.anchors[0].col == 7);
  assert(anchor_list.anchors[0].dir == BOARD_HORIZONTAL_DIRECTION);
  assert(anchor_list.anchors[0].playthrough_blocks == 1);

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

void test_shadow_wmp_one_tile(void) {
  Config *config = config_create_or_die("set -lex CSW21 -wmp true");
  Game *game = config_game_create(config);
  Player *player = game_get_player(game, 0);

  player_set_move_sort_type(player, MOVE_SORT_SCORE);

  AnchorHeap ah;
  // recursive_gen creates anchors in both directions (because they can extend
  // to more than one tile) but wordmap_gen can avoid this and only create the
  // horizontal version.
  load_and_shadow(game, player, QI_QI_CGP, "D", &ah);
  assert(ah.count == 2);

  // Max score is 6 for 9F (I)D, can also record D(I) at 9G
  assert_anchor_score(&ah, 0, 6);
  assert(ah.anchors[0].row == 8);
  assert(ah.anchors[0].col == 6);
  assert(ah.anchors[0].dir == BOARD_HORIZONTAL_DIRECTION);
  assert(ah.anchors[0].tiles_to_play == 1);
  assert(ah.anchors[0].playthrough_blocks == 1);
  assert(ah.anchors[0].leftmost_start_col == 5);
  assert(ah.anchors[0].rightmost_start_col == 6);

  // Max score is 3 for H7 D(I). Does not allow H8 (I)D
  assert_anchor_score(&ah, 1, 3);
  assert(ah.anchors[1].row == 7);
  assert(ah.anchors[1].col == 7);
  assert(ah.anchors[1].dir == BOARD_VERTICAL_DIRECTION);
  assert(ah.anchors[1].tiles_to_play == 1);
  assert(ah.anchors[1].playthrough_blocks == 1);
  assert(ah.anchors[1].leftmost_start_col == 6);
  assert(ah.anchors[1].rightmost_start_col == 6);

  game_destroy(game);
  config_destroy(config);
}

void test_shadow(void) {
  test_shadow_score();
  test_shadow_wmp_nonplaythrough_existence();
  test_shadow_wmp_playthrough_bingo_existence();
  test_shadow_wmp_one_tile();
  test_shadow_top_move();
}