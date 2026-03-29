#include "benchmark_endgame_test.h"

#include "../src/compat/ctime.h"
#include "../src/def/equity_defs.h"
#include "../src/def/game_defs.h"
#include "../src/def/game_history_defs.h"
#include "../src/def/move_defs.h"
#include "../src/def/thread_control_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/endgame_results.h"
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
#include "../src/str/game_string.h"
#include "../src/str/move_string.h"
#include "../src/str/rack_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <assert.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Execute config command quietly (suppress stdout during execution)
static void exec_config_quiet(Config *config, const char *cmd) {
  // Suppress stdout
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

  // Restore stdout
  (void)fflush(stdout);
  (void)dup2(saved_stdout, STDOUT_FILENO);
  close(saved_stdout);
}

// Play moves until the bag is empty, returning true if we get a valid endgame
// position (bag empty, both players have tiles)
static bool play_until_bag_empty(Game *game, MoveList *move_list) {
  while (bag_get_letters(game_get_bag(game)) > 0) {
    const Move *move = get_top_equity_move(game, 0, move_list);
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
      .thread_index = 0,
      .eq_margin_movegen = 0,
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
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
// Saves CGPs to /tmp/stuck_100pct_cgps.txt and partial-stuck to
// /tmp/stuck_partial_cgps.txt.
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

  FILE *fp_full = fopen("/tmp/stuck_100pct_cgps.txt", "we");
  FILE *fp_partial = fopen("/tmp/stuck_partial_cgps.txt", "we");
  FILE *fp_hard = fopen("/tmp/stuck_hard_endgame_cgps.txt", "we");
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
      const Move *move = get_top_equity_move(game, 0, move_list);
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
  printf("  100%% stuck: %d positions → /tmp/stuck_100pct_cgps.txt\n",
         found_full);
  printf("  50-99%% stuck: %d positions → /tmp/stuck_partial_cgps.txt\n",
         found_partial);
  printf("  any stuck at bag-empty: %d positions → "
         "/tmp/stuck_hard_endgame_cgps.txt\n",
         found_hard);
  printf("==============================================================\n");
  (void)fflush(stdout);

  move_list_destroy(move_list);
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

  FILE *fp = fopen(outfile, "we");
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
  generate_nonstuck_cgps(31415, "/tmp/nonstuck_cgps.txt");
}

void test_generate_nonstuck_cgps2(void) {
  log_set_level(LOG_FATAL);
  generate_nonstuck_cgps(271828, "/tmp/nonstuck_cgps2.txt");
}

// Core A/B benchmark: solve each CGP twice (with and without
// forced-pass bypass), comparing value and time pairwise.
static void run_ab_benchmark(const char *cgp_file, const char *label,
                             int old_ply, int new_ply, int max_positions) {
  FILE *fp = fopen(cgp_file, "re");
  if (!fp) {
    printf("No CGP file found at %s — run genstuck/gennonstuck first.\n",
           cgp_file);
    return;
  }

  Config *config =
      config_create_or_die("set -lex CSW21 -threads 6 -s1 score -s2 score");
  exec_config_quiet(config, "new");
  Game *game = config_get_game(config);

  EndgameSolver *solver_old = endgame_solver_create();
  EndgameSolver *solver_new = endgame_solver_create();
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
                        .use_tt_move_ordering = true};

    // --- OLD (no bypass) ---
    Timer t;
    ctimer_start(&t);
    err = error_stack_create();
    endgame_solve(solver_old, &args, results, err);
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
    endgame_solve(solver_new, &args, results, err);
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
  endgame_solver_destroy(solver_old);
  endgame_solver_destroy(solver_new);
  config_destroy(config);
}

void test_benchmark_forced_pass(void) {
  log_set_level(LOG_FATAL);
  run_ab_benchmark("/tmp/stuck_100pct_cgps.txt", "stuck", 3, 2, 500);
}

void test_benchmark_nonstuck(void) {
  log_set_level(LOG_FATAL);
  run_ab_benchmark("/tmp/nonstuck_cgps.txt", "nonstuck", 3, 2, 500);
}

void test_benchmark_nonstuck_3v3(void) {
  log_set_level(LOG_FATAL);
  run_ab_benchmark("/tmp/nonstuck_cgps.txt", "nonstuck", 3, 3, 500);
}

