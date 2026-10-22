#include "benchmark_endgame_test.h"

#include "../src/compat/cfile.h"
#include "../src/compat/ctime.h"
#include "../src/def/equity_defs.h"
#include "../src/def/game_defs.h"
#include "../src/def/game_history_defs.h"
#include "../src/def/move_defs.h"
#include "../src/def/thread_control_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/endgame_results.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/letter_distribution.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/thread_control.h"
#include "../src/impl/cgp.h"
#include "../src/impl/config.h"
#include "../src/impl/endgame.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_platform.h"
#include "test_util.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Execute config command quietly (suppress stdout during execution)
static void exec_config_quiet(Config *config, const char *cmd) {
  const int saved_stdout = test_platform_silence_stdout();

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

  test_platform_restore_stdout(saved_stdout);
}

// Play moves until the bag is empty, returning true if we get a valid endgame
// position (bag empty, both players have tiles)
static bool play_until_bag_empty(Game *game, MoveList *move_list) {
  while (bag_get_letters(game_get_bag(game)) > 0) {
    const Move *move = get_top_equity_move(game, move_list);
    play_move(move, game, NULL);

    // Check if game ended before bag emptied
    if (game_get_game_end_reason(game) != GAME_END_REASON_NONE) {
      return false;
    }
  }

  // Verify valid endgame: bag empty, both players have tiles
  const Rack *rack0 = player_get_rack(game_get_player(game, 0));
  const Rack *rack1 = player_get_rack(game_get_player(game, 1));
  return !rack_is_empty(rack0) && !rack_is_empty(rack1);
}

// Compute stuck tile fraction for a player: generate moves with
// TILES_PLAYED mode and check which rack tiles appear in no legal move.
static float compute_stuck_fraction(Game *game, MoveList *move_list,
                                    int player_idx) {
  int current_on_turn = game_get_player_on_turn_index(game);
  if (current_on_turn != player_idx) {
    game_set_player_on_turn_index(game, player_idx);
  }

  uint64_t tiles_bv = 0;
  const MoveGenArgs args = {
      .game = game,
      .move_list = move_list,
      .move_record_type = MOVE_RECORD_TILES_PLAYED,
      .move_sort_type = MOVE_SORT_SCORE,
      .override_kwg = NULL,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .tiles_played_bv = &tiles_bv,
  };
  generate_moves(&args);

  if (current_on_turn != player_idx) {
    game_set_player_on_turn_index(game, current_on_turn);
  }

  const Rack *rack = player_get_rack(game_get_player(game, player_idx));
  const LetterDistribution *ld = game_get_ld(game);
  int total = 0;
  int stuck = 0;
  int ld_size = ld_get_size(ld);
  for (int ml = 0; ml < ld_size; ml++) {
    int count = rack_get_letter(rack, ml);
    if (count > 0) {
      total += count;
      if (!(tiles_bv & ((uint64_t)1 << ml))) {
        stuck += count;
      }
    }
  }
  if (total == 0) {
    return 0.0F;
  }
  return (float)stuck / (float)total;
}

// Generate stuck-tile CGPs: self-play games until endgame, filter for
// positions where the opponent is 100% stuck (all rack tiles stuck).
// Saves CGPs, in the temporary directory, to stuck_100pct_cgps.txt and
// partial-stuck to stuck_partial_cgps.txt.
void test_generate_stuck_cgps(void) {
  log_set_level(LOG_FATAL);

  Config *config =
      config_create_or_die("set -lex CSW21 -threads 1 -s1 score -s2 score");

  MoveList *move_list = move_list_create(1);
  exec_config_quiet(config, "new");
  Game *game = config_get_game(config);

  const int target_full = 1500;
  const int target_partial = 200;
  const int target_hard = 500;
  const uint64_t base_seed = 7777;
  const int max_attempts = 1500000;

  char *full_path = test_platform_temporary_path("stuck_100pct_cgps.txt");
  char *partial_path = test_platform_temporary_path("stuck_partial_cgps.txt");
  char *hard_path = test_platform_temporary_path("stuck_hard_endgame_cgps.txt");
  FILE *fp_full = cfile_open(full_path, "we", false);
  FILE *fp_partial = cfile_open(partial_path, "we", false);
  FILE *fp_hard = cfile_open(hard_path, "we", false);
  assert(fp_full);
  assert(fp_partial);
  assert(fp_hard);

  int found_full = 0;
  int found_partial = 0;
  int found_hard = 0;

  for (int i = 0; (found_full < target_full || found_partial < target_partial ||
                   found_hard < target_hard) &&
                  i < max_attempts;
       i++) {
    game_reset(game);
    game_seed(game, base_seed + (uint64_t)i);
    draw_starting_racks(game);

    if (!play_until_bag_empty(game, move_list)) {
      continue;
    }

    // Check for stuck tiles right at bag-empty (full racks = hard positions).
    // Save positions where at least one player has at least one stuck tile.
    if (found_hard < target_hard) {
      for (int p = 0; p < 2; p++) {
        float frac = compute_stuck_fraction(game, move_list, p);
        if (frac > 0.0F) {
          char *cgp = game_get_cgp(game, true);
          (void)fprintf(fp_hard, "%s\n", cgp);
          free(cgp);
          found_hard++;
          break;
        }
      }
    }

    // Continue playing greedy moves past bag-empty to reduce rack sizes.
    // Smaller racks are much more likely to have 100% stuck tiles.
    for (int extra = 0; extra < 10; extra++) {
      if (game_get_game_end_reason(game) != GAME_END_REASON_NONE) {
        break;
      }
      const Rack *r0 = player_get_rack(game_get_player(game, 0));
      const Rack *r1 = player_get_rack(game_get_player(game, 1));
      if (rack_is_empty(r0) || rack_is_empty(r1)) {
        break;
      }

      // Check both players for stuck tiles at this position
      bool saved = false;
      for (int p = 0; p < 2; p++) {
        float frac = compute_stuck_fraction(game, move_list, p);
        if (frac >= 1.0F && found_full < target_full) {
          char *cgp = game_get_cgp(game, true);
          (void)fprintf(fp_full, "%s\n", cgp);
          free(cgp);
          found_full++;
          saved = true;
          break;
        }
        if (frac >= 0.5F && frac < 1.0F && found_partial < target_partial) {
          char *cgp = game_get_cgp(game, true);
          (void)fprintf(fp_partial, "%s\n", cgp);
          free(cgp);
          found_partial++;
          saved = true;
          break;
        }
      }
      if (saved) {
        break;
      }

      // Play one more greedy move
      const Move *move = get_top_equity_move(game, move_list);
      if (move_get_type(move) == GAME_EVENT_PASS) {
        break;
      }
      play_move(move, game, NULL);
    }
  }

  (void)fclose(fp_full);
  (void)fclose(fp_partial);
  (void)fclose(fp_hard);

  printf("\n");
  printf("==============================================================\n");
  printf("  Generate Stuck CGPs (seed=%llu)\n", (unsigned long long)base_seed);
  printf("==============================================================\n");
  printf("  100%% stuck: %d positions → %s\n", found_full, full_path);
  printf("  50-99%% stuck: %d positions → %s\n", found_partial, partial_path);
  printf("  any stuck at bag-empty: %d positions → %s\n", found_hard,
         hard_path);
  printf("==============================================================\n");
  (void)fflush(stdout);

  move_list_destroy(move_list);
  free(full_path);
  free(partial_path);
  free(hard_path);
  config_destroy(config);
}

