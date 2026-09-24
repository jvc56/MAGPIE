#include "benchmark_peg_test.h"

#include "../src/compat/ctime.h"
#include "../src/compat/memory_info.h"
#include "../src/def/game_defs.h"
#include "../src/def/rack_defs.h"
#include "../src/def/thread_control_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/game.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/thread_control.h"
#include "../src/impl/cgp.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/peg.h"
#include "../src/str/move_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <assert.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// On-demand pre-endgame (PEG) utility-loss benchmark. Each position is solved
// with two fast in-game configs (A and B), then a single strong COMMON ORACLE
// is run on EVERY position: full-enumeration (no stride), the top-32 candidates
// carried without halving to 4-ply fidelity, with pnoprune (PegArgs.protect_
// moves) forcing A's and B's chosen moves into that field so they are always
// scored. Every leaf — arms and oracle — uses MAGPIE's greedy playout at the
// endgame frontier (no static truncation before game end).
//
// Quality is the oracle's value of a move, never a config's own self-reported
// win% (which is just its estimate at its own shallow fidelity). For each mode
// we report utility loss = oracle_best_win - oracle_win(mode's chosen move):
// how much win% the mode left on the table versus the best play the oracle
// found. Running the oracle even when A and B agree catches the case where they
// agree on a jointly-suboptimal move. The oracle is a strong reference, not
// ground truth (a 4-ply, top-32 greedy-playout search), and is slow
// (~hundreds of seconds/position); the A/B times are the in-game-relevant ones.
//
// Configs (including the oracle) are hardcoded in the test_benchmark_peg_*
// entry points below — there are deliberately no environment-variable knobs.
// Positions come from the committed notes/peg_positions/random_Npeg.txt
// fixtures (run from the repo root); each line's embedded "-lex CSW24" is
// honored by loading via the cgp command. The on-demand entry points hardcode
// their tuning knobs (threads, time budgets, position counts, nesting schedule)
// as local constants — edit them in source to sweep different values.

// One hardcoded PEG solver configuration (an A/B arm or the oracle).
typedef struct PegBenchConfig {
  const char *name;
  int num_threads;
  double time_budget_seconds; // 0 = unbounded
  int scenario_stride;        // <= 1 = full enumeration (bag >= 3 only)
  const int *stage_top_k;     // NULL = built-in default cascade
  int num_stages;             // 0 = default cascade length
  bool nested_enabled;        // nested-PEG non-emptier lookahead
  int nested_cand_cap;
  const int *nested_cand_caps; // per-level cap sequence (NULL = flat cap)
  int nested_n_cand_caps;
  int nested_stride;
  int nested_emptier_ply_cap;
  int nested_max_depth;
  PegPoll *poll; // non-NULL = live mode (publishes partial stages)
} PegBenchConfig;

// Result of solving one position with one fast config.
typedef struct PegBenchOutcome {
  char move_str[32];
  Move move; // kept so the oracle can re-evaluate it via pnoprune
  double elapsed;
  int stage;          // last_completed_stage reached
  bool stage_partial; // true if that stage was cut off mid-way (partial top-K)
  // Deepest stage REACHED (>= stage; one beyond when partial) and its progress.
  // deep_work is the stage's cands_done counter, which is bumped per candidate
  // *scenario* completion (peg.c) — a scenario-granular measure of how far into
  // the stage the arm got. It is comparable across arms: both evaluate the same
  // candidates over the same scenario set at each stage, so the only difference
  // is how much of that work each finished in the budget. This credits the arm
  // that got further within a partial stage, not just for completing one.
  int deep_stage; // index of the deepest stage reached (n_stage_history - 1)
  int deep_work;  // scenario-completions in that deepest stage
  // Per-arm coverage: how many stages the arm completed, the root candidate
  // field size (same for both arms on a position), and the total candidate-
  // scenario evaluations across all stages (the real "how much did this arm
  // compute" number, which differs sharply between nested and rollout).
  int n_stages;    // stages reached (n_stage_history)
  int root_cands;  // stage-0 candidate field size
  int total_evals; // sum of cands_done across all stages
  bool ok;
} PegBenchOutcome;

// How far an arm got, crediting partial-stage progress: deeper stage wins; at
// the same stage, more scenario work finished wins. Returns >0 if `a` got
// further than `b`, <0 if `b` got further, 0 if tied.
static int peg_progress_cmp(const PegBenchOutcome *a,
                            const PegBenchOutcome *b) {
  if (a->deep_stage != b->deep_stage) {
    return a->deep_stage > b->deep_stage ? 1 : -1;
  }
  if (a->deep_work != b->deep_work) {
    return a->deep_work > b->deep_work ? 1 : -1;
  }
  return 0;
}

// Oracle outcome for one position: the best move it found plus the oracle's
// value of A's and B's chosen moves (matched by move string; -1 if not found).
typedef struct OracleResult {
  char best_str[32];
  double best_win;
  double win_a;
  double win_b;
  double spread_a; // oracle mean spread (points) of A's move
  double spread_b; // oracle mean spread (points) of B's move
  bool has_spread; // true when both moves were scored by the oracle
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
  args->nested_enabled = cfg->nested_enabled;
  args->nested_cand_cap = cfg->nested_cand_cap;
  args->nested_cand_caps = cfg->nested_cand_caps;
  args->nested_n_cand_caps = cfg->nested_n_cand_caps;
  args->nested_stride = cfg->nested_stride;
  args->nested_emptier_ply_cap = cfg->nested_emptier_ply_cap;
  args->nested_max_depth = cfg->nested_max_depth;
  args->poll = cfg->poll;
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
  // Reset the (reused) live poll so this solve's stage history / cands_done
  // start from zero rather than accumulating from prior solves.
  peg_poll_reset(args.poll);
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
    outcome.stage = result.last_completed_stage;
    outcome.stage_partial = result.last_stage_partial;
    outcome.n_stages = result.n_stage_history;
    if (result.n_stage_history > 0) {
      const PegStageSnapshot *deep =
          &result.stage_history[result.n_stage_history - 1];
      outcome.deep_stage = result.n_stage_history - 1;
      outcome.deep_work = deep->cands_done;
      outcome.root_cands = result.stage_history[0].field_size;
      for (int stage_idx = 0; stage_idx < result.n_stage_history; stage_idx++) {
        outcome.total_evals += result.stage_history[stage_idx].cands_done;
      }
    }
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
  oracle.spread_a = 0.0;
  oracle.spread_b = 0.0;
  oracle.has_spread = false;
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
      // Oracle's value of each arm's chosen move: win% and mean spread
      // (points).
      if (strcmp(cand_str, a->move_str) == 0) {
        oracle.win_a = result.top_cands[c].win_pct;
        oracle.spread_a = result.top_cands[c].mean_spread;
      }
      if (strcmp(cand_str, b->move_str) == 0) {
        oracle.win_b = result.top_cands[c].win_pct;
        oracle.spread_b = result.top_cands[c].mean_spread;
      }
    }
    oracle.has_spread = oracle.win_a >= 0.0 && oracle.win_b >= 0.0;
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
  double sum_t_a = 0;
  double max_t_a = 0;
  double sum_t_b = 0;
  double max_t_b = 0;
  double sum_t_o = 0;
  double max_t_o = 0;

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
    sum_t_a += a.elapsed;
    max_t_a = a.elapsed > max_t_a ? a.elapsed : max_t_a;
    sum_t_b += b.elapsed;
    max_t_b = b.elapsed > max_t_b ? b.elapsed : max_t_b;
    sum_t_o += o.elapsed;
    max_t_o = o.elapsed > max_t_o ? o.elapsed : max_t_o;

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
  if (solved > 0) {
    printf("    TIME/pos  A %.3fs (max %.3fs)  B %.3fs (max %.3fs)  oracle "
           "%.3fs (max %.3fs)\n",
           sum_t_a / solved, max_t_a, sum_t_b / solved, max_t_b,
           sum_t_o / solved, max_t_o);
  }
  printf("==============================================================\n");
  (void)fflush(stdout);

  free(cgp_lines);
  config_destroy(config);
}