// A/B benchmark: compare endgame solving with and without TT-informed move
// ordering (MMST). Reads positions from a CGP file and solves under a time
// budget. The hard_time_limit triggers a mid-depth cutoff via depth_deadline_ns.
// Separate solvers prevent TT cross-contamination.
__attribute__((unused)) static void
run_mmst_benchmark(const char *cgp_file, const char *label, int max_positions,
                   int num_threads, int ply, double soft_limit,
                   double hard_limit) {
  FILE *fp = fopen(cgp_file, "re");
  if (!fp) {
    printf("No CGP file found at %s — run genstuck/gennonstuck first.\n",
           cgp_file);
    return;
  }

  Config *config =
      config_create_or_die("set -lex CSW21 -threads 1 -s1 score -s2 score");
  exec_config_quiet(config, "new");
  Game *game = config_get_game(config);

  char (*cgp_lines)[4096] = malloc((size_t)max_positions * 4096);
  assert(cgp_lines);
  int found = 0;
  while (found < max_positions && fgets(cgp_lines[found], 4096, fp)) {
    size_t len = strlen(cgp_lines[found]);
    if (len > 0 && cgp_lines[found][len - 1] == '\n') {
      cgp_lines[found][len - 1] = '\0';
    }
    if (strlen(cgp_lines[found]) > 0) {
      found++;
    }
  }
  (void)fclose(fp);

  printf("\n");
  printf("==============================================================\n");
  printf("  MMST Benchmark [%s]: %d positions\n", label, found);
  printf("  %d-ply (soft=%.2fs hard=%.2fs), %d threads, separate TTs (0.25 "
         "each)\n",
         ply, soft_limit, hard_limit, num_threads);
  printf("  Old: static heuristics only\n");
  printf("  New: MMST move ordering\n");
  printf("==============================================================\n");
  printf("  %4s  %8s %8s  %8s %8s  %6s\n", "Pos", "Old Val", "New Val",
         "Old Time", "New Time", "Delta");
  printf("  ----  -------- --------  -------- --------  ------\n");

  EndgameSolver *solver_old = endgame_solver_create();
  EndgameSolver *solver_new = endgame_solver_create();
  EndgameResults *results = endgame_results_create();

  double total_time_old = 0;
  double total_time_new = 0;
  int new_better = 0;
  int old_better = 0;
  int same = 0;
  int total_delta = 0;
  int solved = 0;

  for (int ci = 0; ci < found; ci++) {
    ErrorStack *err = error_stack_create();
    game_load_cgp(game, cgp_lines[ci], err);
    if (!error_stack_is_empty(err)) {
      error_stack_destroy(err);
      continue;
    }
    error_stack_destroy(err);

    // Alternate solve order to reduce systematic bias
    bool new_first = (ci % 2 == 0);

    int32_t val_old = 0;
    int32_t val_new = 0;
    double time_old = 0;
    double time_new = 0;

    for (int pass = 0; pass < 2; pass++) {
      bool is_new = (pass == 0) ? new_first : !new_first;
      EndgameSolver *solver = is_new ? solver_new : solver_old;

      EndgameArgs args = {.game = game,
                          .thread_control = config_get_thread_control(config),
                          .plies = ply,
                          .tt_fraction_of_mem = 0.25,
                          .initial_small_move_arena_size =
                              DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
                          .num_threads = num_threads,
                          .num_top_moves = 1,
                          .use_heuristics = true,
                          .forced_pass_bypass = true,
                          .soft_time_limit = soft_limit,
                          .hard_time_limit = hard_limit,
                          .use_tt_move_ordering = is_new};

      Timer timer;
      ctimer_start(&timer);
      err = error_stack_create();
      endgame_solve(solver, &args, results, err);
      double elapsed = ctimer_elapsed_seconds(&timer);
      assert(error_stack_is_empty(err));
      error_stack_destroy(err);

      int32_t val =
          endgame_results_get_pvline(results, ENDGAME_RESULT_BEST)->score;
      if (is_new) {
        val_new = val;
        time_new = elapsed;
      } else {
        val_old = val;
        time_old = elapsed;
      }
    }

    int delta = val_new - val_old;
    total_delta += delta;
    if (delta > 0) {
      new_better++;
    } else if (delta < 0) {
      old_better++;
    } else {
      same++;
    }

    printf("  %4d  %+8d %+8d  %7.3fs %7.3fs  %+5d\n", ci + 1, val_old,
           val_new, time_old, time_new, delta);
    total_time_old += time_old;
    total_time_new += time_new;
    solved++;

    if ((ci + 1) % 25 == 0) {
      (void)fflush(stdout);
    }
  }

  printf("  ----  -------- --------  -------- --------  ------\n");
  printf("\n");
  printf("  Results (%d positions [%s], %d-ply, soft=%.2fs hard=%.2fs):\n",
         solved, label, ply, soft_limit, hard_limit);
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
  endgame_solver_destroy(solver_old);
  endgame_solver_destroy(solver_new);
  config_destroy(config);
}