// Generate non-stuck endgame positions (stuck_fraction == 0 for both players).
static void generate_nonstuck_cgps(uint64_t base_seed, const char *outfile) {
  Config *config =
      config_create_or_die("set -lex CSW21 -threads 1 -s1 score -s2 score");

  MoveList *move_list = move_list_create(1);
  exec_config_quiet(config, "new");
  Game *game = config_get_game(config);

  const int target = 500;
  const int max_attempts = 100000;

  FILE *fp = cfile_open(outfile, "we", false);
  assert(fp);

  int found = 0;

  for (int i = 0; found < target && i < max_attempts; i++) {
    game_reset(game);
    game_seed(game, base_seed + (uint64_t)i);
    draw_starting_racks(game);

    if (!play_until_bag_empty(game, move_list)) {
      continue;
    }

    if (game_get_game_end_reason(game) != GAME_END_REASON_NONE) {
      continue;
    }

    // Check that neither player has stuck tiles
    float frac0 = compute_stuck_fraction(game, move_list, 0);
    float frac1 = compute_stuck_fraction(game, move_list, 1);
    if (frac0 > 0.0F || frac1 > 0.0F) {
      continue;
    }

    char *cgp = game_get_cgp(game, true);
    (void)fprintf(fp, "%s\n", cgp);
    free(cgp);
    found++;
  }

  (void)fclose(fp);

  printf("\n");
  printf("==============================================================\n");
  printf("  Generate Non-Stuck CGPs (seed=%llu)\n",
         (unsigned long long)base_seed);
  printf("==============================================================\n");
  printf("  0%% stuck: %d positions -> %s\n", found, outfile);
  printf("==============================================================\n");
  (void)fflush(stdout);

  move_list_destroy(move_list);
  config_destroy(config);
}

void test_generate_nonstuck_cgps(void) {
  log_set_level(LOG_FATAL);
  char *path = test_platform_temporary_path("nonstuck_cgps.txt");
  generate_nonstuck_cgps(31415, path);
  free(path);
}

void test_generate_nonstuck_cgps2(void) {
  log_set_level(LOG_FATAL);
  char *path = test_platform_temporary_path("nonstuck_cgps2.txt");
  generate_nonstuck_cgps(271828, path);
  free(path);
}