// Stage-table A/B with a top-32, 4-ply, full-enumeration oracle. A and B are
// fast in-game configs (18 threads, 30s, stride 7); the oracle keeps the top 32
// candidates (no halving) at 4-ply fidelity with no stride and a generous
// budget, run on every position with pnoprune so A's and B's moves are always
// scored. Edit these to sweep configs.
static void run_stage_table_utility(const char *cgp_file, const char *label,
                                    int max_positions) {
  log_set_level(LOG_FATAL);
  static const int stage_top_k_a[] = {4, 2};
  static const int stage_top_k_b[] = {8, 4, 2};
  // Oracle: top-32 candidates carried (no halving) to 4-ply fidelity. fidelity
  // is stage+1, so three same-width stages reach stage 3 = 4-ply while never
  // narrowing the field. Leaves use MAGPIE's greedy playout at the endgame
  // frontier (no static truncation), like the arms.
  static const int stage_top_k_oracle[] = {32, 32, 32};
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
  const PegBenchConfig oracle = {.name = "top32@4ply",
                                 .num_threads = 18,
                                 .time_budget_seconds = 3600,
                                 .scenario_stride = 1,
                                 .stage_top_k = stage_top_k_oracle,
                                 .num_stages = 3};
  run_peg_utility_benchmark(cgp_file, label, &cfg_a, &cfg_b, &oracle,
                            max_positions);
}

// At bag <= 2 the scenario space is tiny and scenario_stride is ignored
// (peg.c only strides at bag >= 3), so A and B both run full enumeration and
// differ only by cascade depth. Solves are fast at this bag size.
void test_benchmark_peg_1(void) {
  run_stage_table_utility("notes/peg_positions/random_1peg.txt", "1-peg", 100);
}

void test_benchmark_peg_2(void) {
  run_stage_table_utility("notes/peg_positions/random_2peg.txt", "2-peg", 25);
}

void test_benchmark_peg_3(void) {
  run_stage_table_utility("notes/peg_positions/random_3peg.txt", "3-peg", 25);
}

void test_benchmark_peg_4(void) {
  run_stage_table_utility("notes/peg_positions/random_4peg.txt", "4-peg", 25);
}

// ---------------------------------------------------------------------------
// Fixture generator (on-demand): produce the random_{1,2,3,4}peg.txt position
// files consumed by the benchmarks above (random_2peg also documents the
// lineage of peg_poll_test's hardcoded position). Greedy self-play
// from a random opening; whenever the bag drops to exactly `target_bag` tiles
// with both players holding a full rack, the position is the canonical
// K-in-bag PEG setup, so emit "<cgp> -lex CSW24" (the per-line lexicon the
// loaders honor). Deterministic in the base seed.
//
// contested_only: when set, keep a candidate only if a quick PEG solve scores
// its best move between PEG_CONTESTED_LO and PEG_CONTESTED_HI win% -- i.e. the
// outcome is genuinely in doubt. This matters most at small bag counts (1-2),
// where most random positions are already decided (win% pegged at 0 or 100) and
// the score margin does NOT separate decided from contested, so a solve is the
// only reliable filter.
// ---------------------------------------------------------------------------

enum { PEG_GEN_QUICK_TOP_K_0 = 8, PEG_GEN_QUICK_TOP_K_1 = 4 };
static const double PEG_CONTESTED_LO = 0.05;
static const double PEG_CONTESTED_HI = 0.95;

// Fast PEG solve to classify the currently-loaded position. Returns the best
// move's win% in [0,1], or -1 on failure. Shallow cascade (top-{8,4}) at full
// scenario enumeration -- enough to tell a contested position from a decided
// one without the cost of the full benchmark oracle.
static double peg_quick_best_win(const Config *config) {
  static const int quick_top_k[] = {PEG_GEN_QUICK_TOP_K_0,
                                    PEG_GEN_QUICK_TOP_K_1};
  PegArgs args;
  memset(&args, 0, sizeof(args));
  args.game = config_get_game(config);
  args.thread_control = config_get_thread_control(config);
  args.num_threads = 4;
  args.time_budget_seconds = 10;
  args.scenario_stride = 1;
  args.stage_top_k = quick_top_k;
  args.num_stages = 2;
  PegResult result;
  ErrorStack *err = error_stack_create();
  peg_solve(&args, &result, err);
  double win = -1.0;
  if (error_stack_is_empty(err) && result.n_top_cands > 0) {
    win = result.top_cands[0].win_pct;
  }
  error_stack_destroy(err);
  peg_result_destroy(&result);
  return win;
}

