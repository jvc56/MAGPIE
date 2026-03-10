#include "peg_fw_test.h"

#include "../src/ent/game.h"
#include "../src/ent/move.h"
#include "../src/impl/config.h"
#include "../src/impl/peg.h"
#include "peg_test_util.h"
#include "test_util.h"

#include "../src/compat/ctime.h"

#include <assert.h>
#include <string.h>

// ONYX CGP: 1-in-bag, NWL20. Best move is 13L ONYX at 93.75%.
#define ONYX_CGP                                                               \
  "cgp 15/3Q7U3/3U2TAURINE2/1CHANSONS2W3/2AI6JO3/DIRL1PO3IN3/"              \
  "E1D2EF3V4/F1I2p1TRAIK3/O1L2T4E4/ABy1PIT2BRIG2/ME1MOZELLE5/"              \
  "1GRADE1O1NOH3/WE3R1V7/AT5E7/G6D7 ENOSTXY/ACEISUY 356/378 0 -lex "        \
  "NWL20"

// French pass CGP: 1-in-bag, FRA20. Best move is pass at ~68.75%.
#define FRENCH_PASS_CGP                                                        \
  "cgp 11ONZE/10J2O1/8A1E1DO1/7QUETEE1H/10E1F1U/8ECUMERA/8C1R1TIR/"        \
  "7WOKS2ET/6DUR6/5G2N1M4/4HALLALiS3/1G1P1P1OM1XI3/VIVONS1BETEL3/"          \
  "IF1N3AS1RYAL1/ETUDIAIS7 AEINRST/ 301/300 0 -lex FRA20 -ld french"

// Helper: run peg_solve with a given first_win_mode and return the result.
// If elapsed_out is non-NULL, stores wall-clock seconds there.
// If quiet is true, suppresses board / per-pass output.
static PegResult run_peg_fw_ex(const char *cgp, peg_first_win_mode_t mode,
                               bool spread_all_final, int num_stages,
                               const int *stage_limits, int num_limits,
                               bool quiet, double *elapsed_out) {
  Config *config =
      config_create_or_die("set -wmp false -s1 score -s2 score -r1 small -r2 small");
  load_and_exec_config_or_die(config, cgp);

  Game *game = config_get_game(config);
  if (!quiet)
    peg_test_print_game_position(game);

  PegSolver *solver = peg_solver_create();
  PegArgs args = {
      .game = game,
      .thread_control = config_get_thread_control(config),
      .time_budget_seconds = 0.0,
      .num_threads = 8,
      .tt_fraction_of_mem = 0.5,
      .dual_lexicon_mode = DUAL_LEXICON_MODE_IGNORANT,
      .num_stages = num_stages,
      .early_cutoff = true,
      .per_pass_callback = quiet ? NULL : peg_test_progress_callback,
      .first_win_mode = mode,
      .first_win_spread_all_final = spread_all_final,
  };
  for (int i = 0; i < num_limits && i < PEG_MAX_STAGES; i++)
    args.stage_candidate_limits[i] = stage_limits[i];

  PegResult result;
  ErrorStack *error_stack = error_stack_create();

  Timer timer;
  ctimer_start(&timer);
  peg_solve(solver, &args, &result, error_stack);
  ctimer_stop(&timer);

  if (elapsed_out)
    *elapsed_out = ctimer_elapsed_seconds(&timer);

  assert(error_stack_is_empty(error_stack));
  if (!quiet)
    peg_test_print_result(&result, game);

  peg_solver_destroy(solver);
  error_stack_destroy(error_stack);
  config_destroy(config);
  return result;
}

static PegResult run_peg_fw(const char *cgp, peg_first_win_mode_t mode,
                            bool spread_all_final, int num_stages,
                            const int *stage_limits, int num_limits) {
  return run_peg_fw_ex(cgp, mode, spread_all_final, num_stages,
                       stage_limits, num_limits, false, NULL);
}

// ---------------------------------------------------------------------------
// Mode 1: PRUNE_ONLY — first_win prunes losers, survivors get full spread.
// Uses ONYX position. Should find 13L ONYX with ~93.75% win AND known spread.
// ---------------------------------------------------------------------------
static void test_peg_fw_prune_only(void) {
  printf("\n=== PEG_FIRST_WIN_PRUNE_ONLY (ONYX) ===\n");
  int limits[] = {24, 10};
  PegResult r = run_peg_fw(ONYX_CGP, PEG_FIRST_WIN_PRUNE_ONLY, false, 3,
                           limits, 2);
  assert(r.stages_completed == 3);
  {
    StringBuilder *sb = string_builder_create();
    string_builder_add_formatted_string(sb, "best_wp=%.3f spread_known=%d",
                                        r.best_win_pct, r.spread_known);
    printf("  %s\n", string_builder_peek(sb));
    string_builder_destroy(sb);
  }
  assert(r.best_win_pct > 0.93 && r.best_win_pct < 0.94);
  assert(r.spread_known);
  assert(r.best_expected_spread > 0.0);
}