// Core A/B benchmark: solve each CGP twice (with and without
// forced-pass bypass), comparing value and time pairwise.
static void run_ab_benchmark(const char *cgp_file, const char *label,
                             int old_ply, int new_ply, int max_positions) {
  FILE *fp = cfile_open(cgp_file, "re", false);
  if (!fp) {
    printf("No CGP file found at %s — run genstuck/gennonstuck first.\n",
           cgp_file);
    return;
  }

  Config *config =
      config_create_or_die("set -lex CSW21 -threads 6 -s1 score -s2 score");
  exec_config_quiet(config, "new");
  Game *game = config_get_game(config);

  EndgameCtx *solver_old = NULL;
  EndgameCtx *solver_new = NULL;
  EndgameResults *results = endgame_results_create();

  // Read CGPs into array (heap-allocated for large counts)
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
  printf("  A/B Benchmark [%s]: %d positions\n", label, num_cgps);
  printf("  Old: %d-ply (no bypass) vs New: %d-ply (with bypass)\n", old_ply,
         new_ply);
  printf("==============================================================\n");
  printf("  %4s  %8s %8s  %8s %8s  %6s\n", "Pos", "Old Val", "New Val",
         "Old Time", "New Time", "Delta");
  printf("  ----  -------- --------  -------- --------  ------\n");

  double total_time_old = 0;
  double total_time_new = 0;
  int new_better = 0;
  int old_better = 0;
  int same = 0;
  int total_delta = 0;
  int solved = 0;

  for (int ci = 0; ci < num_cgps; ci++) {
    ErrorStack *err = error_stack_create();
    game_load_cgp(game, cgp_lines[ci], err);
    if (!error_stack_is_empty(err)) {
      error_stack_destroy(err);
      continue;
    }
    error_stack_destroy(err);

    EndgameArgs args = {.game = game,
                        .thread_control = config_get_thread_control(config),
                        .plies = old_ply,
                        .tt_fraction_of_mem = 0.05,
                        .initial_small_move_arena_size =
                            DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
                        .num_threads = 8,
                        .num_top_moves = 1,
                        .use_heuristics = true,
                        .per_ply_callback = NULL,
                        .per_ply_callback_data = NULL,
                        .forced_pass_bypass = false,
                        .enable_pv_display = false,
                        .seed = 42};

    // --- OLD (no bypass) ---
    Timer t;
    ctimer_start(&t);
    err = error_stack_create();
    endgame_solve(&solver_old, &args, results, err);
    double time_old = ctimer_elapsed_seconds(&t);
    assert(error_stack_is_empty(err));
    error_stack_destroy(err);
    int32_t val_old =
        endgame_results_get_pvline(results, ENDGAME_RESULT_BEST)->score;

    // --- NEW (with bypass) ---
    args.plies = new_ply;
    args.forced_pass_bypass = true;
    ctimer_start(&t);
    err = error_stack_create();
    endgame_solve(&solver_new, &args, results, err);
    double time_new = ctimer_elapsed_seconds(&t);
    assert(error_stack_is_empty(err));
    error_stack_destroy(err);
    int32_t val_new =
        endgame_results_get_pvline(results, ENDGAME_RESULT_BEST)->score;

    int delta = val_new - val_old;
    total_delta += delta;
    if (delta > 0) {
      new_better++;
    } else if (delta < 0) {
      old_better++;
    } else {
      same++;
    }

    printf("  %4d  %+8d %+8d  %7.3fs %7.3fs  %+5d\n", ci + 1, val_old, val_new,
           time_old, time_new, delta);
    total_time_old += time_old;
    total_time_new += time_new;
    solved++;

    if ((ci + 1) % 25 == 0) {
      (void)fflush(stdout);
    }
  }

  printf("  ----  -------- --------  -------- --------  ------\n");
  printf("\n");
  printf("  Results (%d positions, old=%d-ply new=%d-ply):\n", solved, old_ply,
         new_ply);
  printf("    New better: %d  |  Old better: %d  |  Same: %d\n", new_better,
         old_better, same);
  printf("    Total value delta: %+d (avg %+.2f per position)\n", total_delta,
         solved > 0 ? (double)total_delta / solved : 0.0);
  printf("    Old total time: %.3fs (avg %.3fs)\n", total_time_old,
         solved > 0 ? total_time_old / solved : 0.0);
  printf("    New total time: %.3fs (avg %.3fs)\n", total_time_new,
         solved > 0 ? total_time_new / solved : 0.0);
  printf("    Speedup: %.2fx\n",
         total_time_new > 0 ? total_time_old / total_time_new : 0.0);
  printf("==============================================================\n");
  (void)fflush(stdout);

  free(cgp_lines);
  endgame_results_destroy(results);
  endgame_ctx_destroy(solver_old);
  endgame_ctx_destroy(solver_new);
  config_destroy(config);
}

void test_benchmark_forced_pass(void) {
  log_set_level(LOG_FATAL);
  char *path = test_platform_temporary_path("stuck_100pct_cgps.txt");
  run_ab_benchmark(path, "stuck", 3, 2, 500);
  free(path);
}

void test_benchmark_nonstuck(void) {
  log_set_level(LOG_FATAL);
  char *path = test_platform_temporary_path("nonstuck_cgps.txt");
  run_ab_benchmark(path, "nonstuck", 3, 2, 500);
  free(path);
}

void test_benchmark_nonstuck_3v3(void) {
  log_set_level(LOG_FATAL);
  char *path = test_platform_temporary_path("nonstuck_cgps.txt");
  run_ab_benchmark(path, "nonstuck", 3, 3, 500);
  free(path);
}

// ---------------------------------------------------------------------------
// Speed-optimization benchmark harness.
//
// Solves a battery of endgame CGPs ONCE at a fixed configuration and emits, for
// each position, a machine-readable line:
//
//   BENCHROW <idx> <value> <nodes> <time_s>
//
// followed by a summary line. This is designed for baseline-vs-optimized A/B
// across git revisions: run on the baseline binary, save the output, then run
// on the optimized binary and diff. Correctness = <value> must match exactly on
// every position (endgame is exact). When run single-threaded (the default),
// <nodes> is also deterministic, so an unchanged <nodes> proves a refactor left
// the search tree identical, while a pruning change shows as fewer nodes with
// an unchanged value.
//
// Run as egspeedbench for the defaults, or egspeedbench:<key>=<value>,... so
// the same binary can sweep depth / thread count / battery without
// recompiling:
//   cgp          CGP file  (default nonstuck_cgps.txt, temporary dir)
//   lex          lexicon            (default CSW21)
//   plies        endgame plies      (default 4)
//   threads      solver threads     (default 1  -> deterministic nodes)
//   max          max positions      (default 100)
//   incremental  1 = incremental move lists (default 0)
//   interleave   1 = solve each position off and on, alternating order, TT
//                cleared per solve (default 0); incremental is then ignored
//   tag          label for the run  (default "bench")
// For example: egspeedbench:cgp=positions.txt,plies=5,threads=4,incremental=1
static int env_int(const char *name, int fallback) {
  const char *v = getenv(name);
  if (v == NULL || v[0] == '\0') {
    return fallback;
  }
  return (int)strtol(v, NULL, 10);
}

