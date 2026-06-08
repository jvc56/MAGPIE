#include "peg_test.h"

#include "../src/def/equity_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/move_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/game.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/validated_move.h"
#include "../src/impl/config.h"
#include "../src/impl/move_gen.h"
#include "../src/impl/peg.h"
#include "../src/str/move_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

// Several macondo PEG positions store an empty opponent rack, so MAGPIE loads
// the whole unseen pool into the literal bag (a too-big bag peg_solve rejects).
// Repack the unseen pool deterministically: sort it, keep the `target` smallest
// tiles in the bag, and put the rest in the opponent's rack — a genuine
// N-in-bag position with the SAME unseen pool (peg_solve reconstructs the
// opponent rack per scenario, so the specific split does not change the result,
// but a fixed split keeps a pinned single-scenario slice reproducible).
static void peg_redistribute_bag_to_opp(const Game *game, int mover_idx,
                                        int target) {
  Bag *bag = game_get_bag(game);
  Rack *opp_rack = player_get_rack(game_get_player(game, 1 - mover_idx));
  MachineLetter unseen[MAX_BAG_SIZE];
  const int n = bag_peek_tiles(bag, unseen);
  for (int i = 1; i < n; i++) {
    const MachineLetter key = unseen[i];
    int j = i - 1;
    while (j >= 0 && unseen[j] > key) {
      unseen[j + 1] = unseen[j];
      j--;
    }
    unseen[j + 1] = key;
  }
  bag_set_to_tiles(bag, unseen, target);
  for (int i = target; i < n; i++) {
    rack_add_letter(opp_rack, unseen[i]);
  }
}