// ---------------------------------------------------------------------------
// Mode 2: WIN_PCT_THEN_SPREAD (3a) — first_win on all stages, spread only
// for tied candidates on final stage.
// ---------------------------------------------------------------------------
static void test_peg_fw_winpct_then_spread_3a(void) {
  printf("\n=== PEG_FIRST_WIN_WIN_PCT_THEN_SPREAD mode 3a (ONYX) ===\n");
  int limits[] = {24, 10};
  PegResult r = run_peg_fw(ONYX_CGP, PEG_FIRST_WIN_WIN_PCT_THEN_SPREAD,
                           false, 3, limits, 2);
  assert(r.stages_completed == 3);
  assert(r.best_win_pct > 0.93 && r.best_win_pct < 0.94);
}

// ---------------------------------------------------------------------------
// Mode 2: WIN_PCT_THEN_SPREAD (3b) — spread for all final-stage survivors.
// ---------------------------------------------------------------------------
static void test_peg_fw_winpct_then_spread_3b(void) {
  printf("\n=== PEG_FIRST_WIN_WIN_PCT_THEN_SPREAD mode 3b (ONYX) ===\n");
  int limits[] = {24, 10};
  PegResult r = run_peg_fw(ONYX_CGP, PEG_FIRST_WIN_WIN_PCT_THEN_SPREAD,
                           true, 3, limits, 2);
  assert(r.stages_completed == 3);
  assert(r.best_win_pct > 0.93 && r.best_win_pct < 0.94);
  assert(r.spread_known);
  assert(r.best_expected_spread > 0.0);
}

// ---------------------------------------------------------------------------
// Mode 3: WIN_PCT_THEN_SPREAD_ALL — first_win on all stages except the last.
// ---------------------------------------------------------------------------
static void test_peg_fw_winpct_then_spread_all(void) {
  printf("\n=== PEG_FIRST_WIN_WIN_PCT_THEN_SPREAD_ALL (ONYX) ===\n");
  int limits[] = {24, 10};
  PegResult r = run_peg_fw(ONYX_CGP, PEG_FIRST_WIN_WIN_PCT_THEN_SPREAD_ALL,
                           false, 3, limits, 2);
  assert(r.stages_completed == 3);
  assert(r.best_win_pct > 0.93 && r.best_win_pct < 0.94);
  assert(r.spread_known);
  assert(r.best_expected_spread > 0.0);
}

// ---------------------------------------------------------------------------
// Mode 4: WIN_PCT_ONLY — first_win on all stages, never compute spread.
// ---------------------------------------------------------------------------
static void test_peg_fw_winpct_only(void) {
  printf("\n=== PEG_FIRST_WIN_WIN_PCT_ONLY (ONYX) ===\n");
  int limits[] = {24, 10};
  PegResult r = run_peg_fw(ONYX_CGP, PEG_FIRST_WIN_WIN_PCT_ONLY,
                           false, 3, limits, 2);
  assert(r.stages_completed == 3);
  assert(r.best_win_pct > 0.93 && r.best_win_pct < 0.94);
  assert(!r.spread_known);
}

// ---------------------------------------------------------------------------
// French pass position with WIN_PCT_ONLY — verify pass still wins.
// ---------------------------------------------------------------------------
static void test_peg_fw_french_pass_winpct_only(void) {
  printf("\n=== PEG_FIRST_WIN_WIN_PCT_ONLY (French pass) ===\n");
  int limits[] = {7};
  PegResult r = run_peg_fw(FRENCH_PASS_CGP, PEG_FIRST_WIN_WIN_PCT_ONLY,
                           false, 2, limits, 1);
  assert(r.stages_completed == 2);
  assert(move_get_type(&r.best_move) == GAME_EVENT_PASS);
  assert(r.best_win_pct > 0.68 && r.best_win_pct < 0.70);
  assert(!r.spread_known);
}