static double env_double(const char *name, double fallback) {
  const char *v = getenv(name);
  if (v == NULL || v[0] == '\0') {
    return fallback;
  }
  return strtod(v, NULL);
}

typedef struct EndgameSpeedBenchArgs {
  const char *cgp_file; // NULL = nonstuck_cgps.txt in the temporary directory
  const char *lex;
  const char *tag;
  int plies;
  int threads;
  int max_positions;
  bool incremental;
  bool interleave;
} EndgameSpeedBenchArgs;

static void run_endgame_speed_bench(const EndgameSpeedBenchArgs *bench_args) {
  log_set_level(LOG_FATAL);

  char *default_cgp_file = test_platform_temporary_path("nonstuck_cgps.txt");
  const char *cgp_file =
      bench_args->cgp_file != NULL ? bench_args->cgp_file : default_cgp_file;
  const char *lex = bench_args->lex;
  const char *tag = bench_args->tag;
  const int plies = bench_args->plies;
  const int threads = bench_args->threads;
  const int max_positions = bench_args->max_positions;
  const bool incremental = bench_args->incremental;
  // Interleaved A/B: solve every position with the incremental lists off and
  // on, back to back, alternating which goes first per position so neither
  // side systematically benefits from a warm cache. The TT is cleared before
  // every solve so the second solve of a position cannot hit the first's
  // entries. Rows carry a cfg column and two summaries are printed.
  const bool interleave = bench_args->interleave;

  FILE *fp = cfile_open(cgp_file, "re", false);
  if (!fp) {
    printf("BENCHERR no CGP file at %s\n", cgp_file);
    free(default_cgp_file);
    return;
  }

  char settings[256];
  (void)snprintf(settings, sizeof(settings),
                 "set -lex %s -threads %d -s1 score -s2 score", lex, threads);
  Config *config = config_create_or_die(settings);
  exec_config_quiet(config, "new");
  Game *game = config_get_game(config);
  EndgameResults *results = endgame_results_create();
  EndgameCtx *solver = NULL;

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

  printf("BENCHCFG tag=%s lex=%s plies=%d threads=%d incremental=%d "
         "interleave=%d positions=%d file=%s\n",
         tag, lex, plies, threads, (int)incremental, (int)interleave, num_cgps,
         cgp_file);

  // Index 0 = incremental off, 1 = on. Non-interleaved runs use only
  // cfg_totals[incremental].
  double cfg_time[2] = {0.0, 0.0};
  uint64_t cfg_nodes[2] = {0, 0};

  for (int ci = 0; ci < num_cgps; ci++) {
    const int num_cfgs = interleave ? 2 : 1;
    for (int cfg_ord = 0; cfg_ord < num_cfgs; cfg_ord++) {
      // Alternate the order per position: even positions run off first,
      // odd positions run on first.
      int cfg = incremental ? 1 : 0;
      if (interleave) {
        cfg = (ci % 2 == 0) ? cfg_ord : 1 - cfg_ord;
      }

      ErrorStack *err = error_stack_create();
      game_load_cgp(game, cgp_lines[ci], err);
      if (!error_stack_is_empty(err)) {
        error_stack_destroy(err);
        printf("BENCHROW %d SKIP_LOAD\n", ci);
        // Every config loads the same CGP, so skip the position outright.
        break;
      }
      error_stack_destroy(err);

      EndgameArgs args = {.game = game,
                          .thread_control = config_get_thread_control(config),
                          .plies = plies,
                          .tt_fraction_of_mem = 0.05,
                          .initial_small_move_arena_size =
                              DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
                          .num_threads = threads,
                          .num_top_moves = 1,
                          .use_heuristics = true,
                          .per_ply_callback = NULL,
                          .per_ply_callback_data = NULL,
                          .forced_pass_bypass = true,
                          .incremental_movegen = cfg != 0,
                          .enable_pv_display = false,
                          .seed = 42};

      if (interleave && solver != NULL) {
        endgame_ctx_clear_transposition_table(solver);
      }

      Timer t;
      ctimer_start(&t);
      err = error_stack_create();
      endgame_solve(&solver, &args, results, err);
      double elapsed = ctimer_elapsed_seconds(&t);
      assert(error_stack_is_empty(err));
      error_stack_destroy(err);

      int32_t value =
          endgame_results_get_pvline(results, ENDGAME_RESULT_BEST)->score;
      uint64_t nodes = endgame_ctx_get_nodes_searched(solver);

      if (interleave) {
        printf("BENCHROW %d %s %d %llu %.6f\n", ci, cfg ? "on" : "off", value,
               (unsigned long long)nodes, elapsed);
      } else {
        printf("BENCHROW %d %d %llu %.6f\n", ci, value,
               (unsigned long long)nodes, elapsed);
      }
      cfg_time[cfg] += elapsed;
      cfg_nodes[cfg] += nodes;
    }
    if ((ci + 1) % 25 == 0) {
      (void)fflush(stdout);
    }
  }

  const int single_cfg = incremental ? 1 : 0;
  const int first_cfg = interleave ? 0 : single_cfg;
  const int last_cfg = interleave ? 1 : single_cfg;
  for (int cfg = first_cfg; cfg <= last_cfg; cfg++) {
    printf("BENCHSUM tag=%s cfg=%s positions=%d total_time=%.4f "
           "total_nodes=%llu nps=%.0f\n",
           tag, cfg ? "on" : "off", num_cgps, cfg_time[cfg],
           (unsigned long long)cfg_nodes[cfg],
           cfg_time[cfg] > 0 ? (double)cfg_nodes[cfg] / cfg_time[cfg] : 0.0);
  }
  (void)fflush(stdout);

  free(cgp_lines);
  free(default_cgp_file);
  endgame_ctx_destroy(solver);
  endgame_results_destroy(results);
  config_destroy(config);
}