// Asymmetric A/B benchmark: old gets one time budget, new (MMST) gets another.
// Tests whether MMST at a smaller budget can match or beat baseline at a larger
// one.
static void run_mmst_asymmetric_benchmark(const char *cgp_file,
                                          const char *label, int max_positions,
                                          int num_threads, int ply,
                                          double old_soft, double old_hard,
                                          double new_soft, double new_hard) {
  FILE *fp = fopen(cgp_file, "re");
  if (!fp) {
    printf("No CGP file found at %s — run genstuck/gennonstuck first.\n",
           cgp_file);
    return;
  }

  Config *config =
      config_create_or_die("set -lex CSW21 -threads 1 -s1 score -s2 score");
  exec_config_quiet(config, "new");
  Game *game = config_get_game(config);

  char (*cgp_lines)[4096] = malloc((size_t)max_positions * 4096);
  assert(cgp_lines);
  int found = 0;
  while (found < max_positions && fgets(cgp_lines[found], 4096, fp)) {
    size_t len = strlen(cgp_lines[found]);
    if (len > 0 && cgp_lines[found][len - 1] == '\n') {
      cgp_lines[found][len - 1] = '\0';
    }
    if (strlen(cgp_lines[found]) > 0) {
      found++;
    }
  }
  (void)fclose(fp);

  printf("\n");
  printf("==============================================================\n");
  printf("  MMST Asymmetric Benchmark [%s]: %d positions\n", label, found);
  printf("  %d-ply, %d threads, separate TTs (0.25 each)\n", ply, num_threads);
  printf("  Old: static (soft=%.3fs hard=%.3fs)\n", old_soft, old_hard);
  printf("  New: MMST   (soft=%.3fs hard=%.3fs)\n", new_soft, new_hard);
  printf("==============================================================\n");
  printf("  %4s  %8s %8s  %8s %8s  %6s\n", "Pos", "Old Val", "New Val",
         "Old Time", "New Time", "Delta");
  printf("  ----  -------- --------  -------- --------  ------\n");

  EndgameSolver *solver_old = endgame_solver_create();
  EndgameSolver *solver_new = endgame_solver_create();
  EndgameResults *results = endgame_results_create();

  double total_time_old = 0;
  double total_time_new = 0;
  int new_better = 0;
  int old_better = 0;
  int same = 0;
  int total_delta = 0;
  int solved = 0;

  for (int ci = 0; ci < found; ci++) {
    ErrorStack *err = error_stack_create();
    game_load_cgp(game, cgp_lines[ci], err);
    if (!error_stack_is_empty(err)) {
      error_stack_destroy(err);
      continue;
    }
    error_stack_destroy(err);

    bool new_first = (ci % 2 == 0);

    int32_t val_old = 0;
    int32_t val_new = 0;
    double time_old = 0;
    double time_new = 0;

    for (int pass = 0; pass < 2; pass++) {
      bool is_new = (pass == 0) ? new_first : !new_first;
      EndgameSolver *solver = is_new ? solver_new : solver_old;
      double soft = is_new ? new_soft : old_soft;
      double hard = is_new ? new_hard : old_hard;

      EndgameArgs args = {.game = game,
                          .thread_control = config_get_thread_control(config),
                          .plies = ply,
                          .tt_fraction_of_mem = 0.25,
                          .initial_small_move_arena_size =
                              DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
                          .num_threads = num_threads,
                          .num_top_moves = 1,
                          .use_heuristics = true,
                          .forced_pass_bypass = true,
                          .soft_time_limit = soft,
                          .hard_time_limit = hard,
                          .use_tt_move_ordering = is_new};

      Timer timer;
      ctimer_start(&timer);
      err = error_stack_create();
      endgame_solve(solver, &args, results, err);
      double elapsed = ctimer_elapsed_seconds(&timer);
      assert(error_stack_is_empty(err));
      error_stack_destroy(err);

      int32_t val =
          endgame_results_get_pvline(results, ENDGAME_RESULT_BEST)->score;
      if (is_new) {
        val_new = val;
        time_new = elapsed;
      } else {
        val_old = val;
        time_old = elapsed;
      }
    }

    int delta = val_new - val_old;
    total_delta += delta;
    if (delta > 0) {
      new_better++;
    } else if (delta < 0) {
      old_better++;
    } else {
      same++;
    }

    printf("  %4d  %+8d %+8d  %7.3fs %7.3fs  %+5d\n", ci + 1, val_old,
           val_new, time_old, time_new, delta);
    total_time_old += time_old;
    total_time_new += time_new;
    solved++;

    if ((ci + 1) % 25 == 0) {
      (void)fflush(stdout);
    }
  }

  printf("  ----  -------- --------  -------- --------  ------\n");
  printf("\n");
  printf("  Results (%d positions [%s], %d-ply):\n", solved, label, ply);
  printf("    Old: soft=%.3fs hard=%.3fs | New (MMST): soft=%.3fs hard=%.3fs\n",
         old_soft, old_hard, new_soft, new_hard);
  printf("    New better: %d  |  Old better: %d  |  Same: %d\n", new_better,
         old_better, same);
  printf("    Total value delta: %+d (avg %+.2f per position)\n", total_delta,
         solved > 0 ? (double)total_delta / solved : 0.0);
  printf("    Old total time: %.3fs (avg %.3fs)\n", total_time_old,
         solved > 0 ? total_time_old / solved : 0.0);
  printf("    New total time: %.3fs (avg %.3fs)\n", total_time_new,
         solved > 0 ? total_time_new / solved : 0.0);
  printf("==============================================================\n");
  (void)fflush(stdout);

  free(cgp_lines);
  endgame_results_destroy(results);
  endgame_solver_destroy(solver_old);
  endgame_solver_destroy(solver_new);
  config_destroy(config);
}