// Run peg_solve on `cgp` restricted to a single candidate, and assert the
// published best move is that candidate with a sane win%. When `move_str` is
// non-NULL it is parsed as the candidate (used to force the pass); otherwise
// the single highest-equity generated move is used. A single only_move means
// the halving stages are skipped (they need >= 2 candidates to re-rank), so
// this exercises just the deterministic stage-0 greedy evaluation — fast enough
// for the main suite.
//
// When single_bag_ordering is set, the candidate is evaluated against exactly
// one scenario — the position's actual bag ordering — via
// PegArgs.eval_bag_order (no enumeration), which keeps the 3- and 4-in-bag
// cases cheap.
static void peg_assert_single_cand(const char *name, const char *cgp,
                                   const char *move_str,
                                   bool single_bag_ordering, int max_scenarios,
                                   PegOppModel opp_model, double *out_spread) {
  Config *config = config_create_or_die("set -threads 4 -s1 score -s2 score");
  load_and_exec_config_or_die(config, cgp);
  Game *game = config_get_game(config);
  const int mover_idx = game_get_player_on_turn_index(game);
  const Board *board = game_get_board(game);
  const LetterDistribution *ld = game_get_ld(game);

  ErrorStack *error_stack = error_stack_create();

  // The candidate: either the parsed move_str (for the pass) or the single
  // highest-equity generated move. Whichever owns it outlives the solve.
  ValidatedMoves *vms = NULL;
  MoveList *gen_ml = NULL;
  const Move *cand = NULL;
  if (move_str != NULL) {
    vms = validated_moves_create(game, mover_idx, move_str,
                                 /*allow_phonies=*/false,
                                 /*allow_playthrough=*/true, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      log_fatal("[%s] failed to parse move '%s'", name, move_str);
    }
    assert(validated_moves_get_number_of_moves(vms) >= 1);
    cand = validated_moves_get_move(vms, 0);
  } else {
    gen_ml = move_list_create(1);
    const MoveGenArgs gen_args = {
        .game = game,
        .move_list = gen_ml,
        .move_record_type = MOVE_RECORD_BEST,
        .move_sort_type = MOVE_SORT_EQUITY,
        .override_kwg = NULL,
        .eq_margin_movegen = 0,
        .target_equity = EQUITY_MAX_VALUE,
        .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
    };
    generate_moves(&gen_args);
    assert(move_list_get_count(gen_ml) >= 1);
    cand = move_list_get_move(gen_ml, 0);
  }
  const Move *only_moves[1] = {cand};

  PegArgs args;
  memset(&args, 0, sizeof(args));
  args.game = game;
  args.thread_control = config_get_thread_control(config);
  args.num_threads = 4;
  args.time_budget_seconds = 10.0; // generous cap; a single cand is far quicker
  args.only_moves = only_moves;
  args.n_only_moves = 1;
  args.opp_model = opp_model;

  // Pin the evaluation to a single bag ordering (one scenario) for the 3-/4-in-
  // bag cases, so they stay cheap. The position fixes the bag's multiset, but a
  // CGP load shuffles its order with a time seed, so sort to a deterministic
  // ordering — the test is then fully reproducible run-to-run.
  MachineLetter bag_order[MAX_BAG_SIZE];
  if (single_bag_ordering) {
    const int n_bag = bag_peek_tiles(game_get_bag(game), bag_order);
    for (int i = 1; i < n_bag; i++) {
      const MachineLetter key = bag_order[i];
      int j = i - 1;
      while (j >= 0 && bag_order[j] > key) {
        bag_order[j + 1] = bag_order[j];
        j--;
      }
      bag_order[j + 1] = key;
    }
    args.eval_bag_order = bag_order;
    args.eval_bag_order_len = n_bag;
  }

  PegResult result;
  memset(&result, 0, sizeof(result));
  peg_solve(&args, &result, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(result.n_top_cands == 1);

  const PegRankedCand *top = &result.top_cands[0];
  assert(top->n_scenarios >= 1);
  if (max_scenarios > 0) {
    assert(top->n_scenarios <= max_scenarios);
  }
  assert(top->win_pct >= 0.0 && top->win_pct <= 1.0);

  // The published best move must be exactly the single candidate we supplied.
  StringBuilder *best_sb = string_builder_create();
  string_builder_add_move(best_sb, board, &result.best_move, ld, false);
  StringBuilder *cand_sb = string_builder_create();
  string_builder_add_move(cand_sb, board, cand, ld, false);
  assert(strings_equal(string_builder_peek(best_sb),
                       string_builder_peek(cand_sb)));

  printf("[%s] cand=%s win=%.4f spread=%+.3f scen=%d\n", name,
         string_builder_peek(cand_sb), top->win_pct, top->mean_spread,
         top->n_scenarios);
  if (out_spread != NULL) {
    *out_spread = top->mean_spread;
  }

  string_builder_destroy(best_sb);
  string_builder_destroy(cand_sb);
  if (vms != NULL) {
    validated_moves_destroy(vms);
  }
  if (gen_ml != NULL) {
    move_list_destroy(gen_ml);
  }
  error_stack_destroy(error_stack);
  peg_result_destroy(&result);
  config_destroy(config);
}

// 1-in-bag, pass candidate. Engineered position where passing is strictly best:
// mover and opp hold the same bingo rack with a Q stranded in the bag, so
// passing forces opp to draw the unplayable Q. peg_solve evaluates the pass
// over all 8 one-tile-in-bag scenarios (win 1.0).
static void test_peg_main_1bag_pass(void) {
  const char *cgp =
      "cgp 7C6D/7H4LAR/7I2P1ALA/7VOGUE1AG/6RERAN2M1/7S1BY2O1/8OY2Id1/"
      "5JEUX3NEW/3C1U2O3A1E/3O1M6N1B/3ZIP2OAK1E2/2TI1sTIFLERS2/2WED5F1T2/"
      "1HIDEOUT7/VEG1N2IDOL4 AEINRST/AEINRST 372/369 0 -lex CSW24";
  peg_assert_single_cand("peg_1bag_pass", cgp, "pass",
                         /*single_bag_ordering=*/false, /*max_scen=*/0,
                         PEG_OPP_RATIONAL, /*out_spread=*/NULL);
}

// 1-in-bag, scoring candidate: the highest-equity play on macondo's
// "Straightforward1PEG" board, evaluated over all one-tile-in-bag scenarios.
static void test_peg_main_1bag_score(void) {
  const char *cgp =
      "cgp 15/3Q7U3/3U2TAURINE2/1CHANSONS2W3/2AI6JO3/DIRL1PO3IN3/E1D2EF3V4/"
      "F1I2p1TRAIK3/O1L2T4E4/ABy1PIT2BRIG2/ME1MOZELLE5/1GRADE1O1NOH3/"
      "WE3R1V7/AT5E7/G6D7 ENOSTXY/ACEISUY 356/378 0 -lex NWL20";
  peg_assert_single_cand("peg_1bag_score", cgp, NULL,
                         /*single_bag_ordering=*/false, /*max_scen=*/0,
                         PEG_OPP_RATIONAL, /*out_spread=*/NULL);
}

// 2-in-bag, single candidate (highest-equity play) over the full 2-in-bag
// scenario enumeration. macondo "ACIDOTIc" board.
static void test_peg_main_2bag_single(void) {
  const char *cgp =
      "cgp 5U4OHMIC/5N3WREATH/5T4FAX2/5i3B1V3/5N3L1E3/5G2VELDT2/5E3S5/"
      "5DREKS1F3/8YELL3/4ABASER1U3/4GYM3ZO3/WAITE5OR2J/10OI2A/"
      "3QUOIT1PINNER/4RENEGADE2P ACDIOT?/AIIIOSU 431/392 0 -lex CSW24";
  peg_assert_single_cand("peg_2bag_single", cgp, NULL,
                         /*single_bag_ordering=*/false, /*max_scen=*/0,
                         PEG_OPP_RATIONAL, /*out_spread=*/NULL);
}

// 3-in-bag, single scenario of a single candidate (a genuine random 3-in-bag
// position with both racks full). eval_bag_order pins the evaluation to the
// position's actual 3-tile bag ordering, so exactly one scenario runs.
static void test_peg_main_3bag_single(void) {
  const char *cgp =
      "cgp BEDEL10/R1R9U2/O1IT1Q5OM2/W1BIDI4YUM2/N2XI5AT3/E3G4T1R3/"
      "S1VOILE2OKA3/T3T1DISPACED1/9AWE1O1/9Z1s1FA/14R/13GO/13AH/"
      "3JUVIE4UTA/INRO3FLENCHES ?ANNOPY/AEGILNS 344/368 0 -lex CSW21";
  peg_assert_single_cand("peg_3bag_single", cgp, NULL,
                         /*single_bag_ordering=*/true, /*max_scen=*/1,
                         PEG_OPP_RATIONAL, /*out_spread=*/NULL);
}

// 4-in-bag, single scenario of a single candidate (a genuine random 4-in-bag
// position with both racks full). As above, eval_bag_order pins the evaluation
// to the position's actual 4-tile bag ordering.
static void test_peg_main_4bag_single(void) {
  const char *cgp =
      "cgp 3V3W6L/1BEATY1U5GI/2XU3S4FEN/3TA2H4LOY/2GEN1DUCAT1AD1/"
      "2O1I1I2WRITE1/2V1M1ZOAEA4/3JAGER2DRILL/2BOtONE5O1/1FERER7Q1/4S8U1/"
      "12NaM/12ATE/13ST/14H ACEINOP/DEIINOS 361/397 0 -lex CSW24";
  peg_assert_single_cand("peg_4bag_single", cgp, NULL,
                         /*single_bag_ordering=*/true, /*max_scen=*/1,
                         PEG_OPP_RATIONAL, /*out_spread=*/NULL);
}

// Opponent modeling: rational vs pessimistic. Uses the 4-in-bag position with a
// single pinned bag ordering; the top candidate leaves the bag non-empty, so
// the opponent still draws and the opponent model actually drives the playout.
// The same candidate is scored under each model (two separate cases); the
// pessimistic mover spread must be <= the rational one — and here it is sharply
// worse — because the pessimistic opponent plays its worst-for-the-mover reply.
static void test_peg_main_opp_models(void) {
  const char *cgp =
      "cgp 3V3W6L/1BEATY1U5GI/2XU3S4FEN/3TA2H4LOY/2GEN1DUCAT1AD1/"
      "2O1I1I2WRITE1/2V1M1ZOAEA4/3JAGER2DRILL/2BOtONE5O1/1FERER7Q1/4S8U1/"
      "12NaM/12ATE/13ST/14H ACEINOP/DEIINOS 361/397 0 -lex CSW24";
  double rational = 0.0;
  double pessimistic = 0.0;
  peg_assert_single_cand("peg_opp_rational", cgp, NULL,
                         /*single_bag_ordering=*/true, /*max_scen=*/1,
                         PEG_OPP_RATIONAL, &rational);
  peg_assert_single_cand("peg_opp_pessimistic", cgp, NULL,
                         /*single_bag_ordering=*/true, /*max_scen=*/1,
                         PEG_OPP_PESSIMISTIC, &pessimistic);
  // A pessimistic opponent never helps the mover relative to a rational one.
  assert(pessimistic <= rational + 1e-6);
  printf("[peg_opp_models] rational_spread=%+.3f pessimistic_spread=%+.3f\n",
         rational, pessimistic);
}

// Run a macondo PEG position. move_str != NULL restricts the solve to that one
// candidate (only_moves); NULL runs full move generation. redistribute_bag > 0
// first converts an empty-opponent-rack macondo CGP into a genuine N-in-bag
// position. single_ordering pins the position's (sorted) bag as the only
// scenario — a single (candidate, scenario) slice. expect_best, when non-NULL,
// must be a substring of the published best move's text. The top candidate's
// win/spread are returned via the out params (NULL to ignore).
static void peg_macondo_case(const char *name, const char *cgp,
                             const char *move_str, int redistribute_bag,
                             bool single_ordering, PegOppModel opp_model,
                             double time_budget, const char *expect_best,
                             double *out_win, double *out_spread) {
  Config *config = config_create_or_die("set -threads 4 -s1 score -s2 score");
  load_and_exec_config_or_die(config, cgp);
  Game *game = config_get_game(config);
  const int mover_idx = game_get_player_on_turn_index(game);
  const Board *board = game_get_board(game);
  const LetterDistribution *ld = game_get_ld(game);
  if (redistribute_bag > 0) {
    peg_redistribute_bag_to_opp(game, mover_idx, redistribute_bag);
  }

  ErrorStack *error_stack = error_stack_create();
  MoveList *gen_ml = NULL;
  const Move *only_moves[1];
  PegArgs args;
  memset(&args, 0, sizeof(args));
  args.game = game;
  args.thread_control = config_get_thread_control(config);
  args.num_threads = 4;
  args.time_budget_seconds = time_budget;
  args.opp_model = opp_model;
  // For a full-movegen solve, a short halving cascade reaches the endgame-leaf
  // verdict on the few real contenders without evaluating the deep stages over
  // dozens of also-rans — plenty for these decisive positions and far faster.
  static const int kShortCascade[] = {4, 2};
  if (move_str == NULL) {
    args.stage_top_k = kShortCascade;
    args.num_stages = 2;
  }
  if (move_str != NULL) {
    // Find the candidate by its rendered text rather than parsing a move
    // string — robust to playthrough/coordinate input quirks (e.g. a move
    // that opens with a played-through tile like "6F (A)X(E)").
    gen_ml = move_list_create(4096);
    const MoveGenArgs gen_args = {
        .game = game,
        .move_list = gen_ml,
        .move_record_type = MOVE_RECORD_ALL,
        .move_sort_type = MOVE_SORT_EQUITY,
        .override_kwg = NULL,
        .eq_margin_movegen = 0,
        .target_equity = EQUITY_MAX_VALUE,
        .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
    };
    generate_moves(&gen_args);
    const int n_gen = move_list_get_count(gen_ml);
    const Move *found = NULL;
    StringBuilder *msb = string_builder_create();
    for (int i = 0; i < n_gen && found == NULL; i++) {
      const Move *m = move_list_get_move(gen_ml, i);
      string_builder_clear(msb);
      string_builder_add_move(msb, board, m, ld, false);
      if (strings_equal(string_builder_peek(msb), move_str)) {
        found = m;
      }
    }
    string_builder_destroy(msb);
    if (found == NULL) {
      log_fatal("[%s] candidate '%s' was not generated", name, move_str);
    }
    only_moves[0] = found;
    args.only_moves = only_moves;
    args.n_only_moves = 1;
  }
  MachineLetter bag_order[MAX_BAG_SIZE];
  if (single_ordering) {
    const int n_bag = bag_peek_tiles(game_get_bag(game), bag_order);
    for (int i = 1; i < n_bag; i++) {
      const MachineLetter key = bag_order[i];
      int j = i - 1;
      while (j >= 0 && bag_order[j] > key) {
        bag_order[j + 1] = bag_order[j];
        j--;
      }
      bag_order[j + 1] = key;
    }
    args.eval_bag_order = bag_order;
    args.eval_bag_order_len = n_bag;
  }

  PegResult result;
  memset(&result, 0, sizeof(result));
  peg_solve(&args, &result, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(result.n_top_cands >= 1);
  const PegRankedCand *top = &result.top_cands[0];

  StringBuilder *best_sb = string_builder_create();
  string_builder_add_move(best_sb, board, &result.best_move, ld, false);
  const char *best_txt = string_builder_peek(best_sb);
  if (expect_best != NULL && strstr(best_txt, expect_best) == NULL) {
    log_fatal("[%s] best move '%s' does not contain expected '%s'", name,
              best_txt, expect_best);
  }
  printf("[%s] best=%s win=%.4f spread=%+.3f scen=%d\n", name, best_txt,
         top->win_pct, top->mean_spread, top->n_scenarios);
  if (out_win != NULL) {
    *out_win = top->win_pct;
  }
  if (out_spread != NULL) {
    *out_spread = top->mean_spread;
  }

  string_builder_destroy(best_sb);
  if (gen_ml != NULL) {
    move_list_destroy(gen_ml);
  }
  error_stack_destroy(error_stack);
  peg_result_destroy(&result);
  config_destroy(config);
}

// macondo TestStraightforward1PEG: 1-in-bag, 13L ONYX wins 7.5/8 endgames. ONYX
// empties the one-tile bag, so each scenario reduces to an exact endgame — the
// one macondo number peg_solve reproduces exactly. Full solve; assert the
// solver's best move is 13L ONYX with a high win%.
static void test_peg_macondo_onyx(void) {
  const char *cgp =
      "cgp 15/3Q7U3/3U2TAURINE2/1CHANSONS2W3/2AI6JO3/DIRL1PO3IN3/E1D2EF3V4/"
      "F1I2p1TRAIK3/O1L2T4E4/ABy1PIT2BRIG2/ME1MOZELLE5/1GRADE1O1NOH3/"
      "WE3R1V7/AT5E7/G6D7 ENOSTXY/ACEISUY 356/378 0 -lex NWL20";
  double win = 0.0;
  peg_macondo_case("peg_macondo_onyx", cgp, /*move_str=*/NULL,
                   /*redistribute_bag=*/0, /*single_ordering=*/false,
                   PEG_OPP_RATIONAL, /*time_budget=*/5.0,
                   /*expect_best=*/"13L ONYX", &win, /*out_spread=*/NULL);
  assert(win >= 0.9); // macondo: 7.5/8 = 0.9375
}

// macondo Test1PEGPass: a French (FRA20) 1-in-bag where passing is best (W-L-D
// 5-2-1). The macondo CGP stores an empty opponent rack, so redistribute to a
// genuine 1-in-bag, then a full solve must rank the pass first.
static void test_peg_macondo_french_pass(void) {
  const char *cgp =
      "cgp 11ONZE/10J2O1/8A1E1DO1/7QUETEE1H/10E1F1U/8ECUMERA/8C1R1TIR/"
      "7WOKS2ET/6DUR6/5G2N1M4/4HALLALiS3/1G1P1P1OM1XI3/VIVONS1BETEL3/"
      "IF1N3AS1RYAL1/ETUDIAIS7 AEINRST/ 301/300 0 -lex FRA20";
  peg_macondo_case("peg_macondo_french_pass", cgp, /*move_str=*/NULL,
                   /*redistribute_bag=*/1, /*single_ordering=*/false,
                   PEG_OPP_RATIONAL, /*time_budget=*/5.0,
                   /*expect_best=*/"pass", /*out_win=*/NULL,
                   /*out_spread=*/NULL);
}

// macondo TestTwoInBagSingleMove: 2-in-bag, 6F (A)X(E) wins 70/72 = 0.9722 of
// the bag permutations. Scored over the full 2-bag enumeration as the single
// candidate; the win matches macondo's 70/72 exactly.
static void test_peg_macondo_axe(void) {
  const char *cgp =
      "cgp 1T13/1W3Q9/VERB1U9/1E1OPIUM5C1/1LAWIN1I5O1/1Y3A1E5R1/7V4NO1/"
      "NOTArIZE1C2UN1/6ODAH2LA1/3TAHA2I2LED/2JUT4R2A1O/3G5P4D/3R3BrIEFING/"
      "3I5L4E/3K2DESYNES1M AEFGSTX/EEIOOST 370/341 0 -lex CSW21";
  double win = 0.0;
  peg_macondo_case("peg_macondo_axe", cgp, "6F (A)X(E)",
                   /*redistribute_bag=*/0, /*single_ordering=*/false,
                   PEG_OPP_RATIONAL, /*time_budget=*/5.0, /*expect_best=*/NULL,
                   &win, /*out_spread=*/NULL);
  assert(win >= 70.0 / 72.0 - 0.005 && win <= 70.0 / 72.0 + 0.005);
}

// macondo manual Position #2: 13M P(AH). A single (candidate, scenario) slice —
// the candidate evaluated against one pinned bag ordering under the pessimistic
// (macondo-style) opponent model.
static void test_peg_macondo_pah_slice(void) {
  const char *cgp =
      "cgp BEDEL10/R1R9U2/O1IT1Q5OM2/W1BIDI4YUM2/N2XI5AT3/E3G4T1R3/"
      "S1VOILE2OKA3/T3T1DISPACED1/9AWE1O1/9Z1s1FA/14R/13GO/13AH/"
      "3JUVIE4UTA/INRO3FLENCHES ?ANNOPY/AEGILNS 344/368 0 -lex CSW21";
  peg_macondo_case("peg_macondo_pah_slice", cgp, "13M P(AH)",
                   /*redistribute_bag=*/0, /*single_ordering=*/true,
                   PEG_OPP_PESSIMISTIC, /*time_budget=*/5.0,
                   /*expect_best=*/"P(AH)", /*out_win=*/NULL,
                   /*out_spread=*/NULL);
}

// macondo manual Position #1: 2L P(O)ND. The macondo CGP has an empty opponent
// rack, so redistribute to a genuine 4-in-bag, then evaluate the candidate
// against one pinned bag ordering (a single slice) under the pessimistic model.
static void test_peg_macondo_pond_slice(void) {
  const char *cgp =
      "cgp 12D2/1U10O2/1p10L2/1R1C3KANJIS2/1I1O3A2U4/1G1T3I2I4/1H1E3Z2C1LOO/"
      "1TED3E1BYWORD/2Q4N3AXE1/1RuBIGOS3I3/F1A5WEAVE2/O1T8E3/V1E5LOURY2/"
      "ENSNARL2HM4/A6TEMP4 DEFNNPT/ 394/365 0 -lex NWL20";
  peg_macondo_case("peg_macondo_pond_slice", cgp, "2L P(O)ND",
                   /*redistribute_bag=*/4, /*single_ordering=*/true,
                   PEG_OPP_PESSIMISTIC, /*time_budget=*/5.0,
                   /*expect_best=*/"P(O)ND", /*out_win=*/NULL,
                   /*out_spread=*/NULL);
}

void test_peg(void) {
  log_set_level(LOG_FATAL);
  test_peg_main_1bag_pass();
  test_peg_main_1bag_score();
  test_peg_main_2bag_single();
  test_peg_main_3bag_single();
  test_peg_main_4bag_single();
  test_peg_main_opp_models();
  // Cases drawn from the macondo codebase + pre-endgame manual.
  test_peg_macondo_onyx();
  test_peg_macondo_french_pass();
  test_peg_macondo_axe();
  test_peg_macondo_pah_slice();
  test_peg_macondo_pond_slice();
}