// Greedy self-play until the bag holds exactly target_bag tiles with both
// racks full. Returns false if the game ends first or the bag skips past
// target_bag (a turn can draw several tiles at once).
static bool play_until_bag_size(Game *game, MoveList *move_list,
                                int target_bag) {
  while (bag_get_letters(game_get_bag(game)) > target_bag) {
    const Move *move = get_top_equity_move(game, move_list);
    play_move(move, game, NULL);
    if (game_get_game_end_reason(game) != GAME_END_REASON_NONE) {
      return false;
    }
    if (bag_get_letters(game_get_bag(game)) < target_bag) {
      return false; // overshot this target
    }
  }
  if (bag_get_letters(game_get_bag(game)) != target_bag) {
    return false;
  }
  const Rack *rack0 = player_get_rack(game_get_player(game, 0));
  const Rack *rack1 = player_get_rack(game_get_player(game, 1));
  return rack_get_total_letters(rack0) == RACK_SIZE &&
         rack_get_total_letters(rack1) == RACK_SIZE;
}

static void generate_peg_cgps(uint64_t base_seed, int target_bag,
                              int target_count, const char *outfile,
                              bool append, bool contested_only) {
  Config *config =
      config_create_or_die("set -lex CSW24 -threads 1 -s1 score -s2 score");
  MoveList *move_list = move_list_create(1);
  exec_config_quiet(config, "new");
  Game *game = config_get_game(config);

  const int max_attempts = 2000000;
  FILE *fp = fopen_or_die(outfile, append ? "ae" : "we");
  int found = 0;
  int examined = 0;
  for (int attempt = 0; found < target_count && attempt < max_attempts;
       attempt++) {
    game_reset(game);
    game_seed(game, base_seed + (uint64_t)attempt);
    draw_starting_racks(game);
    if (!play_until_bag_size(game, move_list, target_bag)) {
      continue;
    }
    if (contested_only) {
      examined++;
      const double win = peg_quick_best_win(config);
      if (win < PEG_CONTESTED_LO || win > PEG_CONTESTED_HI) {
        continue; // decided (or failed solve) -> skip
      }
    }
    char *cgp = game_get_cgp(game, true);
    (void)fprintf(fp, "%s -lex CSW24\n", cgp);
    free(cgp);
    found++;
  }
  (void)fclose(fp);
  if (contested_only) {
    printf("[genpegcgps] %d-in-bag: %d/%d contested -> %s (%d K-in-bag "
           "positions examined)\n",
           target_bag, found, target_count, outfile, examined);
  } else {
    printf("[genpegcgps] %d-in-bag: %d/%d positions -> %s\n", target_bag, found,
           target_count, outfile);
  }
  (void)fflush(stdout);

  move_list_destroy(move_list);
  config_destroy(config);
}

void test_generate_peg_cgps(void) {
  log_set_level(LOG_FATAL);
  // All bag counts use CONTESTED positions only (best-move win% in [5,95] by a
  // quick pre-filter solve), so the four utility-loss tables are comparable.
  // Without it, ~80% of random 1-2 in-bag positions are already decided (move
  // choice is moot), and the score margin can't separate decided from contested
  // at low bag, so the solve-based filter is required. 1-in-bag uses 100
  // positions (it is cheap, ~85s/solve); 2-4 use 25 (their oracle is slow).
  generate_peg_cgps(10241, 1, 100, "notes/peg_positions/random_1peg.txt",
                    /*append=*/false, /*contested_only=*/true);
  generate_peg_cgps(20242, 2, 25, "notes/peg_positions/random_2peg.txt",
                    /*append=*/false, /*contested_only=*/true);
  generate_peg_cgps(30243, 3, 25, "notes/peg_positions/random_3peg.txt",
                    /*append=*/false, /*contested_only=*/true);
  generate_peg_cgps(40244, 4, 25, "notes/peg_positions/random_4peg.txt",
                    /*append=*/false, /*contested_only=*/true);
}

// CI test for `-pegtopk all` (no candidate cap = EXHAUSTIVE mode: one deep
// stage that keeps every candidate and solves each at full endgame depth). The
// position is solved twice -- with a top-2 cap and with `all` -- and `all` must
// publish strictly more candidates than the cap, proving the cap was removed
// and the whole field was kept (and the exhaustive deep stage actually ran).
//
// The position is a canned (not derived) near-decided 2-in-bag TWL98 position:
// the exhaustive solve of EVERY candidate (field of 9) completes in ~0.03s,
// because the endgame is essentially settled. This matters because most
// K-in-bag positions have genuinely hard endgames whose exhaustive solve takes
// ~minutes
// -- far too slow for CI -- so the position was chosen by timing the actual
// exhaustive `peg_solve` and keeping a fast, fully-completing one (1-in-bag and
// typical 2-in-bag exhaustives do NOT finish quickly). Only run by CI (in the
// main test_table). The line embeds "-lex TWL98", which the cgp load honors.
void test_peg_pegtopk_all(void) {
  log_set_level(LOG_FATAL);
  static const char *const locked_position =
      "15/15/12V2/11GO1G/10SOX1R/10LO1SI/8J1ID1oN/7POUTY1UN/5Q4H2RE/"
      "5UNLIVED1ED/3KOA1A2REBS1/3A1G1T3LITE/2BUD1AE1W1LO2/1FIREMEN1YA1NA1/"
      "1AZINE1THEMaTIC AFIOPRR/ACEEOST 332/386 0 -lex TWL98";

  Config *config =
      config_create_or_die("set -lex TWL98 -threads 1 -s1 score -s2 score");
  char *load_cmd = get_formatted_string("cgp %s", locked_position);

  // This test exercises the -pegtopk cap mechanism, not nesting; the canned
  // position was chosen for a fast FLAT exhaustive (~0.03s). Pin nesting off so
  // the timing/field premise holds independent of the -pegnested default.
  exec_config_quiet(config, "set -pegnested false");

  // Top-2 cap: at most 2 candidates published.
  exec_config_quiet(config, "set -pegtopk 2");
  exec_config_quiet(config, load_cmd);
  exec_config_quiet(config, "peg");
  const int n_capped = config_get_peg_result(config)->n_top_cands;

  // No cap: the whole field is kept and published (exhaustive single stage).
  exec_config_quiet(config, "set -pegtopk all");
  exec_config_quiet(config, load_cmd);
  exec_config_quiet(config, "peg");
  const PegResult *result = config_get_peg_result(config);
  const int n_all = result->n_top_cands;

  assert(result->last_completed_stage >= 1); // the exhaustive deep stage ran
  assert(n_capped <= 2);                     // the cap held the field to 2
  assert(n_all > n_capped); // 'all' removed the cap -> strictly more cands
  printf("[pegtopk_all] capped=%d all=%d\n", n_capped, n_all);
  (void)fflush(stdout);

  free(load_cmd);
  config_destroy(config);
}