void test_benchmark_tt_move_ordering(void) {
  log_set_level(LOG_FATAL);
  // MMST at 100ms vs baseline at 2s, 4 threads (20:1 budget ratio), stuck
  run_mmst_asymmetric_benchmark(
      "/tmp/stuck_100pct_cgps.txt", "stuck-100pct 100ms-MMST vs 2s-old", 250,
      4, 25, 1.0, 2.0, 0.05, 0.1);
}

// Benchmark startup overhead: solve many positions with a tiny time budget
// to measure the fraction of time spent in setup vs. search.
void test_benchmark_startup(void) {
  log_set_level(LOG_FATAL);

  FILE *fp = fopen("/tmp/nonstuck_cgps.txt", "re");
  if (!fp) {
    printf("No CGP file found — run gennonstuck first.\n");
    return;
  }

  int max_positions = 50;
  char (*cgp_lines)[4096] = malloc((size_t)max_positions * 4096);
  assert(cgp_lines);
  int found = 0;
  while (found < max_positions && fgets(cgp_lines[found], 4096, fp)) {
    size_t len = strlen(cgp_lines[found]);
    if (len > 0 && cgp_lines[found][len - 1] == '\n') {
      cgp_lines[found][len - 1] = '\0';
    }
    if (strlen(cgp_lines[found]) > 0) {
      found++;
    }
  }
  (void)fclose(fp);

  Config *config =
      config_create_or_die("set -lex CSW21 -threads 1 -s1 score -s2 score");
  exec_config_quiet(config, "new");
  Game *game = config_get_game(config);

  EndgameSolver *solver = endgame_solver_create();
  EndgameResults *results = endgame_results_create();

  printf("\n");
  printf("==============================================================\n");
  printf("  Startup Benchmark: %d positions, 25ms budget, 4 threads\n", found);
  printf("==============================================================\n");
  printf("  %4s  %8s  %8s\n", "Pos", "Time", "Value");
  printf("  ----  --------  --------\n");

  double total_time = 0;
  for (int ci = 0; ci < found; ci++) {
    ErrorStack *err = error_stack_create();
    game_load_cgp(game, cgp_lines[ci], err);
    if (!error_stack_is_empty(err)) {
      error_stack_destroy(err);
      continue;
    }
    error_stack_destroy(err);

    EndgameArgs args = {.game = game,
                        .thread_control = config_get_thread_control(config),
                        .plies = 25,
                        .tt_fraction_of_mem = 0.25,
                        .initial_small_move_arena_size =
                            DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
                        .num_threads = 4,
                        .num_top_moves = 1,
                        .use_heuristics = true,
                        .forced_pass_bypass = true,
                        .soft_time_limit = 0.0125,
                        .hard_time_limit = 0.025,
                        .use_tt_move_ordering = true};

    Timer timer;
    ctimer_start(&timer);
    err = error_stack_create();
    endgame_solve(solver, &args, results, err);
    double elapsed = ctimer_elapsed_seconds(&timer);
    assert(error_stack_is_empty(err));
    error_stack_destroy(err);

    int32_t val =
        endgame_results_get_pvline(results, ENDGAME_RESULT_BEST)->score;
    printf("  %4d  %7.3fs  %+8d\n", ci + 1, elapsed, val);
    total_time += elapsed;
  }

  printf("  ----  --------  --------\n");
  printf("  Total: %.3fs, Avg: %.3fs\n", total_time,
         found > 0 ? total_time / found : 0.0);
  printf("==============================================================\n");
  (void)fflush(stdout);

  free(cgp_lines);
  endgame_results_destroy(results);
  endgame_solver_destroy(solver);
  config_destroy(config);
}

