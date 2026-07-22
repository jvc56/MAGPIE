#include "peg_test.h"

#include "../src/compat/ctime.h"
#include "../src/def/equity_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/move_defs.h"
#include "../src/def/thread_control_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/board.h"
#include "../src/ent/game.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/thread_control.h"
#include "../src/ent/validated_move.h"
#include "../src/impl/config.h"
#include "../src/impl/move_gen.h"
#include "../src/impl/peg.h"
#include "../src/str/move_string.h"
#include "../src/str/peg_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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
                         /*single_bag_ordering=*/false, /*max_scenarios=*/0,
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
                         /*single_bag_ordering=*/false, /*max_scenarios=*/0,
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
                         /*single_bag_ordering=*/true, /*max_scenarios=*/1,
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
                         /*single_bag_ordering=*/true, /*max_scenarios=*/1,
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
                         /*single_bag_ordering=*/true, /*max_scenarios=*/1,
                         PEG_OPP_RATIONAL, &rational);
  peg_assert_single_cand("peg_opp_pessimistic", cgp, NULL,
                         /*single_bag_ordering=*/true, /*max_scenarios=*/1,
                         PEG_OPP_PESSIMISTIC, &pessimistic);
  // A pessimistic opponent never helps the mover relative to a rational one.
  assert(pessimistic <= rational + 1e-6);
  printf("[peg_opp_models] rational_spread=%+.3f pessimistic_spread=%+.3f\n",
         rational, pessimistic);
}

// Run a macondo PEG position. move_str != NULL restricts the solve to that
// candidate (only_moves), found by matching its rendered text; move_str2 adds a
// second only-solve candidate (so the halving stages can run, which a lone
// candidate skips). move_str == NULL runs full move generation.
// redistribute_bag > 0 first converts an empty-opponent-rack macondo CGP into a
// genuine N-in-bag position. single_ordering pins the position's (sorted) bag
// as the only scenario — a single (candidate, scenario) slice. expect_best,
// when non-NULL, must be a substring of the published best move's text. The top
// candidate's win/spread are returned via the out params (NULL to ignore).
static void peg_macondo_case(const char *name, const char *cgp,
                             const char *move_str, const char *move_str2,
                             int redistribute_bag, bool single_ordering,
                             PegOppModel opp_model, double time_budget,
                             const char *expect_best, double *out_win,
                             double *out_spread) {
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
  const Move *only_moves[2];
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
    const char *targets[2] = {move_str, move_str2};
    const int n_targets = (move_str2 != NULL) ? 2 : 1;
    StringBuilder *msb = string_builder_create();
    for (int t = 0; t < n_targets; t++) {
      const Move *found = NULL;
      for (int i = 0; i < n_gen && found == NULL; i++) {
        const Move *m = move_list_get_move(gen_ml, i);
        string_builder_clear(msb);
        string_builder_add_move(msb, board, m, ld, false);
        if (strings_equal(string_builder_peek(msb), targets[t])) {
          found = m;
        }
      }
      if (found == NULL) {
        log_fatal("[%s] candidate '%s' was not generated", name, targets[t]);
      }
      only_moves[t] = found;
    }
    string_builder_destroy(msb);
    args.only_moves = only_moves;
    args.n_only_moves = n_targets;
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

// macondo TestStraightforward1PEG: the well-studied 1-in-bag where 13L ONYX
// wins 7.5/8 endgames (ties only when the bag tile is the Y), beating the
// higher-scoring 13L OXY. Only-solve those two candidates (macondo's multi
// `-only-solve` workflow); two candidates let the halving stages evaluate ONYX
// at exact endgame fidelity (a lone only-move would fall back to the greedy
// stage-0 leaf and report ~0.625, not the studied 7.5/8). ONYX empties the
// one-tile bag, so its scenarios are exact endgames and the win matches
// macondo's 7.5/8 = 0.9375 exactly.
static void test_peg_macondo_only_onyx(void) {
  const char *cgp =
      "cgp 15/3Q7U3/3U2TAURINE2/1CHANSONS2W3/2AI6JO3/DIRL1PO3IN3/E1D2EF3V4/"
      "F1I2p1TRAIK3/O1L2T4E4/ABy1PIT2BRIG2/ME1MOZELLE5/1GRADE1O1NOH3/"
      "WE3R1V7/AT5E7/G6D7 ENOSTXY/ACEISUY 356/378 0 -lex NWL20";
  double win = 0.0;
  // No time limit (budget 0 = unbounded): this case asserts an EXACT endgame
  // value (7.5/8 = 0.9375), which is only produced when the halving stage runs
  // to full fidelity. Any wall-clock cap could truncate the deep stage on a
  // slow (ASAN/instrumented) build and drop the result to the ~0.625
  // greedy-stage-0 fallback. The position is small and deterministic, so the
  // solve terminates on its own (sub-second on a release build).
  peg_macondo_case("peg_macondo_only_onyx", cgp, "13L ONYX", "13L OXY",
                   /*redistribute_bag=*/0, /*single_ordering=*/false,
                   PEG_OPP_RATIONAL, /*time_budget=*/0.0,
                   /*expect_best=*/"13L ONYX", &win, /*out_spread=*/NULL);
  assert(win >= (7.5 / 8.0) - 0.005 && win <= (7.5 / 8.0) + 0.005); // 0.9375
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
                   /*move_str2=*/NULL, /*redistribute_bag=*/1,
                   /*single_ordering=*/false, PEG_OPP_RATIONAL,
                   /*time_budget=*/5.0,
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
  // No time limit (budget 0 = unbounded): asserts an EXACT value (70/72), only
  // produced when the full 2-bag enumeration runs to fidelity. A wall-clock cap
  // could truncate the deep stage on a slow build and drop the win, failing the
  // assertion. This is a 2-in-bag position (heavier than the 1-in-bag onyx
  // case), so it is the most cap-exposed of the macondo cases. (Measured: the
  // solve completes in ~0.07s on a release build, so the old 5s cap was never
  // actually hit in CI -- it was only latent risk on slow/instrumented builds.)
  peg_macondo_case("peg_macondo_axe", cgp, "6F (A)X(E)", /*move_str2=*/NULL,
                   /*redistribute_bag=*/0, /*single_ordering=*/false,
                   PEG_OPP_RATIONAL, /*time_budget=*/0.0, /*expect_best=*/NULL,
                   &win, /*out_spread=*/NULL);
  assert(win >= (70.0 / 72.0) - 0.005 && win <= (70.0 / 72.0) + 0.005);
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
                   /*move_str2=*/NULL, /*redistribute_bag=*/0,
                   /*single_ordering=*/true, PEG_OPP_PESSIMISTIC,
                   /*time_budget=*/5.0,
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
                   /*move_str2=*/NULL, /*redistribute_bag=*/4,
                   /*single_ordering=*/true, PEG_OPP_PESSIMISTIC,
                   /*time_budget=*/5.0,
                   /*expect_best=*/"P(O)ND", /*out_win=*/NULL,
                   /*out_spread=*/NULL);
}