// On-demand: run PEG to completion on the first max_pos positions of a fixture
// file, printing per-position wall time + chosen move/win/spread + the total. A
// simple timing harness (used to A/B the chained-wordprune cache).
enum { PEG_BENCH_MAX_CANDIDATE_EVENTS = 16384 };

typedef struct PegBenchCandidateEvent {
  int stage_idx;
  int cand_rank;
  int scenarios_completed;
  int64_t elapsed_ns;
} PegBenchCandidateEvent;

typedef struct PegBenchCandidateTrace {
  _Atomic uint64_t event_count;
  int64_t start_ns;
  PegBenchCandidateEvent events[PEG_BENCH_MAX_CANDIDATE_EVENTS];
} PegBenchCandidateTrace;

static void peg_bench_on_candidate_done(int stage_idx, int cand_rank,
                                        const Move *cand, double win_pct,
                                        double mean_spread,
                                        int scenarios_completed,
                                        int64_t completed_ns, bool reordered,
                                        void *user_data) {
  (void)cand;
  (void)win_pct;
  (void)mean_spread;
  (void)reordered;
  PegBenchCandidateTrace *trace = user_data;
  const uint64_t event_idx =
      atomic_fetch_add_explicit(&trace->event_count, 1, memory_order_relaxed);
  if (event_idx >= PEG_BENCH_MAX_CANDIDATE_EVENTS) {
    return;
  }
  PegBenchCandidateEvent *event = &trace->events[event_idx];
  event->stage_idx = stage_idx;
  event->cand_rank = cand_rank;
  event->scenarios_completed = scenarios_completed;
  event->elapsed_ns = completed_ns - trace->start_ns;
}

void test_peg_bench_fixture(void) {
  log_set_level(LOG_FATAL);
  const char *file = "notes/peg_positions/random_3peg.txt";
  const int max_pos = 10;
  const char *threads_env = getenv("PEG_BENCH_THREADS");
  const int threads = threads_env != NULL ? (int)strtol(threads_env, NULL, 10)
                                          : get_num_cores();
  const double tlim = 0.0; // 0 = unbounded

  FILE *fp = fopen(file, "re");
  if (!fp) {
    printf("[pegb] no fixture at %s (run from repo root)\n", file);
    return;
  }
  char (*lines)[4096] = malloc((size_t)max_pos * 4096);
  assert(lines);
  int num_lines = 0;
  while (num_lines < max_pos && fgets(lines[num_lines], 4096, fp)) {
    size_t len = strlen(lines[num_lines]);
    if (len > 0 && lines[num_lines][len - 1] == '\n') {
      lines[num_lines][len - 1] = '\0';
    }
    if (strlen(lines[num_lines]) > 0) {
      num_lines++;
    }
  }
  (void)fclose(fp);

  Config *config =
      config_create_or_die("set -lex CSW24 -threads 1 -s1 score -s2 score");
  printf("[pegb] file=%s positions=%d threads=%d tlim=%.0f\n", file, num_lines,
         threads, tlim);
  (void)fflush(stdout);

  double sum_elapsed = 0;
  for (int pos_idx = 0; pos_idx < num_lines; pos_idx++) {
    char *cmd = get_formatted_string("cgp %s", lines[pos_idx]);
    exec_config_quiet(config, cmd);
    free(cmd);

    PegArgs args;
    memset(&args, 0, sizeof(args));
    args.game = config_get_game(config);
    args.thread_control = config_get_thread_control(config);
    args.num_threads = threads;
    args.time_budget_seconds = tlim;
    args.scenario_stride = 1;
    args.num_stages = 0; // built-in default cascade
    PegBenchCandidateTrace *trace = calloc_or_die(1, sizeof(*trace));
    atomic_init(&trace->event_count, 0);
    args.on_cand_done = peg_bench_on_candidate_done;
    args.user_data = trace;

    PegResult result;
    ErrorStack *err = error_stack_create();
    Timer timer;
    ctimer_start(&timer);
    trace->start_ns = ctimer_monotonic_ns();
    peg_solve(&args, &result, err);
    const double elapsed = ctimer_elapsed_seconds(&timer);
    sum_elapsed += elapsed;

    const uint64_t reported_event_count =
        atomic_load_explicit(&trace->event_count, memory_order_relaxed);
    const uint64_t retained_event_count =
        reported_event_count < PEG_BENCH_MAX_CANDIDATE_EVENTS
            ? reported_event_count
            : PEG_BENCH_MAX_CANDIDATE_EVENTS;
    uint64_t candidate_scenarios = 0;
    for (uint64_t event_idx = 0; event_idx < retained_event_count;
         event_idx++) {
      const PegBenchCandidateEvent *event = &trace->events[event_idx];
      candidate_scenarios += (uint64_t)event->scenarios_completed;
      printf("[pegbcand] pos=%d stage=%d rank=%d scenarios=%d "
             "elapsed_ms=%.3f\n",
             pos_idx, event->stage_idx, event->cand_rank,
             event->scenarios_completed, (double)event->elapsed_ns / 1.0e6);
    }

    char best[32] = "-";
    double win = -1.0;
    double spread = 0.0;
    if (error_stack_is_empty(err) && result.n_top_cands > 0) {
      move_to_string(config_get_game(config), &result.best_move, best,
                     sizeof(best));
      win = result.top_cands[0].win_pct;
      spread = result.top_cands[0].mean_spread;
    }
    printf("[pegb] pos=%2d elapsed=%.2f stage=%d candidates=%llu "
           "candidate_scenarios=%llu event_drops=%llu best=%-14s win=%.4f "
           "spread=%+.3f\n",
           pos_idx, elapsed, result.last_completed_stage,
           (unsigned long long)reported_event_count,
           (unsigned long long)candidate_scenarios,
           (unsigned long long)(reported_event_count - retained_event_count),
           best, win, spread);
    (void)fflush(stdout);
    error_stack_destroy(err);
    peg_result_destroy(&result);
    free(trace);
  }
  printf("[pegb] TOTAL elapsed=%.2fs over %d positions\n", sum_elapsed,
         num_lines);
  (void)fflush(stdout);
  free(lines);
  config_destroy(config);
}