// Tournament with game-pair playout: two solvers alternate moves from the same
// position until game end. Each matchup plays two games per position (swap who
// goes first) so position advantage cancels out. Final spread determines winner.
enum {
  TOURN_MAX_PLAYERS = 8,
};

typedef struct {
  const char *label;
  bool use_mmst;
  double soft_limit;
  double hard_limit;
} TournPlayer;

typedef struct {
  int wins;
  int losses;
  int draws;
  int spread;
  double time_used;
} HeadToHead;

// Compute Elo ratings from pairwise results using iterative optimization.
// Anchors player 0 at Elo 0. Fills elo_out[num_players].
static void compute_elo_ratings(const HeadToHead *h2h, int num_players,
                                double *elo_out) {
  for (int p = 0; p < num_players; p++) {
    elo_out[p] = 0.0;
  }
  for (int iter = 0; iter < 200; iter++) {
    double new_elo[TOURN_MAX_PLAYERS];
    for (int p = 0; p < num_players; p++) {
      new_elo[p] = elo_out[p];
    }
    for (int player_idx = 1; player_idx < num_players; player_idx++) {
      double actual_score = 0.0;
      double expected_score = 0.0;
      double total_games = 0.0;
      for (int opp_idx = 0; opp_idx < num_players; opp_idx++) {
        if (opp_idx == player_idx) {
          continue;
        }
        const HeadToHead *match = &h2h[player_idx * num_players + opp_idx];
        double games = (double)(match->wins + match->losses + match->draws);
        if (games == 0) {
          continue;
        }
        actual_score += (double)match->wins + 0.5 * (double)match->draws;
        double exp_pct =
            1.0 / (1.0 + pow(10.0, (elo_out[opp_idx] - elo_out[player_idx]) /
                                        400.0));
        expected_score += exp_pct * games;
        total_games += games;
      }
      if (total_games > 0 && expected_score > 0.001) {
        double ratio = actual_score / expected_score;
        double adjustment = 400.0 * log10(ratio);
        new_elo[player_idx] = elo_out[player_idx] + adjustment * 0.2;
      }
    }
    for (int p = 0; p < num_players; p++) {
      elo_out[p] = new_elo[p];
    }
  }
}

