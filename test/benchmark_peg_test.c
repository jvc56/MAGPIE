#include "benchmark_peg_test.h"

#include "../src/compat/ctime.h"
#include "../src/def/thread_control_defs.h"
#include "../src/ent/game.h"
#include "../src/ent/move.h"
#include "../src/ent/thread_control.h"
#include "../src/impl/config.h"
#include "../src/impl/peg.h"
#include "../src/str/move_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// On-demand pre-endgame (PEG) utility-loss benchmark. Each position is solved
// with two fast in-game configs (A and B), then a single strong COMMON ORACLE
// is run on EVERY position: a full-enumeration (no stride), deep default
// cascade {32,16,8,4,2}, with pnoprune (PegArgs.protect_moves) forcing A's and
// B's chosen moves to survive to the deepest stage so they are scored in the
// context of the full field even when they would otherwise be pruned.
//
// Quality is the oracle's value of a move, never a config's own self-reported
// win% (which is just its estimate at its own shallow fidelity). For each mode
// we report utility loss = oracle_best_win - oracle_win(mode's chosen move):
// how much win% the mode left on the table versus the best play the oracle
// found. Running the oracle even when A and B agree catches the case where they
// agree on a jointly-suboptimal move.
//
// Configs (including the oracle) are hardcoded in the test_benchmark_peg_*
// entry points below — there are deliberately no environment-variable knobs.
// Positions come from the committed notes/peg_positions/random_Npeg.txt
// fixtures (run from the repo root); each line's embedded "-lex CSW24" is
// honored by loading via the cgp command.

// One hardcoded PEG solver configuration (an A/B arm or the oracle).
typedef struct PegBenchConfig {
  const char *name;
  int num_threads;
  double time_budget_seconds; // 0 = unbounded
  int scenario_stride;        // <= 1 = full enumeration (bag >= 3 only)
  const int *stage_top_k;     // NULL = built-in default cascade
  int num_stages;             // 0 = default cascade length
} PegBenchConfig;

// Result of solving one position with one fast config.
typedef struct PegBenchOutcome {
  char move_str[32];
  Move move; // kept so the oracle can re-evaluate it via pnoprune
  double elapsed;
  bool ok;
} PegBenchOutcome;

// Oracle outcome for one position: the best move it found plus the oracle's
// value of A's and B's chosen moves (matched by move string; -1 if not found).
typedef struct OracleResult {
  char best_str[32];
  double best_win;
  double win_a;
  double win_b;
  double elapsed;
  bool ok;
} OracleResult;

// Run `cmd` against `config` with stdout suppressed (CGP loads are chatty).
static void exec_config_quiet(Config *config, const char *cmd) {
  (void)fflush(stdout);
  int saved_stdout = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 0);
  int devnull = open("/dev/null", O_WRONLY | O_CLOEXEC);
  (void)dup2(devnull, STDOUT_FILENO);
  close(devnull);

  ErrorStack *error_stack = error_stack_create();
  thread_control_set_status(config_get_thread_control(config),
                            THREAD_CONTROL_STATUS_STARTED);
  config_load_command(config, cmd, error_stack);
  assert(error_stack_is_empty(error_stack));
  config_execute_command(config, error_stack);
  assert(error_stack_is_empty(error_stack));
  error_stack_destroy(error_stack);
  thread_control_set_status(config_get_thread_control(config),
                            THREAD_CONTROL_STATUS_FINISHED);

  (void)fflush(stdout);
  (void)dup2(saved_stdout, STDOUT_FILENO);
  close(saved_stdout);
}

static void fill_peg_args(PegArgs *args, const Config *config,
                          const PegBenchConfig *cfg) {
  memset(args, 0, sizeof(*args));
  args->game = config_get_game(config);
  args->thread_control = config_get_thread_control(config);
  args->num_threads = cfg->num_threads;
  args->time_budget_seconds = cfg->time_budget_seconds;
  args->scenario_stride = cfg->scenario_stride;
  args->stage_top_k = cfg->num_stages > 0 ? cfg->stage_top_k : NULL;
  args->num_stages = cfg->num_stages;
}

static void move_to_string(const Game *game, const Move *move, char *out,
                           size_t out_size) {
  StringBuilder *sb = string_builder_create();
  string_builder_add_move(sb, game_get_board(game), move, game_get_ld(game),
                          false);
  (void)snprintf(out, out_size, "%s", string_builder_peek(sb));
  string_builder_destroy(sb);
}