// ---------------------------------------------------------------------------
// Benchmark: all 6 modes × 2 positions, with timing summary.
// ---------------------------------------------------------------------------
void test_peg_fw_bench(void) {
  typedef struct {
    const char *label;
    peg_first_win_mode_t mode;
    bool spread_all_final;
  } ModeSpec;

  static const ModeSpec modes[] = {
      {"NEVER",                  PEG_FIRST_WIN_NEVER,                   false},
      {"PRUNE_ONLY",             PEG_FIRST_WIN_PRUNE_ONLY,              false},
      {"WINPCT_THEN_SPREAD(3a)", PEG_FIRST_WIN_WIN_PCT_THEN_SPREAD,     false},
      {"WINPCT_THEN_SPREAD(3b)", PEG_FIRST_WIN_WIN_PCT_THEN_SPREAD,     true},
      {"WINPCT_THEN_SPREAD_ALL", PEG_FIRST_WIN_WIN_PCT_THEN_SPREAD_ALL, false},
      {"WIN_PCT_ONLY",           PEG_FIRST_WIN_WIN_PCT_ONLY,            false},
  };

  typedef struct {
    const char *label;
    const char *cgp;
    int num_stages;
    int limits[PEG_MAX_STAGES];
    int num_limits;
  } PosSpec;

  static const PosSpec positions[] = {
      {"ONYX",   ONYX_CGP,        3, {24, 10}, 2},
      {"French", FRENCH_PASS_CGP, 2, {7},      1},
  };

  const int num_modes = (int)(sizeof(modes) / sizeof(modes[0]));
  const int num_positions = (int)(sizeof(positions) / sizeof(positions[0]));

  // Results storage
  double times[6][2];
  char move_strs[6][2][64];
  double win_pcts[6][2];
  double spreads[6][2];
  bool spread_knowns[6][2];

  for (int m = 0; m < num_modes; m++) {
    for (int p = 0; p < num_positions; p++) {
      printf("\n--- %s / %s ---\n", modes[m].label, positions[p].label);
      double elapsed;
      PegResult r = run_peg_fw_ex(
          positions[p].cgp, modes[m].mode, modes[m].spread_all_final,
          positions[p].num_stages, positions[p].limits,
          positions[p].num_limits, true, &elapsed);
      times[m][p] = elapsed;
      win_pcts[m][p] = r.best_win_pct;
      spreads[m][p] = r.best_expected_spread;
      spread_knowns[m][p] = r.spread_known;

      // Format best move string
      Config *config = config_create_or_die(
          "set -wmp false -s1 score -s2 score -r1 small -r2 small");
      load_and_exec_config_or_die(config, positions[p].cgp);
      Game *game = config_get_game(config);
      StringBuilder *sb = string_builder_create();
      string_builder_add_move(sb, game_get_board(game), &r.best_move,
                              game_get_ld(game), false);
      snprintf(move_strs[m][p], sizeof(move_strs[m][p]), "%s",
               string_builder_peek(sb));
      string_builder_destroy(sb);
      config_destroy(config);

      printf("  -> %s  win%%=%.1f%%  time=%.3fs\n",
             move_strs[m][p], r.best_win_pct * 100.0, elapsed);
    }
  }

  // Summary table
  printf("\n");
  printf("%-27s | %-18s %6s %8s %6s | %-18s %6s %8s %6s\n",
         "Mode", "ONYX best", "win%", "spread", "time",
         "French best", "win%", "spread", "time");
  printf("%-27s-+-%-18s-%6s-%8s-%6s-+-%-18s-%6s-%8s-%6s\n",
         "---------------------------", "------------------",
         "------", "--------", "------",
         "------------------", "------", "--------", "------");
  for (int m = 0; m < num_modes; m++) {
    char onyx_spread[16], french_spread[16];
    if (spread_knowns[m][0])
      snprintf(onyx_spread, sizeof(onyx_spread), "%+.2f", spreads[m][0]);
    else
      snprintf(onyx_spread, sizeof(onyx_spread), "n/a");
    if (spread_knowns[m][1])
      snprintf(french_spread, sizeof(french_spread), "%+.2f", spreads[m][1]);
    else
      snprintf(french_spread, sizeof(french_spread), "n/a");

    printf("%-27s | %-18s %5.1f%% %8s %5.1fs | %-18s %5.1f%% %8s %5.1fs\n",
           modes[m].label,
           move_strs[m][0], win_pcts[m][0] * 100.0, onyx_spread, times[m][0],
           move_strs[m][1], win_pcts[m][1] * 100.0, french_spread, times[m][1]);
  }
  printf("\n");
}

void test_peg_fw(void) {
  test_peg_fw_prune_only();
  test_peg_fw_winpct_then_spread_3a();
  test_peg_fw_winpct_then_spread_3b();
  test_peg_fw_winpct_then_spread_all();
  test_peg_fw_winpct_only();
  test_peg_fw_french_pass_winpct_only();
}