// Play one endgame: solver_a controls starting_side, solver_b controls the
// other. Returns final spread from solver_a's perspective.
// time_a/time_b accumulate solve time for each side.
// If move_log is non-NULL, appends move descriptions to it.
static int play_one_endgame(Game *game_copy, int starting_side,
                            EndgameSolver *solver_a, EndgameSolver *solver_b,
                            const TournPlayer *player_a,
                            const TournPlayer *player_b,
                            ThreadControl *thread_control,
                            EndgameResults *results, double *time_a,
                            double *time_b, StringBuilder *move_log) {
  *time_a = 0;
  *time_b = 0;

  const LetterDistribution *ld = game_get_ld(game_copy);

  if (move_log) {
    int start_a = equity_to_int(
        player_get_score(game_get_player(game_copy, starting_side)));
    int start_b = equity_to_int(
        player_get_score(game_get_player(game_copy, 1 - starting_side)));
    string_builder_add_formatted_string(move_log, "Start: A=%d B=%d | ",
                                        start_a, start_b);
  }

  // Safety limit to avoid infinite loops (e.g., consecutive scoreless turns)
  for (int move_count = 0; move_count < 50 && !game_over(game_copy);
       move_count++) {
    int side = game_get_player_on_turn_index(game_copy);
    bool is_a = (side == starting_side);
    EndgameSolver *solver = is_a ? solver_a : solver_b;
    const TournPlayer *player = is_a ? player_a : player_b;

    EndgameArgs args = {.game = game_copy,
                        .thread_control = thread_control,
                        .plies = 25,
                        .tt_fraction_of_mem = 0.08,
                        .initial_small_move_arena_size =
                            DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
                        .num_threads = 4,
                        .num_top_moves = 1,
                        .use_heuristics = true,
                        .forced_pass_bypass = true,
                        .soft_time_limit = player->soft_limit,
                        .hard_time_limit = player->hard_limit,
                        .use_tt_move_ordering = player->use_mmst};

    Timer timer;
    ctimer_start(&timer);
    ErrorStack *err = error_stack_create();
    endgame_solve(solver, &args, results, err);
    double elapsed = ctimer_elapsed_seconds(&timer);
    assert(error_stack_is_empty(err));
    error_stack_destroy(err);

    if (is_a) {
      *time_a += elapsed;
    } else {
      *time_b += elapsed;
    }

    // Extract best move from PV and play it
    const PVLine *pv =
        endgame_results_get_pvline(results, ENDGAME_RESULT_BEST);
    if (pv->num_moves == 0) {
      break;
    }
    Move move;
    small_move_to_move(&move, &pv->moves[0], game_get_board(game_copy));

    if (move_log) {
      if (move_count > 0) {
        string_builder_add_string(move_log, " ");
      }
      string_builder_add_formatted_string(move_log, "[%s] ",
                                          is_a ? "A" : "B");
      string_builder_add_move(move_log, game_get_board(game_copy), &move, ld,
                              true);
    }

    play_move(&move, game_copy, NULL);
  }

  // Final spread from solver_a's perspective
  int score_a = equity_to_int(
      player_get_score(game_get_player(game_copy, starting_side)));
  int score_b = equity_to_int(
      player_get_score(game_get_player(game_copy, 1 - starting_side)));

  if (move_log) {
    game_end_reason_t end_reason = game_get_game_end_reason(game_copy);
    if (end_reason == GAME_END_REASON_STANDARD) {
      // One player went out — find who has tiles left
      const Rack *rack_a =
          player_get_rack(game_get_player(game_copy, starting_side));
      const Rack *rack_b =
          player_get_rack(game_get_player(game_copy, 1 - starting_side));
      const Rack *loser_rack = rack_is_empty(rack_a) ? rack_b : rack_a;
      const char *loser_label = rack_is_empty(rack_a) ? "B" : "A";
      int rack_pts = equity_to_int(rack_get_score(ld, loser_rack));
      string_builder_add_formatted_string(move_log, " | %s left: ", loser_label);
      string_builder_add_rack(move_log, loser_rack, ld, false);
      string_builder_add_formatted_string(move_log, " (%d pts, +%d adj)",
                                          rack_pts, 2 * rack_pts);
    } else if (end_reason == GAME_END_REASON_CONSECUTIVE_ZEROS) {
      string_builder_add_string(move_log, " | consecutive zeros");
    }
    string_builder_add_formatted_string(move_log, " | Final: A=%d B=%d (%+d)",
                                        score_a, score_b, score_a - score_b);
  }

  return score_a - score_b;
}