// On-demand nested-PEG decision-quality benchmark. Across 1..4-in-bag contested
// fixtures, arm A solves non-emptier leaves with the staged inner-peg lookahead
// and arm B with the plain greedy rollout (both otherwise identical: same
// budget, default cascade, full enumeration). When the arms pick DIFFERENT
// moves, a deeper EXHAUSTIVE-ish nested oracle (both moves protected via
// pnoprune) scores each move; the win% gap = oracle_win(nested move) -
// oracle_win(rollout move). Positive => nesting reached a better in-budget
// decision. The spread gap (points) is logged separately.
void test_peg_nested_gap(void) {
  log_set_level(LOG_FATAL);
  const double arm_tlim = 8.0;
  const double oracle_tlim = 60.0;
  const int max_pos = 25;
  const int threads = 8;
  // Bag sizes to benchmark (1..4). Narrow to a single size by setting both
  // bounds to it (e.g. first_bag = last_bag = 3 for 3-in-bag only).
  const int first_bag = 1;
  const int last_bag = 4;
  // Live-mode poll so the arms publish PARTIAL stages (the faster arm's extra
  // in-budget candidates are credited). Reused across the sequential arm
  // solves.
  PegPoll *arm_poll = peg_poll_create();

  const int nest_cap = 0;
  // 0 = bag-dependent default stride (2-peg 1, 3-peg 5, 4-peg 7).
  const int nest_stride = 0;
  // Inner-peg recursion depth (how many nested pegs before greedy); default 1.
  const int nest_maxdepth = 1;
  // The inner peg's STAGE schedule (initial field + per-stage keep counts). The
  // arms use {8,4,2}; the oracle uses {8,8,8} (wider).
  static const int nest_cap_seq[] = {8, 4, 2};
  const int nest_n_caps = 3;
  static const int oracle_nest_caps[] = {8, 8, 8}; // wider inner peg for oracle

  // Arm A: staged inner-peg lookahead. Arm B: plain rollout. Both reprune-on
  // and otherwise identical, so the comparison isolates nesting.
  PegBenchConfig cfg_on = {.name = "nested",
                           .num_threads = threads,
                           .time_budget_seconds = arm_tlim,
                           .scenario_stride = 1,
                           .stage_top_k = NULL,
                           .num_stages = 0,
                           .nested_enabled = true,
                           .nested_cand_cap = nest_cap,
                           .nested_stride = nest_stride,
                           .nested_max_depth = nest_maxdepth};
  if (nest_n_caps > 0) {
    cfg_on.nested_cand_caps = nest_cap_seq;
    cfg_on.nested_n_cand_caps = nest_n_caps;
  }
  cfg_on.poll = arm_poll; // live mode -> partial stages used
  PegBenchConfig cfg_off = cfg_on;
  cfg_off.name = "rollout";
  cfg_off.nested_enabled = false;
  cfg_off.nested_cand_caps = NULL;
  cfg_off.nested_n_cand_caps = 0;
  // Oracle: same staged inner peg, but a wider {8,8,8} schedule (no narrowing)
  // over a top-32 outer field, so it scores both arms' moves at full fidelity.
  static const int oracle_topk[] = {32, 32, 32};
  PegBenchConfig cfg_oracle = {.name = "oracle-nested",
                               .num_threads = threads,
                               .time_budget_seconds = oracle_tlim,
                               .scenario_stride = 1,
                               .stage_top_k = oracle_topk,
                               .num_stages = 3,
                               .nested_enabled = true,
                               .nested_cand_caps = oracle_nest_caps,
                               .nested_n_cand_caps = 3,
                               .nested_stride = nest_stride,
                               .nested_max_depth = nest_maxdepth};

  printf("[gap] arm_tlim=%.0f oracle_tlim=%.0f max=%d\n", arm_tlim, oracle_tlim,
         max_pos);
  (void)fflush(stdout);
  const char *dir = "notes/peg_positions";
  for (int bag = first_bag; bag <= last_bag; bag++) {
    char file[256];
    (void)snprintf(file, sizeof(file), "%s/random_%dpeg.txt", dir, bag);
    FILE *fp = fopen(file, "re");
    if (!fp) {
      printf("[gap] bag=%d: no fixture %s\n", bag, file);
      continue;
    }
    Config *config =
        config_create_or_die("set -lex CSW24 -threads 1 -s1 score -s2 score");
    char (*lines)[4096] = malloc((size_t)max_pos * 4096);
    assert(lines);
    int num_lines = 0;
    while (num_lines < max_pos && fgets(lines[num_lines], 4096, fp)) {
      size_t len = strlen(lines[num_lines]);
      if (len > 0 && lines[num_lines][len - 1] == '\n') {
        lines[num_lines][len - 1] = '\0';
      }
      if (strlen(lines[num_lines]) > 0) {
        num_lines++;
      }
    }
    (void)fclose(fp);
    int disagree = 0;
    int scored = 0; // disagreements where the oracle valued BOTH arms' moves
    int nst_better = 0;
    int rol_better = 0;
    double sum_gap = 0;
    double sum_spread_gap = 0; // oracle spread (points): A's move - B's move
    double sum_a_elapsed = 0;
    double sum_b_elapsed = 0;
    int nst_deeper = 0; // nested reached a deeper completed stage
    int rol_deeper = 0;
    int nst_further =
        0; // nested got further crediting partial-stage candidates
    int rol_further = 0;
    for (int pos_idx = 0; pos_idx < num_lines; pos_idx++) {
      char *cmd = get_formatted_string("cgp %s", lines[pos_idx]);
      exec_config_quiet(config, cmd);
      free(cmd);
      PegBenchOutcome a = run_one_peg(config, &cfg_on);
      PegBenchOutcome b = run_one_peg(config, &cfg_off);
      if (!a.ok || !b.ok) {
        continue;
      }
      sum_a_elapsed += a.elapsed;
      sum_b_elapsed += b.elapsed;
      if (a.stage > b.stage) {
        nst_deeper++;
      } else if (b.stage > a.stage) {
        rol_deeper++;
      }
      const int prog = peg_progress_cmp(&a, &b);
      if (prog > 0) {
        nst_further++;
      } else if (prog < 0) {
        rol_further++;
      }
      const bool agree = strcmp(a.move_str, b.move_str) == 0;
      double gap = 0.0;
      double spread_gap = 0.0;
      bool have_spread = false;
      if (!agree) {
        disagree++;
        OracleResult o = run_oracle(config, &cfg_oracle, &a, &b);
        // Only fold the gap in when the oracle valued BOTH protected moves.
        // o.has_spread means win_a and win_b are both real; a budget cutoff can
        // leave one unscored (win = -1), which would poison the aggregates.
        if (o.has_spread) {
          scored++;
          gap = o.win_a - o.win_b; // win%: A's (nested) minus B's (rollout)
          sum_gap += gap;
          if (gap > 0) {
            nst_better++;
          } else if (gap < 0) {
            rol_better++;
          }
          have_spread = true;
          spread_gap = o.spread_a - o.spread_b; // points
          sum_spread_gap += spread_gap;
        }
      }
      // Per-position: rc = root candidate field (same for both arms). Per arm:
      // elapsed, st = stages reached (+'p' if the deepest was partial), ev =
      // total candidate-scenario evaluations (coverage), the chosen move, and
      // the oracle gap on disagreements.
      char spreadstr[24] = "";
      if (have_spread) {
        (void)snprintf(spreadstr, sizeof(spreadstr), " sgap=%+.2f", spread_gap);
      }
      printf("[gap] bag=%d pos=%3d rc%-3d nst[%.2fs st%d%s ev%-6d %-13s] "
             "rol[%.2fs st%d%s ev%-6d %-13s] %s gap=%+.4f%s\n",
             bag, pos_idx, a.root_cands, a.elapsed, a.n_stages,
             a.stage_partial ? "p" : "", a.total_evals, a.move_str, b.elapsed,
             b.n_stages, b.stage_partial ? "p" : "", b.total_evals, b.move_str,
             agree ? "AGREE" : "DIFFER", gap, spreadstr);
      (void)fflush(stdout);
    }
    printf("[gap] BAG %d SUMMARY: positions=%d disagreements=%d scored=%d "
           "nested_better=%d rollout_better=%d mean_gap=%+.4f "
           "mean_spreadgap=%+.3f "
           "| mean_elapsed nst=%.2fs rol=%.2fs | deeper_stage nst=%d rol=%d | "
           "further(stage,cands) nst=%d rol=%d\n",
           bag, num_lines, disagree, scored, nst_better, rol_better,
           scored ? sum_gap / scored : 0.0,
           scored ? sum_spread_gap / scored : 0.0,
           num_lines ? sum_a_elapsed / num_lines : 0.0,
           num_lines ? sum_b_elapsed / num_lines : 0.0, nst_deeper, rol_deeper,
           nst_further, rol_further);
    (void)fflush(stdout);
    free(lines);
    config_destroy(config);
  }
  peg_poll_destroy(arm_poll);
}