static EndgameSpeedBenchArgs endgame_speed_bench_default_args(void) {
  return (EndgameSpeedBenchArgs){.cgp_file = NULL,
                                 .lex = "CSW21",
                                 .tag = "bench",
                                 .plies = 4,
                                 .threads = 1,
                                 .max_positions = 100,
                                 .incremental = false,
                                 .interleave = false};
}

// The integer after "<key>=" in item.
static int speed_bench_spec_int(const char *item, const char *key) {
  ErrorStack *error_stack = error_stack_create();
  const int value = string_to_int(item + strlen(key) + 1, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    log_fatal("egspeedbench %s must be an integer, got '%s'", key, item);
  }
  error_stack_destroy(error_stack);
  return value;
}

void test_endgame_speed_bench(void) {
  const EndgameSpeedBenchArgs args = endgame_speed_bench_default_args();
  run_endgame_speed_bench(&args);
}

// "<key>=<value>[,<key>=<value>...]", with the keys listed above.
void endgame_speed_bench_run_spec(const char *spec) {
  EndgameSpeedBenchArgs args = endgame_speed_bench_default_args();
  StringSplitter *items = split_string(spec, ',', true);
  const int num_items = string_splitter_get_number_of_items(items);
  for (int item_idx = 0; item_idx < num_items; item_idx++) {
    const char *item = string_splitter_get_item(items, item_idx);
    if (has_prefix("cgp=", item)) {
      args.cgp_file = item + strlen("cgp=");
    } else if (has_prefix("lex=", item)) {
      args.lex = item + strlen("lex=");
    } else if (has_prefix("tag=", item)) {
      args.tag = item + strlen("tag=");
    } else if (has_prefix("plies=", item)) {
      args.plies = speed_bench_spec_int(item, "plies");
    } else if (has_prefix("threads=", item)) {
      args.threads = speed_bench_spec_int(item, "threads");
    } else if (has_prefix("max=", item)) {
      args.max_positions = speed_bench_spec_int(item, "max");
    } else if (has_prefix("incremental=", item)) {
      args.incremental = speed_bench_spec_int(item, "incremental") != 0;
    } else if (has_prefix("interleave=", item)) {
      args.interleave = speed_bench_spec_int(item, "interleave") != 0;
    } else {
      log_fatal("egspeedbench spec takes cgp, lex, tag, plies, threads, max, "
                "incremental and interleave, got '%s'",
                item);
    }
  }
  // The strings in args point into items, so it outlives the run.
  run_endgame_speed_bench(&args);
  string_splitter_destroy(items);
}