void test_benchmark_tournament(void) {
  log_set_level(LOG_FATAL);

  FILE *fp = fopen("/tmp/nonstuck_cgps.txt", "re");
  if (!fp) {
    printf("No CGP file found — run gennonstuck first.\n");
    return;
  }

  const int max_positions = 100;
  char (*cgp_lines)[4096] = malloc((size_t)max_positions * 4096);
  assert(cgp_lines);
  int found = 0;
  while (found < max_positions && fgets(cgp_lines[found], 4096, fp)) {
    size_t len = strlen(cgp_lines[found]);
    if (len > 0 && cgp_lines[found][len - 1] == '\n') {
      cgp_lines[found][len - 1] = '\0';
    }
    if (strlen(cgp_lines[found]) > 0) {
      found++;
    }
  }
  (void)fclose(fp);

  const int num_players = 2;
  TournPlayer players[2] = {
      {"old-0.2s", false, 0.1, 0.2},
      {"new-0.2s", true, 0.1, 0.2},
  };

  // Number of pairwise matchups
  const int num_pairs = num_players * (num_players - 1) / 2;

  printf("\n");
  printf("================================================================"
         "==============\n");
  printf("  Game-Pair Tournament: %d positions, %d players, %d matchups\n",
         found, num_players, num_pairs);
  printf("  4 threads, 25-ply, game pairs (swap sides per position)\n");
  printf("  Players: ");
  for (int p = 0; p < num_players; p++) {
    printf("%s%s", players[p].label, p < num_players - 1 ? ", " : "\n");
  }
  printf("================================================================"
         "==============\n");
  (void)fflush(stdout);

  Config *config =
      config_create_or_die("set -lex CSW21 -threads 1 -s1 score -s2 score");
  exec_config_quiet(config, "new");
  Game *game = config_get_game(config);

  // Two solvers per pair (reused across positions via persistent workers)
  EndgameSolver *solver_pool[2];
  solver_pool[0] = endgame_solver_create();
  solver_pool[1] = endgame_solver_create();
  EndgameResults *results = endgame_results_create();

  // Pairwise head-to-head: h2h[a * num_players + b] = a's record vs b
  HeadToHead h2h[TOURN_MAX_PLAYERS * TOURN_MAX_PLAYERS];
  memset(h2h, 0, sizeof(h2h));

  int pair_idx = 0;
  for (int a = 0; a < num_players; a++) {
    for (int b = a + 1; b < num_players; b++) {
      pair_idx++;
      printf("\n  Matchup %d/%d: %s vs %s\n", pair_idx, num_pairs,
             players[a].label, players[b].label);
      (void)fflush(stdout);

      HeadToHead *ab = &h2h[a * num_players + b];
      HeadToHead *ba = &h2h[b * num_players + a];

      int positions_done = 0;
      for (int ci = 0; ci < found; ci++) {
        ErrorStack *err = error_stack_create();
        game_load_cgp(game, cgp_lines[ci], err);
        if (!error_stack_is_empty(err)) {
          error_stack_destroy(err);
          continue;
        }
        error_stack_destroy(err);

        int starting_side = game_get_player_on_turn_index(game);

        // Game 1: A controls starting side
        StringBuilder *log1 = string_builder_create();
        Game *g1 = game_duplicate(game);
        double time_a1 = 0;
        double time_b1 = 0;
        int spread1 = play_one_endgame(
            g1, starting_side, solver_pool[0], solver_pool[1], &players[a],
            &players[b], config_get_thread_control(config), results, &time_a1,
            &time_b1, log1);
        game_destroy(g1);

        // Game 2: B controls starting side (swap)
        StringBuilder *log2 = string_builder_create();
        Game *g2 = game_duplicate(game);
        double time_a2 = 0;
        double time_b2 = 0;
        int spread2 = play_one_endgame(
            g2, starting_side, solver_pool[1], solver_pool[0], &players[b],
            &players[a], config_get_thread_control(config), results, &time_a2,
            &time_b2, log2);
        game_destroy(g2);

        // Net spread from A's perspective: game1 spread + (-game2 spread)
        int net_spread = spread1 - spread2;

        // Print position + moves when the pair disagrees
        if (net_spread != 0) {
          StringBuilder *board_sb = string_builder_create();
          GameStringOptions *gso = game_string_options_create_default();
          string_builder_add_game(game, NULL, gso, NULL, board_sb);
          game_string_options_destroy(gso);

          printf("\n    === Position %d (net %+d for %s) ===\n",
                 positions_done + 1, net_spread, players[a].label);
          printf("%s\n", string_builder_peek(board_sb));
          printf("    Game 1 (A=%s first, spread %+d):\n      %s\n",
                 players[a].label, spread1, string_builder_peek(log1));
          printf("    Game 2 (B=%s first, spread %+d):\n      %s\n",
                 players[b].label, spread2, string_builder_peek(log2));
          (void)fflush(stdout);
          string_builder_destroy(board_sb);
        }
        string_builder_destroy(log1);
        string_builder_destroy(log2);
        ab->spread += net_spread;
        ba->spread -= net_spread;
        ab->time_used += time_a1 + time_a2;
        ba->time_used += time_b1 + time_b2;

        if (net_spread > 0) {
          ab->wins++;
          ba->losses++;
        } else if (net_spread < 0) {
          ab->losses++;
          ba->wins++;
        } else {
          ab->draws++;
          ba->draws++;
        }

        positions_done++;
        if (positions_done % 50 == 0) {
          printf("    %d/%d (A: %d-%d=%d, spread %+d)\n", positions_done,
                 found, ab->wins, ab->losses, ab->draws, ab->spread);
          (void)fflush(stdout);
        }
      }

      printf("    Final: %s %d-%d=%d (%+d spread) vs %s\n", players[a].label,
             ab->wins, ab->losses, ab->draws, ab->spread, players[b].label);
      (void)fflush(stdout);
    }
  }

  // Compute Elo ratings
  double elo[TOURN_MAX_PLAYERS];
  compute_elo_ratings(h2h, num_players, elo);

  // Print per-player summary
  printf("\n");
  printf("  %-10s  %6s  %8s  %6s  %6s  %6s\n", "Player", "Elo", "Spread",
         "Wins", "Losses", "Draws");
  printf("  ----------  ------  --------  ------  ------  ------\n");
  for (int p = 0; p < num_players; p++) {
    int total_wins = 0;
    int total_losses = 0;
    int total_draws = 0;
    int total_spread = 0;
    for (int opp = 0; opp < num_players; opp++) {
      if (opp == p) {
        continue;
      }
      total_wins += h2h[p * num_players + opp].wins;
      total_losses += h2h[p * num_players + opp].losses;
      total_draws += h2h[p * num_players + opp].draws;
      total_spread += h2h[p * num_players + opp].spread;
    }
    printf("  %-10s  %+6.0f  %+8d  %6d  %6d  %6d\n", players[p].label,
           elo[p], total_spread, total_wins, total_losses, total_draws);
  }

  // Print head-to-head matrix
  printf("\n  Head-to-head (row vs col): W-L=D, spread, time\n\n");
  printf("  %-10s", "");
  for (int b_idx = 0; b_idx < num_players; b_idx++) {
    printf("  %-28s", players[b_idx].label);
  }
  printf("\n");

  for (int a_idx = 0; a_idx < num_players; a_idx++) {
    printf("  %-10s", players[a_idx].label);
    for (int b_idx = 0; b_idx < num_players; b_idx++) {
      if (a_idx == b_idx) {
        printf("  %-28s", "---");
      } else {
        const HeadToHead *match = &h2h[a_idx * num_players + b_idx];
        char buf[40];
        snprintf(buf, sizeof(buf), "%d-%d=%d %+d %.0fs", match->wins,
                 match->losses, match->draws, match->spread, match->time_used);
        printf("  %-28s", buf);
      }
    }
    printf("\n");
  }

  printf("\n  %d positions, %d game pairs per matchup.\n", found, found);
  printf("================================================================"
         "==============\n");
  (void)fflush(stdout);

  free(cgp_lines);
  endgame_results_destroy(results);
  endgame_solver_destroy(solver_pool[0]);
  endgame_solver_destroy(solver_pool[1]);
  config_destroy(config);
}