// On-demand: append 75 more contested positions for 2/3/4-in-bag (1-in-bag
// already has 100) with fresh seeds, into notes/peg_positions, so the gap
// benchmark can run 100 per bag.
void test_gen_peg_more(void) {
  log_set_level(LOG_FATAL);
  const char *dir = "notes/peg_positions";
  char f[256];
  (void)snprintf(f, sizeof(f), "%s/random_2peg.txt", dir);
  generate_peg_cgps(20242777, 2, 75, f, /*append=*/true,
                    /*contested_only=*/true);
  (void)snprintf(f, sizeof(f), "%s/random_3peg.txt", dir);
  generate_peg_cgps(30243777, 3, 75, f, /*append=*/true,
                    /*contested_only=*/true);
  (void)snprintf(f, sizeof(f), "%s/random_4peg.txt", dir);
  generate_peg_cgps(40244777, 4, 75, f, /*append=*/true,
                    /*contested_only=*/true);
}

// PEG stage-stability / cost harness: for each fixture position, run peg_solve
// capped at max_stage=k (k=1..KMAX), unbounded (each capped cascade completes),
// single deterministic config (stride 1, nested off). Emits per (position,k)
// the published best move + win/spread + per-stage field/fidelity/wall-time, so
// we can measure (1) whether the top move changes as one more halving stage
// completes, and (2) the marginal cost of completing the next stage vs the ~4%
// speedup (which only touches emptier-leaf endgame time). Env: MAGPIE_PEG_MAX
// (positions/file), MAGPIE_PEG_KMAX (default 5), MAGPIE_PEG_THREADS (default
// 18).
void test_peg_stage_stability(void) {
  log_set_level(LOG_FATAL);
  const char *em = getenv("MAGPIE_PEG_MAX");
  const int maxpos = (em && *em) ? (int)strtol(em, NULL, 10) : 100000;
  const char *ek = getenv("MAGPIE_PEG_KMAX");
  const int kmax = (ek && *ek) ? (int)strtol(ek, NULL, 10) : 5;
  const char *ekn = getenv("MAGPIE_PEG_KMIN");
  const int kmin = (ekn && *ekn) ? (int)strtol(ekn, NULL, 10) : 1;
  const char *et = getenv("MAGPIE_PEG_THREADS");
  const int threads = (et && *et) ? (int)strtol(et, NULL, 10) : 18;
  const char *eb = getenv("MAGPIE_PEG_BUDGET");
  const double budget = (eb && *eb) ? strtod(eb, NULL) : 0.0;
  const char *es_ = getenv("MAGPIE_PEG_STRIDE");
  const int stride = (es_ && *es_) ? (int)strtol(es_, NULL, 10) : 1;
  const struct {
    const char *f;
    int bag;
  } files[] = {
      {"notes/peg_positions/random_1peg.txt", 1},
      {"notes/peg_positions/random_2peg.txt", 2},
      {"notes/peg_positions/random_3peg.txt", 3},
      {"notes/peg_positions/random_4peg.txt", 4},
  };
  Config *config =
      config_create_or_die("set -lex CSW24 -threads 18 -s1 score -s2 score");
  PegPoll *poll = peg_poll_create();
  printf("PEGCFG threads=%d kmax=%d maxpos=%d\n", threads, kmax, maxpos);
  (void)fflush(stdout);
  for (int fi = 0; fi < 4; fi++) {
    FILE *fp = fopen(files[fi].f, "re");
    if (!fp) {
      printf("PEGERR no %s\n", files[fi].f);
      continue;
    }
    char line[4096];
    int pos_id = 0;
    while (pos_id < maxpos && fgets(line, sizeof(line), fp)) {
      size_t len = strlen(line);
      if (len > 0 && line[len - 1] == '\n') {
        line[len - 1] = '\0';
      }
      if (strlen(line) == 0) {
        continue;
      }
      char *cmd = get_formatted_string("cgp %s", line);
      load_and_exec_config_or_die(config, cmd);
      free(cmd);
      Game *game = config_get_game(config);
      for (int k = kmin; k <= kmax; k++) {
        PegArgs a;
        memset(&a, 0, sizeof(a));
        a.game = game;
        a.thread_control = config_get_thread_control(config);
        a.num_threads = threads;
        a.scenario_stride = stride;
        a.nested_enabled = false;
        a.time_budget_seconds = budget;
        a.max_stage = k;
        peg_poll_reset(poll);
        a.poll = poll;
        PegResult r;
        ErrorStack *es = error_stack_create();
        peg_solve(&a, &r, es);
        if (!error_stack_is_empty(es)) {
          error_stack_destroy(es);
          peg_result_destroy(&r);
          break;
        }
        error_stack_destroy(es);
        char mv[64];
        move_to_string(game, &r.best_move, mv, sizeof(mv));
        printf("PEGROW bag=%d pos=%d k=%d move=%s win=%.5f spread=%.2f lcs=%d "
               "partial=%d elapsed=%.4f",
               files[fi].bag, pos_id, k, mv, r.best_win, r.best_spread,
               r.last_completed_stage, r.last_stage_partial ? 1 : 0,
               ctimer_elapsed_seconds(&r.timer));
        for (int s = 0; s < r.n_stage_history; s++) {
          const PegStageSnapshot *ss = &r.stage_history[s];
          double t =
              ss->end_ns ? (double)(ss->end_ns - ss->start_ns) / 1e9 : -1.0;
          printf(" |s%d f%d N%d d%d t%.4f", s, ss->fidelity_plies,
                 ss->field_size, ss->cands_done, t);
        }
        printf("\n");
        peg_result_destroy(&r);
      }
      pos_id++;
      (void)fflush(stdout);
    }
    (void)fclose(fp);
  }
  peg_poll_destroy(poll);
  config_destroy(config);
}