// True if any of the result's top candidates renders as `text`.
static bool peg_top_cands_contains(const PegResult *result, const Board *board,
                                   const LetterDistribution *ld,
                                   const char *text) {
  StringBuilder *sb = string_builder_create();
  bool found = false;
  for (int i = 0; i < result->n_top_cands && !found; i++) {
    string_builder_clear(sb);
    string_builder_add_move(sb, board, &result->top_cands[i].move, ld, false);
    found = strings_equal(string_builder_peek(sb), text);
  }
  string_builder_destroy(sb);
  return found;
}

// Exercises PegArgs.protect_moves ("pnoprune", like the simmer's snoprune): a
// candidate the cascade's top-K cut would normally drop is force-carried into
// the final ranking when listed as protected. Runs the same short {2}-cascade
// solve twice on a candidate-rich 1-in-bag board — once plain, once protecting
// a move that the plain solve pruned — and asserts the move is absent without
// protection and present with it.
static void test_peg_main_pnoprune(void) {
  const char *cgp =
      "cgp 15/3Q7U3/3U2TAURINE2/1CHANSONS2W3/2AI6JO3/DIRL1PO3IN3/E1D2EF3V4/"
      "F1I2p1TRAIK3/O1L2T4E4/ABy1PIT2BRIG2/ME1MOZELLE5/1GRADE1O1NOH3/"
      "WE3R1V7/AT5E7/G6D7 ENOSTXY/ACEISUY 356/378 0 -lex NWL20";
  Config *config = config_create_or_die("set -threads 4 -s1 score -s2 score");
  load_and_exec_config_or_die(config, cgp);
  Game *game = config_get_game(config);
  const Board *board = game_get_board(game);
  const LetterDistribution *ld = game_get_ld(game);
  ErrorStack *error_stack = error_stack_create();

  // Every legal play, equity-sorted, to draw a protectable straggler from.
  MoveList *gen_ml = move_list_create(4096);
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
  assert(n_gen > 3);

  // Keep only the top 2 candidates each stage, so most plays are pruned.
  static const int stage_top_k[] = {2};
  PegArgs base;
  memset(&base, 0, sizeof(base));
  base.game = game;
  base.thread_control = config_get_thread_control(config);
  base.num_threads = 4;
  base.time_budget_seconds = 10.0;
  base.stage_top_k = stage_top_k;
  base.num_stages = 1;

  // Plain solve (no protection): its top_cands are the un-protected survivors.
  PegResult plain;
  memset(&plain, 0, sizeof(plain));
  peg_solve(&base, &plain, error_stack);
  assert(error_stack_is_empty(error_stack));

  // Pick a generated move that the plain solve pruned (not in its top_cands).
  char weak_text[64] = {0};
  const Move *weak = NULL;
  StringBuilder *sb = string_builder_create();
  for (int i = 0; i < n_gen && weak == NULL; i++) {
    const Move *m = move_list_get_move(gen_ml, i);
    string_builder_clear(sb);
    string_builder_add_move(sb, board, m, ld, false);
    if (!peg_top_cands_contains(&plain, board, ld, string_builder_peek(sb))) {
      weak = m;
      (void)snprintf(weak_text, sizeof(weak_text), "%s",
                     string_builder_peek(sb));
    }
  }
  string_builder_destroy(sb);
  assert(weak != NULL);

  // Same solve, now protecting the pruned move.
  const Move *protect[1] = {weak};
  PegArgs prot = base;
  prot.protect_moves = protect;
  prot.n_protect_moves = 1;
  PegResult protected_result;
  memset(&protected_result, 0, sizeof(protected_result));
  peg_solve(&prot, &protected_result, error_stack);
  assert(error_stack_is_empty(error_stack));

  // Pruned without protection, carried into the final ranking with it.
  assert(!peg_top_cands_contains(&plain, board, ld, weak_text));
  assert(peg_top_cands_contains(&protected_result, board, ld, weak_text));
  printf("[peg_pnoprune] protected '%s' carried: plain n=%d, protected n=%d\n",
         weak_text, plain.n_top_cands, protected_result.n_top_cands);

  peg_result_destroy(&plain);
  peg_result_destroy(&protected_result);
  move_list_destroy(gen_ml);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

// Drives the `peg` CLI command end to end through the config command path
// (load_and_exec_config_or_die), exercising the -pegonly / -pegpess /
// -pegstride knobs and config_get_peg_result. The onyx board with -pegonly
// restricted to ONYX + OXY must publish 13L ONYX (the studied 7.5/8 = 0.9375
// verdict).
static void test_peg_main_cli(void) {
  Config *config = config_create_or_die("set -threads 4 -s1 score -s2 score");
  load_and_exec_config_or_die(
      config,
      "cgp 15/3Q7U3/3U2TAURINE2/1CHANSONS2W3/2AI6JO3/DIRL1PO3IN3/E1D2EF3V4/"
      "F1I2p1TRAIK3/O1L2T4E4/ABy1PIT2BRIG2/ME1MOZELLE5/1GRADE1O1NOH3/"
      "WE3R1V7/AT5E7/G6D7 ENOSTXY/ACEISUY 356/378 0 -lex NWL20");
  // -pegonly takes space-free UCGI moves; the period is the in-move separator.
  load_and_exec_config_or_die(config, "set -pegonly 13L.ONYX,13L.OXY");
  load_and_exec_config_or_die(config, "peg");

  const Game *game = config_get_game(config);
  const PegResult *result = config_get_peg_result(config);
  assert(result->last_completed_stage >= 0);
  StringBuilder *best = string_builder_create();
  string_builder_add_move(best, game_get_board(game), &result->best_move,
                          game_get_ld(game), false);
  assert(strings_equal(string_builder_peek(best), "13L ONYX"));
  string_builder_destroy(best);
  const double onyx_win = result->best_win;
  assert(onyx_win >= 0.9); // 7.5/8 = 0.9375

  // The -pegpess / -pegstride / -pegtopk knobs parse and run (still restricted
  // to the two pegonly candidates, so this stays fast).
  load_and_exec_config_or_die(config,
                              "set -pegpess true -pegstride 1 -pegtopk 4,2");
  load_and_exec_config_or_die(config, "peg");
  assert(config_get_peg_result(config)->last_completed_stage >= 0);

  printf("[peg_cli] pegonly best=13L ONYX win=%.4f\n", onyx_win);
  config_destroy(config);
}

// ----- progress callbacks + per-scenario detail -----------------------------

// user_data for the progress callbacks: thread-safe event counters, since the
// solver may fire the callbacks concurrently from worker threads.
typedef struct PegCallbackCounters {
  atomic_int stage_starts;
  atomic_int cand_dones;
  atomic_int scenario_dones;
  atomic_int max_stage_seen;
} PegCallbackCounters;

static void peg_test_on_stage_start(int stage_idx, int k_cands, int inner_d,
                                    int emptier_plies, void *user_data) {
  (void)inner_d;
  (void)emptier_plies;
  assert(k_cands >= 1);
  PegCallbackCounters *counters = user_data;
  atomic_fetch_add(&counters->stage_starts, 1);
  int prev = atomic_load(&counters->max_stage_seen);
  while (stage_idx > prev && !atomic_compare_exchange_weak(
                                 &counters->max_stage_seen, &prev, stage_idx)) {
  }
}

static void peg_test_on_cand_done(int stage_idx, int cand_rank,
                                  const Move *cand, double win_pct,
                                  double mean_spread, int scen_done,
                                  bool reordered, void *user_data) {
  (void)stage_idx;
  (void)cand_rank;
  (void)mean_spread;
  (void)reordered;
  assert(cand != NULL);
  assert(scen_done >= 1);
  assert(win_pct >= 0.0 && win_pct <= 1.0);
  PegCallbackCounters *counters = user_data;
  atomic_fetch_add(&counters->cand_dones, 1);
}

static void peg_test_on_scenario_done(int stage_idx, int cand_rank,
                                      int scenario_idx, int32_t mover_total,
                                      int64_t weight, void *user_data) {
  (void)stage_idx;
  (void)cand_rank;
  (void)scenario_idx;
  (void)mover_total;
  assert(weight > 0);
  PegCallbackCounters *counters = user_data;
  atomic_fetch_add(&counters->scenario_dones, 1);
}

// Exercises the PegArgs progress callbacks (on_stage_start / on_cand_done /
// on_scenario_done) and include_per_scenario on the candidate-rich onyx board
// with a {2}-cascade, so stage 0 and one halving stage both run. Asserts every
// callback fired and the per-scenario breakdown for the best cand is populated.
static void test_peg_main_progress_detail(void) {
  const char *cgp =
      "cgp 15/3Q7U3/3U2TAURINE2/1CHANSONS2W3/2AI6JO3/DIRL1PO3IN3/E1D2EF3V4/"
      "F1I2p1TRAIK3/O1L2T4E4/ABy1PIT2BRIG2/ME1MOZELLE5/1GRADE1O1NOH3/"
      "WE3R1V7/AT5E7/G6D7 ENOSTXY/ACEISUY 356/378 0 -lex NWL20";
  Config *config = config_create_or_die("set -threads 4 -s1 score -s2 score");
  load_and_exec_config_or_die(config, cgp);
  Game *game = config_get_game(config);
  ErrorStack *error_stack = error_stack_create();

  PegCallbackCounters counters;
  atomic_init(&counters.stage_starts, 0);
  atomic_init(&counters.cand_dones, 0);
  atomic_init(&counters.scenario_dones, 0);
  atomic_init(&counters.max_stage_seen, 0);

  static const int stage_top_k[] = {2};
  PegArgs args;
  memset(&args, 0, sizeof(args));
  args.game = game;
  args.thread_control = config_get_thread_control(config);
  args.num_threads = 4;
  args.time_budget_seconds = 10.0;
  args.stage_top_k = stage_top_k;
  args.num_stages = 1;
  args.include_per_scenario = true;
  args.on_stage_start = peg_test_on_stage_start;
  args.on_cand_done = peg_test_on_cand_done;
  args.on_scenario_done = peg_test_on_scenario_done;
  args.user_data = &counters;

  PegResult result;
  memset(&result, 0, sizeof(result));
  peg_solve(&args, &result, error_stack);
  assert(error_stack_is_empty(error_stack));

  // Stage 0 plus at least one halving stage started.
  assert(atomic_load(&counters.stage_starts) >= 2);
  assert(atomic_load(&counters.max_stage_seen) >= 1);
  // Every candidate and every scenario was reported.
  assert(atomic_load(&counters.cand_dones) >= 2);
  assert(atomic_load(&counters.scenario_dones) >= 1);

  // include_per_scenario populated the best cand's per-scenario breakdown.
  assert(result.per_scenario != NULL);
  assert(result.n_per_scenario >= 1);
  int64_t row_weight_sum = 0;
  for (int i = 0; i < result.n_per_scenario; i++) {
    const PegPerScenario *row = &result.per_scenario[i];
    assert(row->scenario_idx == i);
    assert(row->weight > 0);
    // A 1-in-bag board leaves at least one of drawn / remaining non-empty.
    assert(row->drawn[0] != '\0' || row->remaining[0] != '\0');
    row_weight_sum += row->weight;
  }
  assert(row_weight_sum > 0);
  printf("[peg_progress] stages=%d cands=%d scenarios=%d per_scenario=%d\n",
         atomic_load(&counters.stage_starts), atomic_load(&counters.cand_dones),
         atomic_load(&counters.scenario_dones), result.n_per_scenario);

  peg_result_destroy(&result);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

typedef struct PegDiscardStageContext {
  ThreadControl *thread_control;
  int interrupted_stage;
} PegDiscardStageContext;

// Interrupt the second halving stage after its first candidate finishes. The
// solver must discard that one-candidate stage and keep every published result,
// including its captured outcomes, at the preceding fidelity.
static void peg_test_interrupt_discarded_stage(int stage_idx, int cand_rank,
                                               const Move *cand, double win_pct,
                                               double mean_spread,
                                               int scen_done, bool reordered,
                                               void *user_data) {
  (void)cand;
  (void)win_pct;
  (void)mean_spread;
  (void)scen_done;
  (void)reordered;
  PegDiscardStageContext *context = user_data;
  if (stage_idx == 2 && cand_rank == 0) {
    context->interrupted_stage = stage_idx;
    thread_control_set_status(context->thread_control,
                              THREAD_CONTROL_STATUS_USER_INTERRUPT);
  }
}

static void test_peg_discarded_stage_outcomes(void) {
  const char *cgp =
      "cgp 15/3Q7U3/3U2TAURINE2/1CHANSONS2W3/2AI6JO3/DIRL1PO3IN3/E1D2EF3V4/"
      "F1I2p1TRAIK3/O1L2T4E4/ABy1PIT2BRIG2/ME1MOZELLE5/1GRADE1O1NOH3/"
      "WE3R1V7/AT5E7/G6D7 ENOSTXY/ACEISUY 356/378 0 -lex NWL20";
  Config *config = config_create_or_die("set -threads 4 -s1 score -s2 score");
  load_and_exec_config_or_die(config, cgp);
  Game *game = config_get_game(config);
  ThreadControl *thread_control = config_get_thread_control(config);
  ErrorStack *error_stack = error_stack_create();

  ValidatedMoves *moves = validated_moves_create(
      game, game_get_player_on_turn_index(game), "13L ONYX,13L OXY",
      /*allow_phonies=*/false, /*allow_playthrough=*/true, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(validated_moves_get_number_of_moves(moves) == 2);
  const Move *only_moves[2] = {validated_moves_get_move(moves, 0),
                               validated_moves_get_move(moves, 1)};

  PegPoll *poll = peg_poll_create();
  PegDiscardStageContext context = {
      .thread_control = thread_control,
      .interrupted_stage = -1,
  };
  static const int stage_top_k[] = {2, 2};
  PegArgs args;
  memset(&args, 0, sizeof(args));
  args.game = game;
  args.thread_control = thread_control;
  args.num_threads = 4;
  args.time_budget_seconds = 10.0;
  args.stage_top_k = stage_top_k;
  args.num_stages = 2;
  args.only_moves = only_moves;
  args.n_only_moves = 2;
  args.include_per_scenario = true;
  args.poll = poll;
  args.on_cand_done = peg_test_interrupt_discarded_stage;
  args.user_data = &context;

  PegResult result;
  memset(&result, 0, sizeof(result));
  peg_solve(&args, &result, error_stack);
  assert(error_stack_is_empty(error_stack));

  assert(context.interrupted_stage == 2);
  assert(result.last_completed_stage == 1);
  assert(result.n_cand_outcomes == 2);
  for (int outcome_idx = 0; outcome_idx < result.n_cand_outcomes;
       outcome_idx++) {
    assert(result.cand_outcomes[outcome_idx].fidelity == 2);
  }

  peg_result_destroy(&result);
  peg_poll_destroy(poll);
  validated_moves_destroy(moves);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

// Exercises PegArgs.inner_top_k. On a 2-in-bag board (so non-emptying cands
// leave the opponent tiles to draw, where the model and its cap actually bite),
// solve one candidate pessimistically with the cap off (k=0, the unbounded
// worst-case opponent) and on (k=1, the opponent weighs only its best-equity
// reply). Capping the opponent can only help the mover, so the capped win% must
// be >= the uncapped win%.
static void test_peg_main_inner_top_k(void) {
  const char *cgp =
      "cgp 5U4OHMIC/5N3WREATH/5T4FAX2/5i3B1V3/5N3L1E3/5G2VELDT2/5E3S5/"
      "5DREKS1F3/8YELL3/4ABASER1U3/4GYM3ZO3/WAITE5OR2J/10OI2A/"
      "3QUOIT1PINNER/4RENEGADE2P ACDIOT?/AIIIOSU 431/392 0 -lex CSW24";
  Config *config = config_create_or_die("set -threads 4 -s1 score -s2 score");
  load_and_exec_config_or_die(config, cgp);
  Game *game = config_get_game(config);
  ErrorStack *error_stack = error_stack_create();

  // Pick a one-tile play: with 2 in the bag the mover then draws only one, so
  // the opponent still has a tile to draw and every leaf is non-emptier — the
  // case where the pessimistic model and its inner_top_k cap actually run.
  MoveList *gen_ml = move_list_create(4096);
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
  assert(n_gen >= 1);
  const Move *cand = NULL;
  for (int i = 0; i < n_gen; i++) {
    const Move *m = move_list_get_move(gen_ml, i);
    if (move_get_tiles_played(m) == 1) {
      cand = m;
      break;
    }
  }
  assert(cand != NULL);
  const Move *only[1] = {cand};

  PegArgs base;
  memset(&base, 0, sizeof(base));
  base.game = game;
  base.thread_control = config_get_thread_control(config);
  base.num_threads = 4;
  base.time_budget_seconds = 30.0;
  base.opp_model = PEG_OPP_PESSIMISTIC;
  base.only_moves = only;
  base.n_only_moves = 1;

  PegResult uncapped;
  PegResult capped;
  memset(&uncapped, 0, sizeof(uncapped));
  memset(&capped, 0, sizeof(capped));

  base.inner_top_k = 0; // unbounded worst-case opponent
  peg_solve(&base, &uncapped, error_stack);
  assert(error_stack_is_empty(error_stack));

  base.inner_top_k = 1; // opponent weighs only its best-equity reply
  peg_solve(&base, &capped, error_stack);
  assert(error_stack_is_empty(error_stack));

  assert(uncapped.n_top_cands == 1 && capped.n_top_cands == 1);
  const double win_uncapped = uncapped.top_cands[0].win_pct;
  const double win_capped = capped.top_cands[0].win_pct;
  const double spread_uncapped = uncapped.top_cands[0].mean_spread;
  const double spread_capped = capped.top_cands[0].mean_spread;
  // Capping the opponent can only help (or not change) the mover, on both the
  // win bucket and the more sensitive spread.
  assert(win_capped >= win_uncapped - 0.0005);
  assert(spread_capped >= spread_uncapped - 0.0005);
  printf("[peg_inner_top_k] pessimistic win %.4f->%.4f spread %.3f->%.3f "
         "(uncapped->capped k=1)\n",
         win_uncapped, win_capped, spread_uncapped, spread_capped);

  peg_result_destroy(&uncapped);
  peg_result_destroy(&capped);
  move_list_destroy(gen_ml);
  error_stack_destroy(error_stack);
  config_destroy(config);
}

// ---------------------------------------------------------------------------
// On-demand deep anchors (test.c keys peg1pb / peg1onyx / peg2axe / peg2acid /
// peg3pah / peg4pond): production peg_solve regression pins on studied boards.
//
// Each runs the cascade (full candidate generation, full scenario enumeration,
// default halving schedule capped by max_stage) with the studied move
// pnoprune-protected (PegArgs.protect_moves) so it is carried to the deepest
// stage and scored even when production ranks another move higher — then pins
// THAT move's win%/spread. These are production's own deep values, not
// macondo's guaranteed-win numbers (production's non-emptier leaves are a
// shallow rational playout); the macondo/GillesB move values are cross-checked
// separately in the fast main-suite macondo cases and in peg_pess_test.
//
// macondo CGPs that store an empty opponent rack load the whole unseen pool
// into the bag; redistribute_bag > 0 first repacks it to an N-in-bag position
// (deterministically: smallest N tiles to the bag, the rest to the opponent).
// ---------------------------------------------------------------------------
static void peg_anchor_protected_move(
    const char *name, const char *cgp, const char *move_str,
    int redistribute_bag, PegOppModel opp_model, int inner_top_k,
    int scenario_stride, int max_stage, double budget_seconds,
    double expected_win, double expected_spread, double win_tol,
    double spread_tol) {
  Config *config = config_create_or_die("set -threads 8 -s1 score -s2 score");
  load_and_exec_config_or_die(config, cgp);
  Game *game = config_get_game(config);
  const int mover_idx = game_get_player_on_turn_index(game);
  const Board *board = game_get_board(game);
  const LetterDistribution *ld = game_get_ld(game);
  ErrorStack *error_stack = error_stack_create();

  if (redistribute_bag > 0) {
    peg_redistribute_bag_to_opp(game, mover_idx, redistribute_bag);
  }

  // Find the studied move by its rendered text rather than parsing a move
  // string — robust to playthrough/coordinate input quirks (e.g. a move that
  // opens with a played-through tile like "6F (A)X(E)"). The Move lives in
  // gen_ml, which outlives the solve below.
  MoveList *gen_ml = move_list_create(4096);
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
  const Move *studied = NULL;
  StringBuilder *find_sb = string_builder_create();
  for (int i = 0; i < n_gen && studied == NULL; i++) {
    const Move *m = move_list_get_move(gen_ml, i);
    string_builder_clear(find_sb);
    string_builder_add_move(find_sb, board, m, ld, false);
    if (strings_equal(string_builder_peek(find_sb), move_str)) {
      studied = m;
    }
  }
  string_builder_destroy(find_sb);
  if (studied == NULL) {
    log_fatal("[%s] studied move '%s' not generated on this board", name,
              move_str);
  }
  const Move *protect[1] = {studied};

  PegArgs args;
  memset(&args, 0, sizeof(args));
  args.game = game;
  args.thread_control = config_get_thread_control(config);
  args.num_threads = 8;
  args.time_budget_seconds = budget_seconds;
  args.max_stage = max_stage;
  args.opp_model = opp_model;
  args.inner_top_k = inner_top_k;
  args.scenario_stride = scenario_stride;
  args.protect_moves = protect;
  args.n_protect_moves = 1;

  PegResult result;
  memset(&result, 0, sizeof(result));
  peg_solve(&args, &result, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert(result.n_top_cands >= 1);

  // The protected studied move is guaranteed to survive every cut; find it in
  // the ranking and read its evaluated win%/spread.
  StringBuilder *studied_sb = string_builder_create();
  string_builder_add_move(studied_sb, board, studied, ld, false);
  const char *studied_text = string_builder_peek(studied_sb);
  double win = -1.0;
  double spread = 0.0;
  StringBuilder *cand_sb = string_builder_create();
  for (int i = 0; i < result.n_top_cands; i++) {
    string_builder_clear(cand_sb);
    string_builder_add_move(cand_sb, board, &result.top_cands[i].move, ld,
                            false);
    if (strings_equal(string_builder_peek(cand_sb), studied_text)) {
      win = result.top_cands[i].win_pct;
      spread = result.top_cands[i].mean_spread;
      break;
    }
  }
  StringBuilder *best_sb = string_builder_create();
  string_builder_add_move(best_sb, board, &result.best_move, ld, false);
  printf("[%s] %s: win=%.4f spread=%+.3f (field best %s, stage %d, %.1fs)\n",
         name, studied_text, win, spread, string_builder_peek(best_sb),
         result.last_completed_stage, ctimer_elapsed_seconds(&result.timer));
  if (win < 0.0) {
    log_fatal("[%s] protected move '%s' missing from ranking", name,
              studied_text);
  }
  const double win_diff = win - expected_win;
  const double abs_win_diff = win_diff < 0.0 ? -win_diff : win_diff;
  if (abs_win_diff > win_tol) {
    log_fatal("[%s] win %.4f outside tolerance %.4f of expected %.4f", name,
              win, win_tol, expected_win);
  }
  const double spread_diff = spread - expected_spread;
  const double abs_spread_diff = spread_diff < 0.0 ? -spread_diff : spread_diff;
  if (abs_spread_diff > spread_tol) {
    log_fatal("[%s] spread %.3f outside tolerance %.3f of expected %.3f", name,
              spread, spread_tol, expected_spread);
  }

  string_builder_destroy(studied_sb);
  string_builder_destroy(cand_sb);
  string_builder_destroy(best_sb);
  move_list_destroy(gen_ml);
  error_stack_destroy(error_stack);
  peg_result_destroy(&result);
  config_destroy(config);
}

// 1-in-bag: engineered position where passing is strictly best (mover and opp
// both hold the same bingo rack with a Q stranded in the bag; any bingo opens
// an opp counter-bingo, while passing wins every one-tile-bag scenario).
void test_peg_1bag_pass_best(void) {
  const char *cgp =
      "cgp 7C6D/7H4LAR/7I2P1ALA/7VOGUE1AG/6RERAN2M1/7S1BY2O1/8OY2Id1/"
      "5JEUX3NEW/3C1U2O3A1E/3O1M6N1B/3ZIP2OAK1E2/2TI1sTIFLERS2/2WED5F1T2/"
      "1HIDEOUT7/VEG1N2IDOL4 AEINRST/AEINRST 372/369 0 -lex CSW24";
  peg_anchor_protected_move("peg1pb", cgp, "pass", /*redistribute_bag=*/0,
                            PEG_OPP_RATIONAL, /*inner_top_k=*/0,
                            /*scenario_stride=*/1, /*max_stage=*/0,
                            /*budget_seconds=*/120.0, /*expected_win=*/1.0,
                            /*expected_spread=*/51.375, /*win_tol=*/0.005,
                            /*spread_tol=*/8.0);
}

// 1-in-bag: macondo TestStraightforward1PEG board (studied play 13L ONYX,
// 7.5/8 = 0.9375 in macondo); here we pin production's value for ONYX.
void test_peg_1bag_onyx(void) {
  const char *cgp =
      "cgp 15/3Q7U3/3U2TAURINE2/1CHANSONS2W3/2AI6JO3/DIRL1PO3IN3/E1D2EF3V4/"
      "F1I2p1TRAIK3/O1L2T4E4/ABy1PIT2BRIG2/ME1MOZELLE5/1GRADE1O1NOH3/"
      "WE3R1V7/AT5E7/G6D7 ENOSTXY/ACEISUY 356/378 0 -lex NWL20";
  peg_anchor_protected_move("peg1onyx", cgp, "13L ONYX", /*redistribute_bag=*/0,
                            PEG_OPP_RATIONAL, /*inner_top_k=*/0,
                            /*scenario_stride=*/1, /*max_stage=*/0,
                            /*budget_seconds=*/120.0, /*expected_win=*/0.9375,
                            /*expected_spread=*/8.75, /*win_tol=*/0.005,
                            /*spread_tol=*/5.0);
}

// 2-in-bag: macondo TestTwoInBagSingleMove board (studied play 6F (A)X(E),
// 70/72 = 0.9722 in macondo); pin production's value for (A)X(E).
void test_peg_2bag_axe(void) {
  const char *cgp =
      "cgp 1T13/1W3Q9/VERB1U9/1E1OPIUM5C1/1LAWIN1I5O1/1Y3A1E5R1/7V4NO1/"
      "NOTArIZE1C2UN1/6ODAH2LA1/3TAHA2I2LED/2JUT4R2A1O/3G5P4D/3R3BrIEFING/"
      "3I5L4E/3K2DESYNES1M AEFGSTX/EEIOOST 370/341 0 -lex CSW21";
  peg_anchor_protected_move("peg2axe", cgp, "6F (A)X(E)",
                            /*redistribute_bag=*/0, PEG_OPP_RATIONAL,
                            /*inner_top_k=*/0, /*scenario_stride=*/1,
                            /*max_stage=*/0, /*budget_seconds=*/120.0,
                            /*expected_win=*/0.9722, /*expected_spread=*/37.431,
                            /*win_tol=*/0.005, /*spread_tol=*/8.0);
}

// 2-in-bag: GillesB CSW24 board (studied play C6 ACIDOT(I)c, 72/72 in
// GillesB's solver); pin production's value for ACIDOT(I)c.
void test_peg_2bag_acidotic(void) {
  const char *cgp =
      "cgp 5U4OHMIC/5N3WREATH/5T4FAX2/5i3B1V3/5N3L1E3/5G2VELDT2/5E3S5/"
      "5DREKS1F3/8YELL3/4ABASER1U3/4GYM3ZO3/WAITE5OR2J/10OI2A/"
      "3QUOIT1PINNER/4RENEGADE2P ACDIOT?/AIIIOSU 431/392 0 -lex CSW24";
  peg_anchor_protected_move("peg2acid", cgp, "C6 ACIDOT(I)c",
                            /*redistribute_bag=*/0, PEG_OPP_RATIONAL,
                            /*inner_top_k=*/0, /*scenario_stride=*/1,
                            /*max_stage=*/0, /*budget_seconds=*/120.0,
                            /*expected_win=*/1.0, /*expected_spread=*/112.444,
                            /*win_tol=*/0.005, /*spread_tol=*/10.0);
}

// 3-in-bag: macondo manual position #2 (Tunnicliffe v Brennan, CSW21); studied
// play 13M P(AH). max_stage bounds the 3-bag scenario space.
void test_peg_3bag_pah(void) {
  const char *cgp =
      "cgp BEDEL10/R1R9U2/O1IT1Q5OM2/W1BIDI4YUM2/N2XI5AT3/E3G4T1R3/"
      "S1VOILE2OKA3/T3T1DISPACED1/9AWE1O1/9Z1s1FA/14R/13GO/13AH/"
      "3JUVIE4UTA/INRO3FLENCHES ?ANNOPY/AEGILNS 344/368 0 -lex CSW21";
  peg_anchor_protected_move("peg3pah", cgp, "13M P(AH)", /*redistribute_bag=*/0,
                            PEG_OPP_RATIONAL, /*inner_top_k=*/0,
                            /*scenario_stride=*/1, /*max_stage=*/2,
                            /*budget_seconds=*/240.0, /*expected_win=*/0.6625,
                            /*expected_spread=*/21.532, /*win_tol=*/0.06,
                            /*spread_tol=*/12.0);
}

// 4-in-bag: macondo manual position #1 (Sokol v Walton); studied play
// 2L P(O)ND. The macondo CGP stores an empty opponent rack, so repack the
// unseen pool to a genuine 4-in-bag position first. max_stage bounds the
// 4-bag scenario space.
void test_peg_4bag_pond(void) {
  const char *cgp =
      "cgp 12D2/1U10O2/1p10L2/1R1C3KANJIS2/1I1O3A2U4/1G1T3I2I4/1H1E3Z2C1LOO/"
      "1TED3E1BYWORD/2Q4N3AXE1/1RuBIGOS3I3/F1A5WEAVE2/O1T8E3/V1E5LOURY2/"
      "ENSNARL2HM4/A6TEMP4 DEFNNPT/ 394/365 0 -lex NWL20";
  peg_anchor_protected_move("peg4pond", cgp, "2L P(O)ND",
                            /*redistribute_bag=*/4, PEG_OPP_RATIONAL,
                            /*inner_top_k=*/0, /*scenario_stride=*/1,
                            /*max_stage=*/2, /*budget_seconds=*/240.0,
                            /*expected_win=*/1.0, /*expected_spread=*/46.892,
                            /*win_tol=*/0.01, /*spread_tol=*/10.0);
}

// 3-in-bag pessimistic variant of peg3pah: same P(AH) candidate scored under
// the worst-case opponent model, but bounded so it stays tractable — the
// opponent weighs only its inner_top_k highest-equity replies, and the bag
// orderings are weight-stratified down to ~1/scenario_stride. Pins production's
// bounded-pessimistic value for P(AH) as a regression anchor.
void test_peg_3bag_pah_pessimistic(void) {
  const char *cgp =
      "cgp BEDEL10/R1R9U2/O1IT1Q5OM2/W1BIDI4YUM2/N2XI5AT3/E3G4T1R3/"
      "S1VOILE2OKA3/T3T1DISPACED1/9AWE1O1/9Z1s1FA/14R/13GO/13AH/"
      "3JUVIE4UTA/INRO3FLENCHES ?ANNOPY/AEGILNS 344/368 0 -lex CSW21";
  peg_anchor_protected_move("peg3pahpess", cgp, "13M P(AH)",
                            /*redistribute_bag=*/0, PEG_OPP_PESSIMISTIC,
                            /*inner_top_k=*/8, /*scenario_stride=*/7,
                            /*max_stage=*/2, /*budget_seconds=*/300.0,
                            /*expected_win=*/0.1765,
                            /*expected_spread=*/-17.216,
                            /*win_tol=*/0.05, /*spread_tol=*/12.0);
}

// 4-in-bag pessimistic variant of peg4pond: same bounded-pessimistic treatment
// (top-8 opponent replies, ~1/7 bag orderings) on the empty-opp pond board
// (repacked to 4-in-bag). Pins production's bounded-pessimistic value for
// P(O)ND.
void test_peg_4bag_pond_pessimistic(void) {
  const char *cgp =
      "cgp 12D2/1U10O2/1p10L2/1R1C3KANJIS2/1I1O3A2U4/1G1T3I2I4/1H1E3Z2C1LOO/"
      "1TED3E1BYWORD/2Q4N3AXE1/1RuBIGOS3I3/F1A5WEAVE2/O1T8E3/V1E5LOURY2/"
      "ENSNARL2HM4/A6TEMP4 DEFNNPT/ 394/365 0 -lex NWL20";
  peg_anchor_protected_move("peg4pondpess", cgp, "2L P(O)ND",
                            /*redistribute_bag=*/4, PEG_OPP_PESSIMISTIC,
                            /*inner_top_k=*/8, /*scenario_stride=*/7,
                            /*max_stage=*/2, /*budget_seconds=*/240.0,
                            /*expected_win=*/0.9876, /*expected_spread=*/31.082,
                            /*win_tol=*/0.02, /*spread_tol=*/12.0);
}

// Verify that peg_solve accepts a position without returning a
// bag-out-of-range error. The best-equity move is used as the single
// candidate so no expensive move search or multi-stage cascade runs.
static void peg_assert_bag_accepted(const char *name, const char *cgp) {
  Config *config = config_create_or_die("set -s1 score -s2 score");
  load_and_exec_config_or_die(config, cgp);
  Game *game = config_get_game(config);

  MoveList *gen_ml = move_list_create(1);
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
  assert(move_list_get_count(gen_ml) == 1);
  const Move *only_moves[1] = {move_list_get_move(gen_ml, 0)};

  ErrorStack *error_stack = error_stack_create();
  PegArgs args;
  memset(&args, 0, sizeof(args));
  args.game = game;
  args.thread_control = config_get_thread_control(config);
  args.num_threads = 1;
  args.only_moves = only_moves;
  args.n_only_moves = 1;

  PegResult result;
  memset(&result, 0, sizeof(result));
  peg_solve(&args, &result, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    char *err = error_stack_get_string_and_reset(error_stack);
    log_fatal("[%s] peg_solve unexpectedly failed: %s", name, err);
  }
  assert(result.n_top_cands == 1);
  assert(result.top_cands[0].win_pct >= 0.0 &&
         result.top_cands[0].win_pct <= 1.0);
  printf("[%s] accepted: win=%.4f spread=%+.3f\n", name,
         result.top_cands[0].win_pct, result.top_cands[0].mean_spread);

  peg_result_destroy(&result);
  error_stack_destroy(error_stack);
  move_list_destroy(gen_ml);
  config_destroy(config);
}

// Verifies the opp-rack bag adjustment fix for the direct peg_solve API.
// Two variants of the 1-in-bag ONYX position are tested:
//   - Empty opp rack: all opp tiles hidden in the game bag (raw bag = 8,
//     effective = 1 after subtracting the 7 unknown opp tiles).
//   - Partial opp rack (3 tiles): 4 opp tiles hidden in the game bag (raw
//     bag = 5, effective = 1 after subtracting 4 unknown tiles).
// Both should be accepted as 1-in-bag positions; previously both failed with
// a bag-out-of-range error.
static void test_peg_opp_rack_sizes(void) {
  // Empty opp rack: raw bag = 7 (opp) + 1 (real) = 8; effective = 1.
  peg_assert_bag_accepted(
      "peg_opp_empty",
      "cgp 15/3Q7U3/3U2TAURINE2/1CHANSONS2W3/2AI6JO3/DIRL1PO3IN3/E1D2EF3V4/"
      "F1I2p1TRAIK3/O1L2T4E4/ABy1PIT2BRIG2/ME1MOZELLE5/1GRADE1O1NOH3/"
      "WE3R1V7/AT5E7/G6D7 ENOSTXY/ 356/378 0 -lex NWL20");
  // Partial opp rack (3 of 7 tiles known): raw bag = 4 (opp) + 1 (real) = 5;
  // effective = 1. Opp originally held ACEISUY; keeping ACE and leaving ISUY
  // as unknown tiles in the bag exercises the (RACK_SIZE - opp_rack_size)
  // subtraction with a non-zero starting count.
  peg_assert_bag_accepted(
      "peg_opp_partial",
      "cgp 15/3Q7U3/3U2TAURINE2/1CHANSONS2W3/2AI6JO3/DIRL1PO3IN3/E1D2EF3V4/"
      "F1I2p1TRAIK3/O1L2T4E4/ABy1PIT2BRIG2/ME1MOZELLE5/1GRADE1O1NOH3/"
      "WE3R1V7/AT5E7/G6D7 ENOSTXY/ACE 356/378 0 -lex NWL20");
}

// Verifies the opp-rack bag adjustment fix through the CLI command path
// (config / cgp / set -pegonly / peg), exercising both the empty and partial
// opp-rack cases on the same 1-in-bag ONYX board.
static void test_peg_opp_rack_sizes_cli(void) {
  Config *config = config_create_or_die("set -s1 score -s2 score");

  // Empty opp rack: the game bag holds all 7 opp tiles plus the 1 real bag
  // tile; peg should subtract the 7 unknown opp tiles and see a 1-in-bag
  // position.
  load_and_exec_config_or_die(
      config,
      "cgp 15/3Q7U3/3U2TAURINE2/1CHANSONS2W3/2AI6JO3/DIRL1PO3IN3/E1D2EF3V4/"
      "F1I2p1TRAIK3/O1L2T4E4/ABy1PIT2BRIG2/ME1MOZELLE5/1GRADE1O1NOH3/"
      "WE3R1V7/AT5E7/G6D7 ENOSTXY/ 356/378 0 -lex NWL20");
  load_and_exec_config_or_die(config, "set -pegonly 13L.ONYX");
  load_and_exec_config_or_die(config, "peg");
  assert(config_get_peg_result(config)->last_completed_stage >= 0);
  printf("[peg_cli_opp_empty] accepted: win=%.4f\n",
         config_get_peg_result(config)->best_win);

  // Partial opp rack (3 tiles known): the game bag holds 4 unknown opp tiles
  // plus the 1 real bag tile; peg should subtract (RACK_SIZE - 3) = 4 and
  // see a 1-in-bag position.
  load_and_exec_config_or_die(
      config,
      "cgp 15/3Q7U3/3U2TAURINE2/1CHANSONS2W3/2AI6JO3/DIRL1PO3IN3/E1D2EF3V4/"
      "F1I2p1TRAIK3/O1L2T4E4/ABy1PIT2BRIG2/ME1MOZELLE5/1GRADE1O1NOH3/"
      "WE3R1V7/AT5E7/G6D7 ENOSTXY/ACE 356/378 0 -lex NWL20");
  load_and_exec_config_or_die(config, "set -pegonly 13L.ONYX");
  load_and_exec_config_or_die(config, "peg");
  assert(config_get_peg_result(config)->last_completed_stage >= 0);
  printf("[peg_cli_opp_partial] accepted: win=%.4f\n",
         config_get_peg_result(config)->best_win);

  config_destroy(config);
}

// Feed synthetic per-ordering rows to the outcomes-cell renderer and assert the
// compact string. Decouples the rendering from a live solve.
static void assert_outcomes_eq(const PegPerScenario *rows, int n_rows,
                               const char *expected) {
  char *got = peg_build_outcomes_string_rows(rows, n_rows);
  if (strcmp(got, expected) != 0) {
    printf("[peg_outcomes] expected '%s' got '%s'\n", expected, got);
  }
  assert(strcmp(got, expected) == 0);
  free(got);
}

static void test_peg_outcomes_string(void) {
  // A 2-tile play (drawn pair + one bag tile left): the mover draws its two
  // tiles together, so every draw must lead with the 2-tile multiset (DH/A, not
  // D/H/A). Draws with the same total tiles but a different drawn pair stay
  // distinct (DH/R and DR/H, never a merged D/HR). Three wins, three losses, so
  // the shorter (here equal -> win) list is the one shown.
  const PegPerScenario two_tile[] = {
      {.drawn = "DH", .remaining = "A", .weight = 2, .mover_total = 5},
      {.drawn = "DH", .remaining = "R", .weight = 2, .mover_total = 5},
      {.drawn = "DR", .remaining = "H", .weight = 2, .mover_total = 5},
      {.drawn = "DR", .remaining = "A", .weight = 2, .mover_total = -5},
      {.drawn = "FR", .remaining = "A", .weight = 2, .mover_total = -5},
      {.drawn = "FR", .remaining = "D", .weight = 2, .mover_total = -5},
  };
  assert_outcomes_eq(two_tile, 6, "W: DH/Ax2 DH/Rx2 DR/Hx2");

  // A play that draws the whole bag (no remainder): the draw is a bare
  // multiset, no slash.
  const PegPerScenario no_remainder[] = {
      {.drawn = "AD", .remaining = "", .weight = 4, .mover_total = 5},
      {.drawn = "EI", .remaining = "", .weight = 4, .mover_total = -5},
  };
  assert_outcomes_eq(no_remainder, 2, "W: ADx4");

  // A 1-tile draw with a 2-tile bag remainder. When both remainder orderings
  // share a bucket the remainder collapses to a multiset behind the prefix
  // (X/YZ); the weights of its orderings sum into the one token.
  const PegPerScenario rem_multiset[] = {
      {.drawn = "X", .remaining = "YZ", .weight = 3, .mover_total = 5},
      {.drawn = "X", .remaining = "ZY", .weight = 3, .mover_total = 5},
      {.drawn = "X", .remaining = "AB", .weight = 3, .mover_total = -5},
      {.drawn = "X", .remaining = "BA", .weight = 3, .mover_total = -5},
  };
  assert_outcomes_eq(rem_multiset, 4, "W: X/YZx6");

  // Same prefix, but the remainder orderings split across buckets, so the
  // remainder is "/"-segmented behind the drawn prefix (X/Y/Z).
  const PegPerScenario rem_split[] = {
      {.drawn = "X", .remaining = "YZ", .weight = 4, .mover_total = 5},
      {.drawn = "X", .remaining = "ZY", .weight = 4, .mover_total = -5},
  };
  assert_outcomes_eq(rem_split, 2, "W: X/Y/Zx4");

  // A tie draw with no losing draw (the ONYX case from the bug report). The old
  // "shorter list wins" logic picked the empty loss list and rendered a bare
  // "L:". Now wins (the largest list) is left implied and the tie is shown.
  // Because the tie list is the only one shown (losses is empty), "T: E" alone
  // can't say whether the implied majority is wins or losses, so it is named.
  const PegPerScenario tie_no_loss[] = {
      {.drawn = "A", .remaining = "", .weight = 1, .mover_total = 5},
      {.drawn = "B", .remaining = "", .weight = 1, .mover_total = 5},
      {.drawn = "E", .remaining = "", .weight = 1, .mover_total = 0},
  };
  assert_outcomes_eq(tie_no_loss, 3, "T: E, otherwise wins");

  // A tie draw with no winning draw: losses are the largest list, left implied,
  // and the tie is the only list shown, so the implied loss is named.
  const PegPerScenario tie_no_win[] = {
      {.drawn = "P", .remaining = "", .weight = 1, .mover_total = -5},
      {.drawn = "Q", .remaining = "", .weight = 1, .mover_total = -5},
      {.drawn = "X", .remaining = "", .weight = 1, .mover_total = 0},
  };
  assert_outcomes_eq(tie_no_win, 3, "T: X, otherwise loses");

  // All three buckets: the largest list (here losses) is implied; the two
  // shorter lists -- wins and the tie -- are printed comma-separated, in
  // wins/ties/loss order.
  const PegPerScenario win_tie_loss[] = {
      {.drawn = "C", .remaining = "", .weight = 1, .mover_total = 5},
      {.drawn = "U", .remaining = "", .weight = 1, .mover_total = 5},
      {.drawn = "Y", .remaining = "", .weight = 1, .mover_total = 5},
      {.drawn = "D", .remaining = "", .weight = 1, .mover_total = -5},
      {.drawn = "F", .remaining = "", .weight = 1, .mover_total = -5},
      {.drawn = "G", .remaining = "", .weight = 1, .mover_total = -5},
      {.drawn = "H", .remaining = "", .weight = 1, .mover_total = -5},
      {.drawn = "E", .remaining = "", .weight = 1, .mover_total = 0},
  };
  assert_outcomes_eq(win_tie_loss, 8, "W: C U Y, T: E");

  // Ties are the most common result: the tie list is the largest, so it is the
  // one left implied. A tie majority is rare, so it is named explicitly with a
  // trailing ", otherwise ties" after the shorter win / loss lists.
  const PegPerScenario tie_majority[] = {
      {.drawn = "A", .remaining = "", .weight = 1, .mover_total = 5},
      {.drawn = "B", .remaining = "", .weight = 1, .mover_total = -5},
      {.drawn = "C", .remaining = "", .weight = 1, .mover_total = 0},
      {.drawn = "D", .remaining = "", .weight = 1, .mover_total = 0},
      {.drawn = "E", .remaining = "", .weight = 1, .mover_total = 0},
  };
  assert_outcomes_eq(tie_majority, 5, "W: A, L: B, otherwise ties");
}

void test_peg(void) {
  log_set_level(LOG_FATAL);
  test_peg_outcomes_string();
  test_peg_main_1bag_pass();
  test_peg_main_2bag_single();
  test_peg_main_3bag_single();
  test_peg_main_4bag_single();
  test_peg_main_opp_models();
  test_peg_main_pnoprune();
  test_peg_main_progress_detail();
  test_peg_discarded_stage_outcomes();
  test_peg_main_inner_top_k();
  // Cases drawn from the macondo codebase + pre-endgame manual.
  test_peg_macondo_only_onyx();
  test_peg_macondo_french_pass();
  test_peg_macondo_axe();
  test_peg_macondo_pah_slice();
  test_peg_macondo_pond_slice();
  test_peg_main_cli();
  // Opp-rack bag adjustment fix: empty and partial opp racks.
  test_peg_opp_rack_sizes();
  test_peg_opp_rack_sizes_cli();
}
