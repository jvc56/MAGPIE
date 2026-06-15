#include "benchmark_peg_test.h"

#include "../src/compat/ctime.h"
#include "../src/def/equity_defs.h"
#include "../src/def/game_defs.h"
#include "../src/def/game_history_defs.h"
#include "../src/def/move_defs.h"
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
#include "../src/impl/move_gen.h"
#include "../src/impl/peg.h"
#include "../src/str/move_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <assert.h>
#include <fcntl.h>
#include <limits.h>
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

// Count player_idx's scoring tile-placement plays at the current position.
static int peg_count_scoring_plays(Game *game, MoveList *move_list,
                                   int player_idx) {
  const int on_turn = game_get_player_on_turn_index(game);
  if (on_turn != player_idx) {
    game_set_player_on_turn_index(game, player_idx);
  }
  const MoveGenArgs args = {
      .game = game,
      .move_list = move_list,
      .move_record_type = MOVE_RECORD_ALL,
      .move_sort_type = MOVE_SORT_SCORE,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
  };
  generate_moves(&args);
  const int n = move_list_get_count(move_list);
  int scoring = 0;
  for (int i = 0; i < n; i++) {
    const Move *move = move_list_get_move(move_list, i);
    if (move_get_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE &&
        move_get_score(move) > 0) {
      scoring++;
    }
  }
  if (on_turn != player_idx) {
    game_set_player_on_turn_index(game, on_turn);
  }
  return scoring;
}

// CI test for `-pegtopk all` (no candidate cap). Finds a locked-down K-in-bag
// position by seeded self-play — both players have >= 2 scoring plays but the
// board is otherwise minimal, so the mover has only a handful of candidates —
// then solves it twice: with a top-2 cap and with `all`. `all` must publish
// strictly more candidates than the cap (it kept the whole field). Fast because
// the field is tiny; only run by CI (this is in the main test_table).
void test_peg_pegtopk_all(void) {
  log_set_level(LOG_FATAL);
  for (int target_bag = 1; target_bag <= 2; target_bag++) {
    // -tlim bounds each solve in case the locked field is not tiny; the
    // assertions hold regardless (stage 0 keeps the whole field for 'all').
    Config *config = config_create_or_die(
        "set -lex CSW24 -threads 1 -s1 score -s2 score -tlim 15");
    MoveList *move_list = move_list_create(1 << 14); // generous move capacity
    exec_config_quiet(config, "new");
    Game *game = config_get_game(config);

    // Seeded search for the most locked-down position (smallest mover field
    // with both players holding >= 2 scoring plays).
    char locked_cgp[4096] = {0};
    int best_field = INT_MAX;
    const uint64_t base_seed = 424242 + (uint64_t)target_bag;
    for (int attempt = 0; attempt < 1500 && best_field > 6; attempt++) {
      game_reset(game);
      game_seed(game, base_seed + (uint64_t)attempt);
      draw_starting_racks(game);
      if (!play_until_bag_size(game, move_list, target_bag)) {
        continue;
      }
      const int mover_idx = game_get_player_on_turn_index(game);
      if (peg_count_scoring_plays(game, move_list, mover_idx) < 2 ||
          peg_count_scoring_plays(game, move_list, 1 - mover_idx) < 2) {
        continue;
      }
      const int field = move_list_get_count(move_list); // mover's full field
      if (field > 2 && field < best_field) {
        best_field = field;
        char *cgp = game_get_cgp(game, true);
        (void)snprintf(locked_cgp, sizeof(locked_cgp), "%s", cgp);
        free(cgp);
      }
    }
    assert(best_field != INT_MAX); // found a locked, contested position

    char load_cmd[4096];
    (void)snprintf(load_cmd, sizeof(load_cmd), "cgp %s", locked_cgp);

    // Top-2 cap: at most 2 candidates published.
    exec_config_quiet(config, "set -pegtopk 2");
    exec_config_quiet(config, load_cmd);
    exec_config_quiet(config, "peg");
    const int n_capped = config_get_peg_result(config)->n_top_cands;

    // No cap: the whole field is kept and published.
    exec_config_quiet(config, "set -pegtopk all");
    exec_config_quiet(config, load_cmd);
    exec_config_quiet(config, "peg");
    const PegResult *result = config_get_peg_result(config);
    const int n_all = result->n_top_cands;

    assert(result->last_completed_stage >= 0);
    assert(n_capped <= 2);    // the cap held the field to 2
    assert(n_all > n_capped); // 'all' removed the cap -> strictly more cands
    printf("[pegtopk_all] %d-in-bag locked field=%d: capped=%d all=%d\n",
           target_bag, best_field, n_capped, n_all);

    move_list_destroy(move_list);
    config_destroy(config);
  }
}
