#include "peg2_fw_test.h"

#include "../src/ent/game.h"
#include "../src/impl/config.h"
#include "../src/impl/peg.h"
#include "peg_test_util.h"
#include "test_util.h"

#include "../src/compat/ctime.h"

#include <assert.h>
#include <string.h>

// CSW21 2-in-bag CGP. Best move: 10I X(I) at 70/72 win% (~97.2%).
#define PEG2_CGP                                                               \
  "cgp 1T13/1W3Q9/VERB1U9/1E1OPIUM5C1/1LAWIN1I5O1/1Y3A1E5R1/"               \
  "7V4NO1/NOTArIZE1C2UN1/6ODAH2LA1/3TAHA2I2LED/2JUT4R2A1O/"                  \
  "3G5P4D/3R3BrIEFING/3I5L4E/3K2DESYNES1M AEFGSTX/EEIOOST "                  \
  "370/341 0 -lex CSW21"

// Helper: run peg_solve with a given first_win_mode and return the result.
static PegResult run_peg2_fw_ex(const char *cgp, peg_first_win_mode_t mode,
                                bool spread_all_final, int num_stages,
                                const int *stage_limits, int num_limits,
                                bool quiet, double *elapsed_out) {
  Config *config =
      config_create_or_die("set -s1 score -s2 score -r1 small -r2 small");
  load_and_exec_config_or_die(config, cgp);

  Game *game = config_get_game(config);
  if (!quiet)
    peg_test_print_game_position(game);

  static const char *allowlist[] = {"10I X(I)", "13A EXT(R)AS", "7L S(NO)T"};

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
      .inner_opp_multi_tile_limit = 8,
      .inner_opp_one_tile_limit = 8,
      .max_non_emptying = 3,
      .skip_phase_1b = false,
      .skip_root_pass = true,
      .per_pass_callback = quiet ? NULL : peg_test_progress_callback,
      .first_win_mode = mode,
      .first_win_spread_all_final = spread_all_final,
      .candidate_allowlist = allowlist,
      .candidate_allowlist_count = 3,
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

// ---------------------------------------------------------------------------
// Benchmark: all 6 modes on the 2-bag position, with timing summary.
// ---------------------------------------------------------------------------
void test_peg2_fw_bench(void) {
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
      {"2bag_CSW21", PEG2_CGP, 1, {0}, 0},
  };

  const int num_modes = (int)(sizeof(modes) / sizeof(modes[0]));

  // Print the position once at the start.
  {
    Config *config = config_create_or_die(
        "set -s1 score -s2 score -r1 small -r2 small");
    load_and_exec_config_or_die(config, PEG2_CGP);
    peg_test_print_game_position(config_get_game(config));
    config_destroy(config);
  }

  // Results storage
  double times[6];
  PegResult results[6];

  // Load game once for formatting move strings.
  Config *fmt_config = config_create_or_die(
      "set -s1 score -s2 score -r1 small -r2 small");
  load_and_exec_config_or_die(fmt_config, PEG2_CGP);
  Game *fmt_game = config_get_game(fmt_config);

  for (int m = 0; m < num_modes; m++) {
    printf("\n--- %s / %s ---\n", modes[m].label, positions[0].label);
    double elapsed;
    PegResult r = run_peg2_fw_ex(
        positions[0].cgp, modes[m].mode, modes[m].spread_all_final,
        positions[0].num_stages, positions[0].limits,
        positions[0].num_limits, true, &elapsed);
    times[m] = elapsed;
    results[m] = r;

    // Print all ranked candidates.
    for (int i = 0; i < r.num_ranked; i++) {
      StringBuilder *sb = string_builder_create();
      string_builder_add_move(sb, game_get_board(fmt_game),
                              &r.ranked[i].move, game_get_ld(fmt_game), false);
      char spread_str[16];
      if (r.ranked[i].spread_known)
        snprintf(spread_str, sizeof(spread_str), "%+.2f",
                 r.ranked[i].expected_spread);
      else
        snprintf(spread_str, sizeof(spread_str), "n/a");
      printf("  %d. %-18s  win%%=%.1f%%  spread=%-8s%s\n", i + 1,
             string_builder_peek(sb), r.ranked[i].win_pct * 100.0,
             spread_str, r.ranked[i].pruned ? "  (pruned)" : "");
      string_builder_destroy(sb);
    }
    printf("  time=%.3fs\n", elapsed);
  }

  // Summary table
  printf("\n");
  printf("%-27s | %-18s %6s %8s %6s\n",
         "Mode", "best move", "win%", "spread", "time");
  printf("%-27s-+-%-18s-%6s-%8s-%6s\n",
         "---------------------------", "------------------",
         "------", "--------", "------");
  for (int m = 0; m < num_modes; m++) {
    StringBuilder *sb = string_builder_create();
    string_builder_add_move(sb, game_get_board(fmt_game),
                            &results[m].best_move, game_get_ld(fmt_game),
                            false);
    char spread_str[16];
    if (results[m].spread_known)
      snprintf(spread_str, sizeof(spread_str), "%+.2f",
               results[m].best_expected_spread);
    else
      snprintf(spread_str, sizeof(spread_str), "n/a");

    printf("%-27s | %-18s %5.1f%% %8s %5.1fs\n",
           modes[m].label, string_builder_peek(sb),
           results[m].best_win_pct * 100.0, spread_str, times[m]);
    string_builder_destroy(sb);
  }
  printf("\n");

  config_destroy(fmt_config);
}