// PEG strength A/B by TRUE oracle value: run two arms that differ only in time
// budget (base = T, opt = T*1.04, modelling a 4%-faster engine), then score
// each arm's chosen move against a deep top-32 oracle. mean_loss_a -
// mean_loss_b is the expected true win% the extra budget buys per PEG move (=
// flip_rate x win-delta). Env: MAGPIE_PEG_BA (base budget, 2.0), MAGPIE_PEG_BB
// (opt budget, 2.08), MAGPIE_PEG_ORACLE (oracle budget, 30), MAGPIE_PEG_MAX
// (pos/file), MAGPIE_PEG_STRIDE.
static void run_budget_ab(const char *cgp_file, const char *label,
                          double base_budget, double opt_budget, int stride,
                          double oracle_budget, int maxpos) {
  static const int oracle_k[] = {32, 32, 32};
  const PegBenchConfig cfg_a = {.name = "base",
                                .num_threads = 18,
                                .time_budget_seconds = base_budget,
                                .scenario_stride = stride,
                                .num_stages = 0};
  const PegBenchConfig cfg_b = {.name = "opt+4%",
                                .num_threads = 18,
                                .time_budget_seconds = opt_budget,
                                .scenario_stride = stride,
                                .num_stages = 0};
  const PegBenchConfig oracle = {.name = "top32@4ply",
                                 .num_threads = 18,
                                 .time_budget_seconds = oracle_budget,
                                 .scenario_stride = 1,
                                 .stage_top_k = oracle_k,
                                 .num_stages = 3};
  run_peg_utility_benchmark(cgp_file, label, &cfg_a, &cfg_b, &oracle, maxpos);
}

void test_peg_strength_ab(void) {
  log_set_level(LOG_FATAL);
  const char *ea = getenv("MAGPIE_PEG_BA");
  const double ba = (ea && *ea) ? strtod(ea, NULL) : 2.0;
  const char *eb = getenv("MAGPIE_PEG_BB");
  const double bb = (eb && *eb) ? strtod(eb, NULL) : 2.08;
  const char *eo = getenv("MAGPIE_PEG_ORACLE");
  const double ob = (eo && *eo) ? strtod(eo, NULL) : 30.0;
  const char *em = getenv("MAGPIE_PEG_MAX");
  const int maxpos = (em && *em) ? (int)strtol(em, NULL, 10) : 25;
  const char *estr = getenv("MAGPIE_PEG_STRIDE");
  const int stride = (estr && *estr) ? (int)strtol(estr, NULL, 10) : 0;
  printf("PEGABCFG base=%.2fs opt=%.2fs oracle=%.0fs stride=%d maxpos=%d\n", ba,
         bb, ob, stride, maxpos);
  run_budget_ab("notes/peg_positions/random_1peg.txt", "1peg", ba, bb, stride,
                ob, maxpos);
  run_budget_ab("notes/peg_positions/random_2peg.txt", "2peg", ba, bb, stride,
                ob, maxpos);
  run_budget_ab("notes/peg_positions/random_3peg.txt", "3peg", ba, bb, stride,
                ob, maxpos);
  run_budget_ab("notes/peg_positions/random_4peg.txt", "4peg", ba, bb, stride,
                ob, maxpos);
}

// Generate a FRESH battery of PEG positions (new seeds, contested) to /tmp for
// a long strength run, so we are not measuring on the committed fixtures. Env
// MAGPIE_PEG_GENCOUNT (positions per bag, default 200).
void test_gen_peg_fresh(void) {
  log_set_level(LOG_FATAL);
  const char *ec = getenv("MAGPIE_PEG_GENCOUNT");
  const int count = (ec && *ec) ? (int)strtol(ec, NULL, 10) : 200;
  generate_peg_cgps(918273101ULL, 1, count, "/tmp/peg_fresh_1peg.txt", false,
                    true);
  generate_peg_cgps(918273202ULL, 2, count, "/tmp/peg_fresh_2peg.txt", false,
                    true);
  generate_peg_cgps(918273303ULL, 3, count, "/tmp/peg_fresh_3peg.txt", false,
                    true);
  generate_peg_cgps(918273404ULL, 4, count, "/tmp/peg_fresh_4peg.txt", false,
                    true);
}