// ---------------------------------------------------------------------------
// Time-limited full-game endgame playout benchmark.
//
// From each bag-empty endgame CGP, plays the position out to game over: solve
// the position under a per-move wall-clock budget (time-limited iterative
// deepening), play the best move found, repeat until the game ends. This is the
// realistic time-control scenario -- the solver gets a clock, not a fixed depth
// -- and exercises the solver repeatedly on the shrinking remaining endgame.
//
// Under a per-move time budget both a baseline and an optimized binary spend
// the same wall clock, so the speedup shows up as MORE NODES searched and
// DEEPER exact search reached in that budget (stronger play at equal time).
// Emits, per position:
//   POROW <idx> moves=<n> nodes=<N> depthsum=<D> time=<s> ended=<0|1>
// and a summary. Set MAGPIE_PO_TIMEMS=0 to disable the time limit and instead
// fully solve each move to MAGPIE_PO_PLIES (a pure speed comparison).
//   MAGPIE_PO_CGP    CGP file  (default nonstuck_cgps.txt, temporary dir)
//   MAGPIE_PO_LEX    lexicon         (default CSW21)
//   MAGPIE_PO_MAX    positions       (default 20)
//   MAGPIE_PO_TIMEMS per-move budget (default 100; 0 = no limit)
//   MAGPIE_PO_PLIES  depth ceiling   (default 25)
//   MAGPIE_PO_THREADS solver threads (default 1)
//   MAGPIE_PO_MAXMOVES per-playout move cap (default 40, safety)
//   MAGPIE_PO_TAG    label           (default "playout")
//   MAGPIE_PO_INCREMENTAL 1 = incremental move lists (default 0)
void test_endgame_playout_bench(void) {
  log_set_level(LOG_FATAL);

  char *default_cgp_file = test_platform_temporary_path("nonstuck_cgps.txt");
  const char *cgp_file = getenv("MAGPIE_PO_CGP");
  if (cgp_file == NULL || cgp_file[0] == '\0') {
    cgp_file = default_cgp_file;
  }
  const char *lex = getenv("MAGPIE_PO_LEX");
  if (lex == NULL || lex[0] == '\0') {
    lex = "CSW21";
  }
  const char *tag = getenv("MAGPIE_PO_TAG");
  if (tag == NULL || tag[0] == '\0') {
    tag = "playout";
  }
  const int max_positions = env_int("MAGPIE_PO_MAX", 20);
  const int time_ms = env_int("MAGPIE_PO_TIMEMS", 100);
  const int max_plies = env_int("MAGPIE_PO_PLIES", 25);
  const int threads = env_int("MAGPIE_PO_THREADS", 1);
  const bool incremental = env_int("MAGPIE_PO_INCREMENTAL", 0) != 0;
  const int max_moves = env_int("MAGPIE_PO_MAXMOVES", 40);

  FILE *fp = cfile_open(cgp_file, "re", false);
  if (!fp) {
    printf("POERR no CGP file at %s\n", cgp_file);
    free(default_cgp_file);
    return;
  }

  char settings[256];
  (void)snprintf(settings, sizeof(settings),
                 "set -lex %s -threads %d -s1 score -s2 score", lex, threads);
  Config *config = config_create_or_die(settings);
  exec_config_quiet(config, "new");
  Game *game = config_get_game(config);
  EndgameResults *results = endgame_results_create();
  EndgameCtx *solver = NULL;
  Move *play = move_create();

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

  printf("POCFG tag=%s lex=%s time_ms=%d plies=%d threads=%d incremental=%d "
         "positions=%d\n",
         tag, lex, time_ms, max_plies, threads, (int)incremental, num_cgps);

  long total_moves = 0;
  uint64_t total_nodes = 0;
  long total_depth = 0;
  double total_time = 0.0;
  int completed = 0;

  for (int ci = 0; ci < num_cgps; ci++) {
    ErrorStack *err = error_stack_create();
    game_load_cgp(game, cgp_lines[ci], err);
    if (!error_stack_is_empty(err)) {
      error_stack_destroy(err);
      printf("POROW %d SKIP_LOAD\n", ci);
      continue;
    }
    error_stack_destroy(err);

    int moves = 0;
    uint64_t pos_nodes = 0;
    long pos_depth = 0;
    double pos_time = 0.0;
    bool ended = false;

    while (moves < max_moves) {
      if (game_get_game_end_reason(game) != GAME_END_REASON_NONE) {
        ended = true;
        break;
      }
      // Hard wall-clock cutoff: unlimited depth (plies = ceiling), IDS deepens
      // until the external deadline fires MID-depth (checked every 1024 nodes
      // via check_depth_deadline). soft/hard_time_limit stay 0 so the EBF
      // between-depth stop is off -- the only stop is the hard deadline, so
      // both engines burn the same T ms and the faster one completes deeper. On
      // interrupt the result is the best move from the last COMPLETED depth.
      const int64_t deadline_ns =
          (time_ms > 0) ? (ctimer_monotonic_ns() + (int64_t)time_ms * 1000000LL)
                        : 0;
      EndgameArgs args = {.game = game,
                          .thread_control = config_get_thread_control(config),
                          .plies = max_plies,
                          .tt_fraction_of_mem = 0.05,
                          .initial_small_move_arena_size =
                              DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
                          .num_threads = threads,
                          .num_top_moves = 1,
                          .use_heuristics = true,
                          .forced_pass_bypass = true,
                          .incremental_movegen = incremental,
                          .enable_pv_display = false,
                          .soft_time_limit = 0,
                          .hard_time_limit = 0,
                          .external_deadline_ns = deadline_ns,
                          .seed = 42};

      Timer t;
      ctimer_start(&t);
      err = error_stack_create();
      endgame_solve(&solver, &args, results, err);
      pos_time += ctimer_elapsed_seconds(&t);
      assert(error_stack_is_empty(err));
      error_stack_destroy(err);

      const PVLine *pv =
          endgame_results_get_pvline(results, ENDGAME_RESULT_BEST);
      if (pv->num_moves == 0) {
        break;
      }
      pos_nodes += endgame_ctx_get_nodes_searched(solver);
      pos_depth += pv->negamax_depth;

      // Per-move move+value dump (for baseline-vs-optimized move-agreement /
      // points comparison): tiny_move identifies the played move; score is the
      // solver's spread estimate at the last completed depth.
      if (getenv("MAGPIE_PO_PRINTMOVE") != NULL) {
        printf("MOVE pos=%d ply=%d tiny=%llu score=%d depth=%d\n", ci, moves,
               (unsigned long long)pv->moves[0].tiny_move, pv->score,
               pv->negamax_depth);
      }

      SmallMove best_sm = pv->moves[0];
      small_move_to_move(play, &best_sm, game_get_board(game));
      play_move(play, game, NULL);
      moves++;
    }

    total_moves += moves;
    total_nodes += pos_nodes;
    total_depth += pos_depth;
    total_time += pos_time;
    if (ended) {
      completed++;
    }
    printf("POROW %d moves=%d nodes=%llu depthsum=%ld time=%.4f ended=%d\n", ci,
           moves, (unsigned long long)pos_nodes, pos_depth, pos_time,
           ended ? 1 : 0);
    if ((ci + 1) % 10 == 0) {
      (void)fflush(stdout);
    }
  }

  printf("POSUM tag=%s positions=%d completed=%d total_moves=%ld "
         "total_nodes=%llu total_depth=%ld avg_depth=%.3f total_time=%.4f "
         "nps=%.0f\n",
         tag, num_cgps, completed, total_moves, (unsigned long long)total_nodes,
         total_depth,
         total_moves > 0 ? (double)total_depth / (double)total_moves : 0.0,
         total_time, total_time > 0 ? (double)total_nodes / total_time : 0.0);
  (void)fflush(stdout);

  free(cgp_lines);
  free(default_cgp_file);
  move_destroy(play);
  endgame_ctx_destroy(solver);
  endgame_results_destroy(results);
  config_destroy(config);
}