static PegBenchOutcome run_one_peg(const Config *config,
                                   const PegBenchConfig *cfg) {
  PegArgs args;
  fill_peg_args(&args, config, cfg);
  PegResult result;
  ErrorStack *err = error_stack_create();
  Timer timer;
  ctimer_start(&timer);
  peg_solve(&args, &result, err);

  PegBenchOutcome outcome;
  memset(&outcome, 0, sizeof(outcome));
  outcome.elapsed = ctimer_elapsed_seconds(&timer);
  if (error_stack_is_empty(err)) {
    outcome.move = result.best_move;
    move_to_string(config_get_game(config), &outcome.move, outcome.move_str,
                   sizeof(outcome.move_str));
    outcome.ok = true;
  }
  error_stack_destroy(err);
  peg_result_destroy(&result);
  return outcome;
}

// Full-field oracle with pnoprune protecting A's and B's moves so both are
// carried to the deepest stage and scored. Returns the oracle's best move/win
// and its value of each protected move.
static OracleResult run_oracle(const Config *config, const PegBenchConfig *cfg,
                               const PegBenchOutcome *a,
                               const PegBenchOutcome *b) {
  const bool same = strcmp(a->move_str, b->move_str) == 0;
  const Move *protect[2];
  int n_protect = 0;
  protect[n_protect++] = &a->move;
  if (!same) {
    protect[n_protect++] = &b->move;
  }

  PegArgs args;
  fill_peg_args(&args, config, cfg);
  args.protect_moves = protect;
  args.n_protect_moves = n_protect;

  PegResult result;
  ErrorStack *err = error_stack_create();
  Timer timer;
  ctimer_start(&timer);
  peg_solve(&args, &result, err);

  OracleResult oracle;
  memset(&oracle, 0, sizeof(oracle));
  oracle.elapsed = ctimer_elapsed_seconds(&timer);
  oracle.best_win = -1.0;
  oracle.win_a = -1.0;
  oracle.win_b = -1.0;
  if (error_stack_is_empty(err) && result.n_top_cands > 0) {
    oracle.ok = true;
    const Game *game = config_get_game(config);
    // top_cands is sorted descending, so [0] is the best play found.
    oracle.best_win = result.top_cands[0].win_pct;
    move_to_string(game, &result.top_cands[0].move, oracle.best_str,
                   sizeof(oracle.best_str));
    for (int c = 0; c < result.n_top_cands; c++) {
      char cand_str[32];
      move_to_string(game, &result.top_cands[c].move, cand_str,
                     sizeof(cand_str));
      if (strcmp(cand_str, a->move_str) == 0) {
        oracle.win_a = result.top_cands[c].win_pct;
      }
      if (strcmp(cand_str, b->move_str) == 0) {
        oracle.win_b = result.top_cands[c].win_pct;
      }
    }
  }
  error_stack_destroy(err);
  peg_result_destroy(&result);
  return oracle;
}