// PEG throughput->strength CURVE: for each position run several arms that
// differ ONLY in time budget (base T, then T*mult for each multiplier), then
// ONE deep oracle scores every arm's chosen move. Prints one flushed line per
// position (interrupt-safe): PEGCURVE bag pos best=<oracle_best_win>
// aK=<move>|<oracle_win>... mean over positions of (best - oracle_win(arm)) =
// that arm's utility loss; the base-minus-arm difference is the true win% the
// extra budget buys. Env: MAGPIE_PEG_BASE (2.0), MAGPIE_PEG_MULTS
// ("1.04,1.5,2.0"),
//      MAGPIE_PEG_ORACLE (30), MAGPIE_PEG_STRIDE (0), MAGPIE_PEG_MAX (per
//      file).
void test_peg_strength_curve(void) {
  log_set_level(LOG_FATAL);
  const char *be = getenv("MAGPIE_PEG_BASE");
  const double base = (be && *be) ? strtod(be, NULL) : 2.0;
  const char *oe = getenv("MAGPIE_PEG_ORACLE");
  const double ob = (oe && *oe) ? strtod(oe, NULL) : 30.0;
  const char *se = getenv("MAGPIE_PEG_STRIDE");
  const int stride = (se && *se) ? (int)strtol(se, NULL, 10) : 0;
  const char *me = getenv("MAGPIE_PEG_MAX");
  const int maxpos = (me && *me) ? (int)strtol(me, NULL, 10) : 1000000;
  double mult[8] = {1.04, 1.5, 2.0};
  int nmult = 3;
  const char *ms = getenv("MAGPIE_PEG_MULTS");
  if (ms && *ms) {
    char buf[128];
    (void)snprintf(buf, sizeof(buf), "%s", ms);
    nmult = 0;
    const char *tok = strtok(buf, ",");
    while (tok && nmult < 8) {
      mult[nmult++] = strtod(tok, NULL);
      tok = strtok(NULL, ",");
    }
  }
  const int narm = nmult + 1;
  double budget[9];
  budget[0] = base;
  for (int i = 0; i < nmult; i++) {
    budget[i + 1] = base * mult[i];
  }
  const struct {
    const char *f;
    int bag;
  } files[] = {
      {"/tmp/peg_fresh_1peg.txt", 1},
      {"/tmp/peg_fresh_2peg.txt", 2},
      {"/tmp/peg_fresh_3peg.txt", 3},
      {"/tmp/peg_fresh_4peg.txt", 4},
  };
  Config *config =
      config_create_or_die("set -lex CSW24 -threads 1 -s1 score -s2 score");
  static const int oracle_k[] = {32, 32, 32};
  printf("PEGCURVECFG base=%.2f oracle=%.0f stride=%d arms=", base, ob, stride);
  for (int a = 0; a < narm; a++) {
    printf("%.3f%s", budget[a], a + 1 < narm ? "," : "\n");
  }
  (void)fflush(stdout);
  for (int fi = 0; fi < 4; fi++) {
    FILE *fp = fopen(files[fi].f, "re");
    if (!fp) {
      printf("PEGCURVEERR no %s\n", files[fi].f);
      continue;
    }
    char line[4096];
    int pos_id = 0;
    while (pos_id < maxpos && fgets(line, sizeof(line), fp)) {
      size_t len = strlen(line);
      if (len > 0 && line[len - 1] == '\n') {
        line[len - 1] = '\0';
      }
      if (strlen(line) == 0) {
        continue;
      }
      char *cmd = get_formatted_string("cgp %s", line);
      load_and_exec_config_or_die(config, cmd);
      free(cmd);
      const Game *game = config_get_game(config);
      PegBenchOutcome arm[9];
      bool all_ok = true;
      for (int a = 0; a < narm; a++) {
        PegBenchConfig cfg = {.name = "arm",
                              .num_threads = 18,
                              .time_budget_seconds = budget[a],
                              .scenario_stride = stride,
                              .num_stages = 0};
        arm[a] = run_one_peg(config, &cfg);
        if (!arm[a].ok) {
          all_ok = false;
        }
      }
      if (!all_ok) {
        printf("PEGCURVE bag=%d pos=%d SKIP\n", files[fi].bag, pos_id);
        pos_id++;
        (void)fflush(stdout);
        continue;
      }
      const Move *protect[9];
      char pstr[9][32];
      int np = 0;
      for (int a = 0; a < narm; a++) {
        bool dup = false;
        for (int j = 0; j < np; j++) {
          if (strcmp(pstr[j], arm[a].move_str) == 0) {
            dup = true;
            break;
          }
        }
        if (!dup) {
          protect[np] = &arm[a].move;
          (void)snprintf(pstr[np], sizeof(pstr[np]), "%s", arm[a].move_str);
          np++;
        }
      }
      PegArgs oa;
      const PegBenchConfig ocfg = {.name = "oracle",
                                   .num_threads = 18,
                                   .time_budget_seconds = ob,
                                   .scenario_stride = 1,
                                   .stage_top_k = oracle_k,
                                   .num_stages = 3};
      fill_peg_args(&oa, config, &ocfg);
      oa.protect_moves = protect;
      oa.n_protect_moves = np;
      PegResult r;
      ErrorStack *es = error_stack_create();
      peg_solve(&oa, &r, es);
      if (error_stack_is_empty(es) && r.n_top_cands > 0) {
        // top_cands is sorted by the solver's utility (win_pct + 1e-4*spread),
        // so [0] is the best-utility move. Log both win% and spread so utility
        // loss = (best_win + 1e-4*best_spread) - (arm_win + 1e-4*arm_spread).
        printf("PEGCURVE bag=%d pos=%d bestw=%.5f bests=%.2f", files[fi].bag,
               pos_id, r.top_cands[0].win_pct, r.top_cands[0].mean_spread);
        for (int a = 0; a < narm; a++) {
          double ow = -1.0;
          double os = 0.0;
          for (int c = 0; c < r.n_top_cands; c++) {
            char cs[32];
            move_to_string(game, &r.top_cands[c].move, cs, sizeof(cs));
            if (strcmp(cs, arm[a].move_str) == 0) {
              ow = r.top_cands[c].win_pct;
              os = r.top_cands[c].mean_spread;
              break;
            }
          }
          printf(" a%d=%s|%.5f|%.2f", a, arm[a].move_str, ow, os);
        }
        printf("\n");
      } else {
        printf("PEGCURVE bag=%d pos=%d ORACLEFAIL\n", files[fi].bag, pos_id);
      }
      error_stack_destroy(es);
      peg_result_destroy(&r);
      (void)fflush(stdout);
      pos_id++;
    }
    (void)fclose(fp);
  }
  config_destroy(config);
}