// ---------------------------------------------------------------------------
// Single-move transducer for a cross-process head-to-head match.
//
// Reads one endgame position (CGP) plus this player's remaining game time bank
// (HARD, seconds) from the environment, solves ONE move under a soft/hard time
// control -- soft = HARD / est_moves_left (a per-turn allocation that banks
// unused time; the EBF between-depth limit stops there) with HARD as the
// mid-search wall-clock backstop (external_deadline_ns) -- plays the best move,
// and emits one machine-readable line the driver feeds to the other engine:
//
//   M1 s0=<int> s1=<int> onmove=<0|1> ended=<0|1> used=<sec> soft=<sec>
//      depth=<int> nodes=<llu> tiny=<llu> cgp=<resulting CGP...>
//
// cgp= is LAST because a CGP contains spaces. Because the two engines differ
// only in movegen speed, running the SAME position through each binary at a
// wall-clock budget is exactly the baseline-vs-optimized comparison; over many
// time-limited games the faster engine's only edge is completing more search in
// the bank it accrues.
//   MAGPIE_M1_CGP     position (raw CGP, no "cgp " prefix)   [required]
//   MAGPIE_M1_HARD    remaining game bank in seconds          (default 5.0)
//   MAGPIE_M1_THREADS solver threads                          (default 1)
//   MAGPIE_M1_LEX     lexicon                                 (default CSW21)
void test_endgame_move1(void) {
  log_set_level(LOG_FATAL);
  const char *cgp = getenv("MAGPIE_M1_CGP");
  if (cgp == NULL || cgp[0] == '\0') {
    printf("M1 ERR no MAGPIE_M1_CGP\n");
    return;
  }
  const char *lex = getenv("MAGPIE_M1_LEX");
  if (lex == NULL || lex[0] == '\0') {
    lex = "CSW21";
  }
  const int threads = env_int("MAGPIE_M1_THREADS", 1);
  const double hard = env_double("MAGPIE_M1_HARD", 5.0);

  char settings[256];
  (void)snprintf(settings, sizeof(settings),
                 "set -lex %s -threads %d -s1 score -s2 score", lex, threads);
  Config *config = config_create_or_die(settings);
  exec_config_quiet(config, "new");
  Game *game = config_get_game(config);

  ErrorStack *err = error_stack_create();
  game_load_cgp(game, cgp, err);
  if (!error_stack_is_empty(err)) {
    printf("M1 ERR bad cgp\n");
    error_stack_destroy(err);
    config_destroy(config);
    return;
  }
  error_stack_destroy(err);

  const int on_turn = game_get_player_on_turn_index(game);
  // Per-turn soft allocation: spread the bank over an estimate of this player's
  // remaining moves (~2 tiles played per move), banking whatever is not used.
  const int rack_tiles =
      rack_get_total_letters(player_get_rack(game_get_player(game, on_turn)));
  int est_moves = (rack_tiles + 1) / 2;
  if (est_moves < 1) {
    est_moves = 1;
  }
  double soft = hard / (double)est_moves;
  if (soft > hard) {
    soft = hard;
  }

  EndgameResults *results = endgame_results_create();
  EndgameCtx *solver = NULL;
  Move *play = move_create();

  EndgameArgs args = {
      .game = game,
      .thread_control = config_get_thread_control(config),
      .plies = MAX_SEARCH_DEPTH,
      .tt_fraction_of_mem = 0.05,
      .initial_small_move_arena_size = DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
      .num_threads = threads,
      .num_top_moves = 1,
      .use_heuristics = true,
      .forced_pass_bypass = true,
      .enable_pv_display = false,
      .soft_time_limit = soft,
      .hard_time_limit = hard,
      .external_deadline_ns = ctimer_monotonic_ns() + (int64_t)(hard * 1e9),
      .seed = 42};

  Timer t;
  ctimer_start(&t);
  err = error_stack_create();
  endgame_solve(&solver, &args, results, err);
  const double used = ctimer_elapsed_seconds(&t);
  assert(error_stack_is_empty(err));
  error_stack_destroy(err);

  const PVLine *pv = endgame_results_get_pvline(results, ENDGAME_RESULT_BEST);
  int depth = 0;
  uint64_t nodes = endgame_ctx_get_nodes_searched(solver);
  uint64_t tiny = 0;
  if (pv->num_moves > 0) {
    depth = pv->negamax_depth;
    tiny = pv->moves[0].tiny_move;
    SmallMove best = pv->moves[0];
    small_move_to_move(play, &best, game_get_board(game));
    play_move(play, game, NULL);
  }

  const int s0 = equity_to_int(player_get_score(game_get_player(game, 0)));
  const int s1 = equity_to_int(player_get_score(game_get_player(game, 1)));
  const int now_on = game_get_player_on_turn_index(game);
  const int ended = game_get_game_end_reason(game) != GAME_END_REASON_NONE;
  char *out_cgp = game_get_cgp(game, true);
  printf("M1 s0=%d s1=%d onmove=%d ended=%d used=%.4f soft=%.4f depth=%d "
         "nodes=%llu tiny=%llu cgp=%s\n",
         s0, s1, now_on, ended, used, soft, depth, (unsigned long long)nodes,
         (unsigned long long)tiny, out_cgp);
  (void)fflush(stdout);

  free(out_cgp);
  move_destroy(play);
  endgame_ctx_destroy(solver);
  endgame_results_destroy(results);
  config_destroy(config);
}