static void run_peg_utility_benchmark(const char *cgp_file, const char *label,
                                      const PegBenchConfig *cfg_a,
                                      const PegBenchConfig *cfg_b,
                                      const PegBenchConfig *oracle_cfg,
                                      int max_positions) {
  FILE *fp = fopen(cgp_file, "re");
  if (!fp) {
    printf("No CGP file found at %s — run from the repo root.\n", cgp_file);
    return;
  }

  Config *config =
      config_create_or_die("set -lex CSW24 -threads 1 -s1 score -s2 score");

  char (*cgp_lines)[4096] = malloc((size_t)max_positions * 4096);
  assert(cgp_lines);
  int num_cgps = 0;
  while (num_cgps < max_positions && fgets(cgp_lines[num_cgps], 4096, fp)) {
    size_t len = strlen(cgp_lines[num_cgps]);
    if (len > 0 && cgp_lines[num_cgps][len - 1] == '\n') {
      cgp_lines[num_cgps][len - 1] = '\0';
    }
    if (strlen(cgp_lines[num_cgps]) > 0) {
      num_cgps++;
    }
  }
  (void)fclose(fp);

  printf("\n");
  printf("==============================================================\n");
  printf("  PEG utility-loss benchmark [%s]: %d positions\n", label, num_cgps);
  printf("  A=%s  B=%s   oracle=%s (stride=%d stages=%d tlim=%.0fs, +pnoprune "
         "A/B)\n",
         cfg_a->name, cfg_b->name, oracle_cfg->name,
         oracle_cfg->scenario_stride, oracle_cfg->num_stages,
         oracle_cfg->time_budget_seconds);
  printf(
      "  loss = oracle_best_win - oracle_win(mode's move); oracle win is the "
      "only quality measure.\n");
  printf("==============================================================\n");
  printf("  %4s  %-13s%6s   %-13s%6s%7s   %-13s%6s%7s\n", "Pos", "oracle best",
         "win", "A move", "Awin", "lossA", "B move", "Bwin", "lossB");
  printf(
      "  ----  -------------%6s   -------------%6s%7s   -------------%6s%7s\n",
      "", "", "", "", "");

  double sum_loss_a = 0;
  double sum_loss_b = 0;
  double worst_a = 0;
  double worst_b = 0;
  int loss_n_a = 0;
  int loss_n_b = 0;
  int a_optimal = 0;
  int b_optimal = 0;
  int solved = 0;

  for (int ci = 0; ci < num_cgps; ci++) {
    char *cmd = get_formatted_string("cgp %s", cgp_lines[ci]);
    exec_config_quiet(config, cmd);
    free(cmd);

    PegBenchOutcome a = run_one_peg(config, cfg_a);
    PegBenchOutcome b = run_one_peg(config, cfg_b);
    if (!a.ok || !b.ok) {
      continue; // bag size outside the PEG range — skip.
    }
    OracleResult o = run_oracle(config, oracle_cfg, &a, &b);
    solved++;

    // win/loss are 0..1; report as percentage points.
    double loss_a = o.win_a >= 0.0 ? 100.0 * (o.best_win - o.win_a) : -1.0;
    double loss_b = o.win_b >= 0.0 ? 100.0 * (o.best_win - o.win_b) : -1.0;
    if (loss_a >= 0.0) {
      sum_loss_a += loss_a;
      loss_n_a++;
      if (loss_a > worst_a) {
        worst_a = loss_a;
      }
      if (loss_a < 0.05) {
        a_optimal++;
      }
    }
    if (loss_b >= 0.0) {
      sum_loss_b += loss_b;
      loss_n_b++;
      if (loss_b > worst_b) {
        worst_b = loss_b;
      }
      if (loss_b < 0.05) {
        b_optimal++;
      }
    }

    char awin[8];
    char bwin[8];
    char lossa[8];
    char lossb[8];
    (void)snprintf(awin, sizeof(awin), o.win_a >= 0 ? "%.1f" : "  -",
                   100.0 * o.win_a);
    (void)snprintf(bwin, sizeof(bwin), o.win_b >= 0 ? "%.1f" : "  -",
                   100.0 * o.win_b);
    (void)snprintf(lossa, sizeof(lossa), loss_a >= 0 ? "%.1f" : "  ?", loss_a);
    (void)snprintf(lossb, sizeof(lossb), loss_b >= 0 ? "%.1f" : "  ?", loss_b);
    printf("  %4d  %-13s%6.1f   %-13s%6s%7s   %-13s%6s%7s\n", ci + 1,
           o.best_str, 100.0 * o.best_win, a.move_str, awin, lossa, b.move_str,
           bwin, lossb);
    (void)fflush(stdout);
  }

  printf("  ----  --------------------   --------------------   "
         "--------------------\n");
  printf("\n");
  printf("  Results (%d positions):\n", solved);
  printf("    A (%s): matched oracle best %d/%d, mean loss %.2f, worst %.1f\n",
         cfg_a->name, a_optimal, loss_n_a,
         loss_n_a > 0 ? sum_loss_a / loss_n_a : 0.0, worst_a);
  printf("    B (%s): matched oracle best %d/%d, mean loss %.2f, worst %.1f\n",
         cfg_b->name, b_optimal, loss_n_b,
         loss_n_b > 0 ? sum_loss_b / loss_n_b : 0.0, worst_b);
  printf("==============================================================\n");
  (void)fflush(stdout);

  free(cgp_lines);
  config_destroy(config);
}

// Stage-table A/B with a full-enumeration deep oracle. A and B are fast in-game
// configs (18 threads, 30s, stride 7); the oracle is the full default cascade
// with no stride and a generous budget, run on every position with pnoprune so
// A's and B's moves are always scored. Edit these to sweep configs.
static void run_stage_table_utility(const char *cgp_file, const char *label,
                                    int max_positions) {
  log_set_level(LOG_FATAL);
  static const int stage_top_k_a[] = {4, 2};
  static const int stage_top_k_b[] = {8, 4, 2};
  const PegBenchConfig cfg_a = {.name = "4,2",
                                .num_threads = 18,
                                .time_budget_seconds = 30,
                                .scenario_stride = 7,
                                .stage_top_k = stage_top_k_a,
                                .num_stages = 2};
  const PegBenchConfig cfg_b = {.name = "8,4,2",
                                .num_threads = 18,
                                .time_budget_seconds = 30,
                                .scenario_stride = 7,
                                .stage_top_k = stage_top_k_b,
                                .num_stages = 3};
  const PegBenchConfig oracle = {.name = "full/deep",
                                 .num_threads = 18,
                                 .time_budget_seconds = 3600,
                                 .scenario_stride = 1,
                                 .stage_top_k = NULL,
                                 .num_stages = 0};
  run_peg_utility_benchmark(cgp_file, label, &cfg_a, &cfg_b, &oracle,
                            max_positions);
}

void test_benchmark_peg_3(void) {
  run_stage_table_utility("notes/peg_positions/random_3peg.txt", "3-peg", 10);
}

void test_benchmark_peg_4(void) {
  run_stage_table_utility("notes/peg_positions/random_4peg.txt", "4-peg", 50);
}
