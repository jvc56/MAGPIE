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
#include "../src/util/io_util.h"
#include "test_util.h"
#include <assert.h>
#include <fcntl.h>
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

  Config *config = config_create_or_die(
      "set -lex CSW21 -threads 1 -s1 score -s2 score -r1 small -r2 small");

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

  for (int i = 0;
       (found_full < target_full || found_partial < target_partial ||
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
void test_generate_nonstuck_cgps(void) {
  log_set_level(LOG_FATAL);

  Config *config = config_create_or_die(
      "set -lex CSW21 -threads 1 -s1 score -s2 score -r1 small -r2 small");

  MoveList *move_list = move_list_create(1);
  exec_config_quiet(config, "new");
  Game *game = config_get_game(config);

  const int target = 500;
  const uint64_t base_seed = 31415;
  const int max_attempts = 100000;

  FILE *fp = fopen("/tmp/nonstuck_cgps.txt", "we");
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
  printf("  0%% stuck: %d positions -> /tmp/nonstuck_cgps.txt\n", found);
  printf("==============================================================\n");
  (void)fflush(stdout);

  move_list_destroy(move_list);
  config_destroy(config);
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

  Config *config = config_create_or_die(
      "set -lex CSW21 -threads 6 -s1 score -s2 score -r1 small -r2 small");
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
                        .forced_pass_bypass = false};

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