// Reproducible corpus and solver measurements for root-search changes. Each
// record is a separate solve with a fresh TT; output includes values and moves
// so timing comparisons cannot silently hide changed answers.
void test_endgame_root_bench(void) {
  log_set_level(LOG_FATAL);
  const char *path = getenv("MAGPIE_ROOT_FILE");
  assert(path != NULL);
  const int count = env_int("MAGPIE_ROOT_COUNT", 64);
  const int threads = env_int("MAGPIE_ROOT_THREADS", 1);
  char settings[256];
  (void)snprintf(settings, sizeof(settings),
                 "set -lex CSW24 -threads %d -wmp true -rit true -wit true "
                 "-s1 equity -s2 equity",
                 threads);
  Config *config = config_create_or_die(settings);
  exec_config_quiet(config, "new");
  Game *game = config_get_game(config);
  if (env_int("MAGPIE_ROOT_GENERATE", 0)) {
    FILE *output = cfile_open(path, "we", false);
    assert(output != NULL);
    MoveList *moves = move_list_create(1);
    int found = 0;
    const int seed = env_int("MAGPIE_ROOT_SEED", 420000103);
    for (int attempt = 0; found < count && attempt < count * 100; attempt++) {
      game_reset(game);
      game_seed(game, (uint64_t)seed + (uint64_t)attempt);
      draw_starting_racks(game);
      if (!play_until_bag_empty(game, moves)) {
        continue;
      }
      if (env_int("MAGPIE_ROOT_SMALL", 0)) {
        while (
            rack_get_total_letters(player_get_rack(game_get_player(game, 0))) +
                    rack_get_total_letters(
                        player_get_rack(game_get_player(game, 1))) >
                6 &&
            game_get_game_end_reason(game) == GAME_END_REASON_NONE) {
          play_move(get_top_equity_move(game, moves), game, NULL);
        }
      }
      if (game_get_game_end_reason(game) != GAME_END_REASON_NONE) {
        continue;
      }
      char *cgp = game_get_cgp(game, true);
      const int written = fprintf(output, "%s\n", cgp);
      assert(written > 0);
      free(cgp);
      found++;
    }
    assert(found == count);
    const int close_status = fclose(output);
    assert(close_status == 0);
    move_list_destroy(moves);
    config_destroy(config);
    return;
  }
  FILE *input = cfile_open(path, "re", false);
  assert(input != NULL);
  char cgp[4096];
  for (int position = 0; position < count; position++) {
    const char *line = fgets(cgp, sizeof(cgp), input);
    assert(line != NULL);
    ErrorStack *errors = error_stack_create();
    game_load_cgp(game, cgp, errors);
    assert(error_stack_is_empty(errors));
    EndgameCtx *solver = NULL;
    EndgameResults *results = endgame_results_create();
    EndgameArgs args = {
        .game = game,
        .thread_control = config_get_thread_control(config),
        .plies = env_int("MAGPIE_ROOT_PLIES", 4),
        .tt_fraction_of_mem = 0.01,
        .initial_small_move_arena_size = DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
        .num_threads = threads,
        .num_top_moves = env_int("MAGPIE_ROOT_TOPK", 1),
        .use_heuristics = true,
        .forced_pass_bypass = true,
        .first_win = env_int("MAGPIE_ROOT_FIRSTWIN", 0) != 0,
        .hard_time_limit = env_double("MAGPIE_ROOT_SECONDS", 0),
        .seed = 42,
    };
    Timer timer;
    ctimer_start(&timer);
    if (args.hard_time_limit > 0) {
      args.external_deadline_ns =
          ctimer_monotonic_ns() + (int64_t)(args.hard_time_limit * 1e9);
    }
    endgame_solve(&solver, &args, results, errors);
    const double elapsed = ctimer_elapsed_seconds(&timer);
    assert(error_stack_is_empty(errors));
    const PVLine *pv = endgame_results_get_pvline(results, ENDGAME_RESULT_BEST);
    printf(
        "ROOTROW %d value=%d spread=%d depth=%d nodes=%llu seconds=%.9f "
        "move=%llu\n",
        position, pv->score,
        endgame_results_get_spread(results, ENDGAME_RESULT_BEST, game),
        endgame_results_get_depth(results, ENDGAME_RESULT_BEST),
        (unsigned long long)endgame_ctx_get_nodes_searched(solver), elapsed,
        (unsigned long long)(pv->num_moves > 0 ? pv->moves[0].tiny_move : 0));
    (void)fflush(stdout);
    endgame_ctx_destroy(solver);
    endgame_results_destroy(results);
    error_stack_destroy(errors);
  }
  const int close_status = fclose(input);
  assert(close_status == 0);
  config_destroy(config);
}
