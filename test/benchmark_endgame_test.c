#include "benchmark_endgame_test.h"

#include "../src/compat/ctime.h"
#include "../src/def/equity_defs.h"
#include "../src/def/game_defs.h"
#include "../src/def/game_history_defs.h"
#include "../src/def/letter_distribution_defs.h"
#include "../src/def/move_defs.h"
#include "../src/def/thread_control_defs.h"
#include "../src/ent/analysis_trace.h"
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
#include "../src/impl/endgame_admission_model.h"
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

static int env_int(const char *name, int fallback);

static uint64_t tiles_bv_from_small_moves(const MoveList *move_list) {
  uint64_t tiles_bv = 0;
  for (int move_idx = 0; move_idx < move_list->count; move_idx++) {
    const SmallMove *move = move_list->small_moves[move_idx];
    for (int tile_idx = 0; tile_idx < small_move_get_tiles_played(move);
         tile_idx++) {
      const uint64_t tiny_move = move->tiny_move;
      const bool is_blank = (tiny_move & ((uint64_t)1 << (12 + tile_idx))) != 0;
      const MachineLetter ml =
          (tiny_move >> (20 + 6 * tile_idx)) & (MachineLetter)63;
      tiles_bv |= is_blank ? 1 : ((uint64_t)1 << ml);
    }
  }
  return tiles_bv;
}

static uint64_t tiles_bv_from_rack(const Rack *rack) {
  uint64_t tiles_bv = 0;
  const uint16_t dist_size = rack_get_dist_size(rack);
  for (uint16_t ml = 0; ml < dist_size; ml++) {
    if (rack_get_letter(rack, ml) > 0) {
      tiles_bv |= (uint64_t)1 << ml;
    }
  }
  return tiles_bv;
}

// Isolated MOVE_RECORD_TILES_PLAYED benchmark. This deliberately excludes CGP
// loading from the timed region and invokes both players so it measures only
// the move-generation mode used by endgame stuck-tile detection. Every input
// position is independently verified to have at least one stuck rack before
// any timed rounds run.
//
// Environment:
//   MAGPIE_TP_CGP      CGP file (default /tmp/stuck_hard_endgame_cgps.txt)
//   MAGPIE_TP_MAX      positions (default 64)
//   MAGPIE_TP_REPEATS  calls per player/position/round (default 50)
//   MAGPIE_TP_ROUNDS   rounds (default 9)
void test_tiles_played_bench(void) {
  log_set_level(LOG_FATAL);

  const char *cgp_file = getenv("MAGPIE_TP_CGP");
  if (cgp_file == NULL || cgp_file[0] == '\0') {
    cgp_file = "/tmp/stuck_hard_endgame_cgps.txt";
  }
  const int max_positions = env_int("MAGPIE_TP_MAX", 64);
  const int repeats = env_int("MAGPIE_TP_REPEATS", 50);
  const int rounds = env_int("MAGPIE_TP_ROUNDS", 9);

  FILE *fp = fopen(cgp_file, "re");
  if (!fp) {
    printf("TPBENCHERR no CGP file at %s; run genstuck first\n", cgp_file);
    return;
  }
  char (*cgp_lines)[4096] = malloc((size_t)max_positions * 4096);
  assert(cgp_lines);
  int num_cgps = 0;
  while (num_cgps < max_positions && fgets(cgp_lines[num_cgps], 4096, fp)) {
    size_t len = strlen(cgp_lines[num_cgps]);
    if (len > 0 && cgp_lines[num_cgps][len - 1] == '\n') {
      cgp_lines[num_cgps][len - 1] = '\0';
    }
    if (cgp_lines[num_cgps][0] != '\0') {
      num_cgps++;
    }
  }
  (void)fclose(fp);

  Config *config =
      config_create_or_die("set -lex CSW21 -threads 1 -s1 score -s2 score");
  exec_config_quiet(config, "new");
  Game *game = config_get_game(config);
  MoveList *move_list = move_list_create_small(1);
  MoveList *reference_moves = move_list_create_small(250000);

  // Compare against the union of rack tiles in every ALL_SMALL move. This is
  // slower than the production mode but provides an independent correctness
  // oracle for every player/position in the benchmark battery.
  int comparisons = 0;
  for (int ci = 0; ci < num_cgps; ci++) {
    ErrorStack *err = error_stack_create();
    game_load_cgp(game, cgp_lines[ci], err);
    assert(error_stack_is_empty(err));
    error_stack_destroy(err);
    const int saved_on_turn = game_get_player_on_turn_index(game);
    bool position_has_stuck_rack = false;
    for (int player_idx = 0; player_idx < 2; player_idx++) {
      game_set_player_on_turn_index(game, player_idx);
      const MoveGenArgs reference_args = {
          .game = game,
          .move_list = reference_moves,
          .move_record_type = MOVE_RECORD_ALL_SMALL,
          .move_sort_type = MOVE_SORT_SCORE,
          .override_kwg = NULL,
          .eq_margin_movegen = 0,
          .target_equity = EQUITY_MAX_VALUE,
          .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
      };
      generate_moves(&reference_args);
      const uint64_t reference_bv = tiles_bv_from_small_moves(reference_moves);
      const Rack *rack = player_get_rack(game_get_player(game, player_idx));
      const uint64_t rack_bv = tiles_bv_from_rack(rack);
      if ((reference_bv & rack_bv) != rack_bv) {
        position_has_stuck_rack = true;
      }

      const uint64_t initial_bv = reference_bv & UINT64_C(0xAAAAAAAAAAAAAAAA);
      uint64_t actual_bv = 0;
      const MoveGenArgs actual_args = {
          .game = game,
          .move_list = move_list,
          .move_record_type = MOVE_RECORD_TILES_PLAYED,
          .move_sort_type = MOVE_SORT_SCORE,
          .override_kwg = NULL,
          .eq_margin_movegen = 0,
          .target_equity = EQUITY_MAX_VALUE,
          .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
          .tiles_played_bv = &actual_bv,
          .initial_tiles_bv = initial_bv,
      };
      generate_moves(&actual_args);
      assert(actual_bv == (initial_bv | reference_bv));
      comparisons++;
    }
    assert(position_has_stuck_rack);
    game_set_player_on_turn_index(game, saved_on_turn);
  }
  printf("TPBENCH verified_stuck_positions=%d correctness=%d\n", num_cgps,
         comparisons);

  for (int round = 0; round < rounds; round++) {
    double total_time = 0.0;
    uint64_t checksum = 0;
    int calls = 0;
    for (int ci = 0; ci < num_cgps; ci++) {
      ErrorStack *err = error_stack_create();
      game_load_cgp(game, cgp_lines[ci], err);
      assert(error_stack_is_empty(err));
      error_stack_destroy(err);
      const int saved_on_turn = game_get_player_on_turn_index(game);
      for (int player_idx = 0; player_idx < 2; player_idx++) {
        game_set_player_on_turn_index(game, player_idx);
        for (int repeat = 0; repeat < repeats; repeat++) {
          uint64_t tiles_bv = 0;
          const MoveGenArgs args = {
              .game = game,
              .move_list = move_list,
              .move_record_type = MOVE_RECORD_TILES_PLAYED,
              .move_sort_type = MOVE_SORT_SCORE,
              .override_kwg = NULL,
              .eq_margin_movegen = 0,
              .target_equity = EQUITY_MAX_VALUE,
              .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
              .tiles_played_bv = &tiles_bv,
          };
          Timer timer;
          ctimer_start(&timer);
          generate_moves(&args);
          total_time += ctimer_elapsed_seconds(&timer);
          checksum += tiles_bv;
          calls++;
        }
      }
      game_set_player_on_turn_index(game, saved_on_turn);
    }
    printf("TPBENCH round=%d positions=%d calls=%d time=%.6f checksum=%llu\n",
           round + 1, num_cgps, calls, total_time,
           (unsigned long long)checksum);
    (void)fflush(stdout);
  }

  small_move_list_destroy(move_list);
  small_move_list_destroy(reference_moves);
  config_destroy(config);
  free(cgp_lines);
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
// Parameterized entirely by environment so the same binary can sweep depth /
// thread count / battery without recompiling:
//   MAGPIE_BENCH_CGP     CGP file           (default /tmp/nonstuck_cgps.txt)
//   MAGPIE_BENCH_LEX     lexicon            (default CSW21)
//   MAGPIE_BENCH_PLIES   endgame plies      (default 4)
//   MAGPIE_BENCH_THREADS solver threads     (default 1  -> deterministic nodes)
//   MAGPIE_BENCH_MAX     max positions      (default 100)
//   MAGPIE_BENCH_TAG     label for the run  (default "bench")
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

// Generate a persistent, modest-complexity endgame corpus for calibrating the
// value of additional search work. Each source game contributes at most one
// position, at a deterministically varied total-rack size, which avoids
// treating several adjacent turns from one game as independent observations.
//
// Environment:
//   MAGPIE_EG_CURVE_CGP        output (default obj/endgame-value-cgps.txt)
//   MAGPIE_EG_CURVE_COUNT      positions (default 40)
//   MAGPIE_EG_CURVE_MIN_TILES  minimum total rack tiles (default 6)
//   MAGPIE_EG_CURVE_MAX_TILES  maximum total rack tiles (default 10)
//   MAGPIE_EG_CURVE_SEED       first game seed (default 7292026)
void test_generate_endgame_curve_cgps(void) {
  log_set_level(LOG_FATAL);
  const char *outfile = getenv("MAGPIE_EG_CURVE_CGP");
  if (outfile == NULL || outfile[0] == '\0') {
    outfile = "obj/endgame-value-cgps.txt";
  }
  const int target_count = env_int("MAGPIE_EG_CURVE_COUNT", 40);
  const int min_tiles = env_int("MAGPIE_EG_CURVE_MIN_TILES", 6);
  const int max_tiles = env_int("MAGPIE_EG_CURVE_MAX_TILES", 10);
  const uint64_t base_seed = (uint64_t)env_int("MAGPIE_EG_CURVE_SEED", 7292026);
  assert(target_count > 0);
  assert(min_tiles >= 2);
  assert(max_tiles >= min_tiles);

  Config *config =
      config_create_or_die("set -lex CSW24 -threads 1 -s1 score -s2 score");
  exec_config_quiet(config, "new");
  Game *game = config_get_game(config);
  MoveList *move_list = move_list_create(1);
  FILE *fp = fopen(outfile, "we");
  assert(fp);

  int found = 0;
  const int tile_span = max_tiles - min_tiles + 1;
  for (int attempt = 0; found < target_count && attempt < target_count * 100;
       attempt++) {
    game_reset(game);
    game_seed(game, base_seed + (uint64_t)attempt);
    draw_starting_racks(game);
    if (!play_until_bag_empty(game, move_list)) {
      continue;
    }

    const int wanted = min_tiles + (found % tile_span);
    while (game_get_game_end_reason(game) == GAME_END_REASON_NONE) {
      const Rack *rack0 = player_get_rack(game_get_player(game, 0));
      const Rack *rack1 = player_get_rack(game_get_player(game, 1));
      const int total_tiles = (int)rack_get_total_letters(rack0) +
                              (int)rack_get_total_letters(rack1);
      if (total_tiles == wanted && !rack_is_empty(rack0) &&
          !rack_is_empty(rack1)) {
        char *cgp = game_get_cgp(game, true);
        (void)fprintf(fp, "%s\n", cgp);
        free(cgp);
        found++;
        break;
      }
      if (total_tiles < wanted) {
        // This game skipped the requested rack total; keep the corpus
        // stratified by trying the same `wanted` value in the next game.
        break;
      }
      const Move *move = get_top_equity_move(game, move_list);
      play_move(move, game, NULL);
    }
  }
  (void)fclose(fp);
  printf("EGCURVEGEN positions=%d requested=%d tiles=%d..%d seed=%llu "
         "file=%s\n",
         found, target_count, min_tiles, max_tiles,
         (unsigned long long)base_seed, outfile);
  (void)fflush(stdout);

  move_list_destroy(move_list);
  config_destroy(config);
}

typedef struct EndgameCurvePoint {
  int depth;
  uint64_t nodes;
  int64_t elapsed_ns;
  int64_t cpu_ns;
  int root_completed;
  int root_total;
  int ply2_completed;
  int ply2_total;
  uint64_t tiny_move;
  int32_t search_value;
} EndgameCurvePoint;

enum { ENDGAME_CURVE_MAX_POINTS = MAX_SEARCH_DEPTH + 1 };

typedef struct EndgameCurveTrace {
  EndgameCurvePoint points[ENDGAME_CURVE_MAX_POINTS];
  int count;
  AnalysisProgressEvent admission_points[ENDGAME_CURVE_MAX_POINTS];
  int admission_count;
  SmallMove *root_moves;
  int root_move_count;
  analysis_status_t finish_status;
} EndgameCurveTrace;

typedef struct EndgameCurveAdmissionPolicy {
  bool use_time_manager;
  EndgameAdmissionPolicy prediction;
  EndgameAdmissionTimeManagerPolicy time_manager;
} EndgameCurveAdmissionPolicy;

static EndgameDepthAdmissionDecision
endgame_curve_admission_callback(const EndgameDepthAdmissionRequest *request,
                                 void *user_data) {
  EndgameCurveAdmissionPolicy *policy = user_data;
  if (policy->use_time_manager && request->next_depth == 5) {
    return endgame_admission_time_manager_callback(request,
                                                   &policy->time_manager);
  }
  return endgame_admission_model_callback(request, &policy->prediction);
}

static void endgame_curve_record(const AnalysisProgressEvent *event,
                                 void *user_data) {
  if (event->mode != ANALYSIS_MODE_ENDGAME) {
    return;
  }
  EndgameCurveTrace *trace = user_data;
  if (event->event == ANALYSIS_EVENT_FINISH) {
    trace->finish_status = event->status;
    return;
  }
  if (event->event == ANALYSIS_EVENT_ADMISSION) {
    assert(trace->admission_count < ENDGAME_CURVE_MAX_POINTS);
    trace->admission_points[trace->admission_count++] = *event;
    return;
  }
  if (event->event != ANALYSIS_EVENT_DEPTH_DONE) {
    return;
  }
  assert(trace->count < ENDGAME_CURVE_MAX_POINTS);
  trace->points[trace->count++] =
      (EndgameCurvePoint){.depth = event->depth,
                          .nodes = event->nodes,
                          .elapsed_ns = event->elapsed_ns,
                          .cpu_ns = event->cpu_ns,
                          .root_completed = event->candidates_completed,
                          .root_total = event->candidates_total,
                          .ply2_completed = event->subcandidates_completed,
                          .ply2_total = event->subcandidates_total,
                          .tiny_move = event->item_id,
                          .search_value = (int32_t)event->value};
}

static void endgame_curve_record_root_moves(
    const Game *game, const SmallMove *initial_moves, int initial_move_count,
    int initial_spread, int solving_player, void *user_data) {
  (void)game;
  (void)initial_spread;
  (void)solving_player;
  EndgameCurveTrace *trace = user_data;
  free(trace->root_moves);
  trace->root_moves =
      malloc((size_t)initial_move_count * sizeof(*trace->root_moves));
  assert(trace->root_moves);
  memcpy(trace->root_moves, initial_moves,
         (size_t)initial_move_count * sizeof(*trace->root_moves));
  trace->root_move_count = initial_move_count;
}

static const SmallMove *
endgame_curve_find_root_move(const EndgameCurveTrace *trace,
                             uint64_t tiny_move) {
  for (int move_idx = 0; move_idx < trace->root_move_count; move_idx++) {
    if (trace->root_moves[move_idx].tiny_move == tiny_move) {
      return &trace->root_moves[move_idx];
    }
  }
  return NULL;
}

static bool endgame_curve_find_exact_value(const uint64_t tiny_move,
                                           const uint64_t exact_tiny_moves[],
                                           const int32_t exact_values[],
                                           int exact_count, int32_t *value) {
  for (int i = 0; i < exact_count; i++) {
    if (exact_tiny_moves[i] == tiny_move) {
      *value = exact_values[i];
      return true;
    }
  }
  return false;
}

// Hardware-agnostic endgame value curve. One unbounded iterative-deepening
// solve records the returned move after every completed depth together with
// cumulative search nodes. Every distinct returned move is then evaluated at
// the same ample oracle depth using EndgameArgs.actual_move. This makes regret
// exact in spread points while the x-axis is deterministic work rather than
// elapsed time.
//
// EGCURVEPOINT fields:
//   nodes       cumulative nodes needed to complete this depth
//   elapsed_ns  wall-clock conversion measured on this run
//   cpu_ns      aggregate process CPU; cpu_ns/elapsed_ns is scheduled cores
//   root/ply2   structural coverage at the completed IDS boundary
//   exact       oracle value of the move returned at this depth
//   regret      oracle best value - exact
//
// Environment:
//   MAGPIE_EG_CURVE_CGP      input (default obj/endgame-value-cgps.txt)
//   MAGPIE_EG_CURVE_MAX      positions (default 40)
//   MAGPIE_EG_CURVE_START    first source position (default 0)
//   MAGPIE_EG_CURVE_THREADS  solver threads (default 10)
//   MAGPIE_EG_CURVE_PLIES    iterative-deepening arm ceiling
//                            (default MAX_SEARCH_DEPTH)
//   MAGPIE_EG_CURVE_JUDGE_PLIES  forced-move judge depth; at least PLIES
//                                (default PLIES)
//   MAGPIE_EG_CURVE_MAX_SECONDS  per-position arm ceiling; 0 = unbounded
//   MAGPIE_EG_CURVE_JUDGE_MAX_SECONDS  per-distinct-nominee judge ceiling;
//                                      0 = unbounded
//   MAGPIE_EG_CURVE_JUDGE    1 evaluates depth nominees (default); 0 emits
//                            admission-only traces without oracle re-searches
//   MAGPIE_EG_CURVE_JUDGE_MIN_DEPTH  only oracle/emit nominees at or above
//                                    this depth (default 1)
//   MAGPIE_EG_CURVE_SOLVE_MIN_TILES  minimum actual rack total (0)
//   MAGPIE_EG_CURVE_SOLVE_MAX_TILES  maximum actual rack total (2*RACK_SIZE)
//   MAGPIE_EG_CURVE_ADMISSION_SHADOW  emit frozen-model predictions (0)
//   MAGPIE_EG_CURVE_TIME_MANAGER_SHADOW  apply the frozen target-5 value prior
//                                        and TimeManager in shadow (0)
//   MAGPIE_EG_CURVE_TM_SAFETY_SECONDS  protected response/overtime time (0)
//   MAGPIE_EG_CURVE_TM_FUTURE_SECONDS  protected future-turn demand (0)
//   MAGPIE_EG_CURVE_TM_FUTURE_VALUE   future utility gain per second (0)
//   MAGPIE_EG_CURVE_TM_AUTO_FUTURE_NODES  reserve the shadow-only fixed-depth-5
//                                          future-work envelope (0)
//   MAGPIE_EG_CURVE_TM_FUTURE_RATE_MULTIPLIER  live seconds/node safety
//                                               multiplier (1.5)
//   MAGPIE_EG_CURVE_TM_TOTAL_CLOCK_SECONDS  player's clock at solve start;
//                                           separate from MAX_SECONDS (0)
void test_endgame_value_curve(void) {
  log_set_level(LOG_FATAL);
  const char *cgp_file = getenv("MAGPIE_EG_CURVE_CGP");
  if (cgp_file == NULL || cgp_file[0] == '\0') {
    cgp_file = "obj/endgame-value-cgps.txt";
  }
  const int max_positions = env_int("MAGPIE_EG_CURVE_MAX", 40);
  const int start_position = env_int("MAGPIE_EG_CURVE_START", 0);
  const int threads = env_int("MAGPIE_EG_CURVE_THREADS", 10);
  const int plies = env_int("MAGPIE_EG_CURVE_PLIES", MAX_SEARCH_DEPTH);
  const int judge_plies = env_int("MAGPIE_EG_CURVE_JUDGE_PLIES", plies);
  const double max_seconds = env_double("MAGPIE_EG_CURVE_MAX_SECONDS", 0.0);
  const double judge_max_seconds =
      env_double("MAGPIE_EG_CURVE_JUDGE_MAX_SECONDS", 0.0);
  const int judge = env_int("MAGPIE_EG_CURVE_JUDGE", 1);
  const int judge_min_depth = env_int("MAGPIE_EG_CURVE_JUDGE_MIN_DEPTH", 1);
  const int solve_min_tiles = env_int("MAGPIE_EG_CURVE_SOLVE_MIN_TILES", 0);
  const int solve_max_tiles =
      env_int("MAGPIE_EG_CURVE_SOLVE_MAX_TILES", 2 * RACK_SIZE);
  const int admission_shadow = env_int("MAGPIE_EG_CURVE_ADMISSION_SHADOW", 0);
  const int time_manager_shadow =
      env_int("MAGPIE_EG_CURVE_TIME_MANAGER_SHADOW", 0);
  const double tm_safety_seconds =
      env_double("MAGPIE_EG_CURVE_TM_SAFETY_SECONDS", 0.0);
  const double tm_future_seconds =
      env_double("MAGPIE_EG_CURVE_TM_FUTURE_SECONDS", 0.0);
  const double tm_future_value =
      env_double("MAGPIE_EG_CURVE_TM_FUTURE_VALUE", 0.0);
  const int tm_auto_future_nodes =
      env_int("MAGPIE_EG_CURVE_TM_AUTO_FUTURE_NODES", 0);
  const double tm_future_rate_multiplier =
      env_double("MAGPIE_EG_CURVE_TM_FUTURE_RATE_MULTIPLIER", 1.5);
  const double tm_total_clock_seconds =
      env_double("MAGPIE_EG_CURVE_TM_TOTAL_CLOCK_SECONDS", 0.0);
  assert(max_positions > 0);
  assert(start_position >= 0 && start_position < max_positions);
  assert(threads > 0);
  assert(plies > 0 && plies <= MAX_SEARCH_DEPTH);
  assert(judge_plies >= plies && judge_plies <= MAX_SEARCH_DEPTH);
  assert(max_seconds >= 0.0);
  assert(judge_max_seconds >= 0.0);
  assert(judge == 0 || judge == 1);
  assert(judge_min_depth > 0 && judge_min_depth <= plies);
  assert(solve_min_tiles >= 0 && solve_max_tiles >= solve_min_tiles);
  assert(admission_shadow == 0 || admission_shadow == 1);
  assert(time_manager_shadow == 0 || time_manager_shadow == 1);
  assert(!time_manager_shadow || admission_shadow);
  assert(tm_safety_seconds >= 0.0);
  assert(tm_future_seconds >= 0.0);
  assert(tm_future_value >= 0.0);
  assert(tm_auto_future_nodes == 0 || tm_auto_future_nodes == 1);
  assert(tm_future_rate_multiplier >= 1.0);
  assert(tm_total_clock_seconds >= 0.0);

  FILE *fp = fopen(cgp_file, "re");
  if (fp == NULL) {
    printf("EGCURVEERR no CGP file at %s; run genegcurve first\n", cgp_file);
    return;
  }
  char (*cgp_lines)[4096] = malloc((size_t)max_positions * 4096);
  assert(cgp_lines);
  int num_cgps = 0;
  while (num_cgps < max_positions && fgets(cgp_lines[num_cgps], 4096, fp)) {
    size_t len = strlen(cgp_lines[num_cgps]);
    if (len > 0 && cgp_lines[num_cgps][len - 1] == '\n') {
      cgp_lines[num_cgps][len - 1] = '\0';
    }
    if (cgp_lines[num_cgps][0] != '\0') {
      num_cgps++;
    }
  }
  (void)fclose(fp);

  char settings[256];
  (void)snprintf(settings, sizeof(settings),
                 "set -lex CSW24 -threads %d -s1 score -s2 score", threads);
  Config *config = config_create_or_die(settings);
  exec_config_quiet(config, "new");
  Game *game = config_get_game(config);
  EndgameCtx *solver = NULL;
  EndgameResults *results = endgame_results_create();
  printf("EGCURVECFG positions=%d start=%d threads=%d plies=%d "
         "judge_plies=%d judge=%d judge_min_depth=%d "
         "max_seconds=%.3f judge_max_seconds=%.3f tiles=%d..%d "
         "admission_shadow=%d time_manager_shadow=%d "
         "tm_safety=%.3f tm_future=%.3f tm_future_value=%.9f "
         "tm_auto_future_nodes=%d tm_future_rate_multiplier=%.3f "
         "tm_total_clock=%.3f "
         "file=%s unit=nodes\n",
         num_cgps - start_position, start_position, threads, plies, judge_plies,
         judge, judge_min_depth, max_seconds, judge_max_seconds,
         solve_min_tiles, solve_max_tiles, admission_shadow,
         time_manager_shadow, tm_safety_seconds, tm_future_seconds,
         tm_future_value, tm_auto_future_nodes, tm_future_rate_multiplier,
         tm_total_clock_seconds, cgp_file);
  (void)fflush(stdout);

  for (int pos = start_position; pos < num_cgps; pos++) {
    ErrorStack *err = error_stack_create();
    game_load_cgp(game, cgp_lines[pos], err);
    if (!error_stack_is_empty(err)) {
      printf("EGCURVEERR pos=%d load\n", pos);
      error_stack_destroy(err);
      continue;
    }
    error_stack_destroy(err);
    const int rack_tiles =
        (int)rack_get_total_letters(player_get_rack(game_get_player(game, 0))) +
        (int)rack_get_total_letters(player_get_rack(game_get_player(game, 1)));
    if (rack_tiles < solve_min_tiles || rack_tiles > solve_max_tiles) {
      continue;
    }

    EndgameCurveTrace trace = {0};
    AnalysisProgressListener listener = {
        .callback = endgame_curve_record,
        .user_data = &trace,
        .run_id = (uint64_t)pos + 1,
    };
    const int solving_player = game_get_player_on_turn_index(game);
    const int turns_remaining =
        MAX(1, (int)rack_get_total_letters(
                   player_get_rack(game_get_player(game, solving_player))));
    const uint64_t tm_future_reserve_nodes =
        tm_auto_future_nodes ? endgame_future_depth5_reserve_nodes(rack_tiles)
                             : 0;
    EndgameCurveAdmissionPolicy admission_policy = {
        .use_time_manager = time_manager_shadow,
        .prediction =
            {
                .model = endgame_admission_default_model(),
                .tt_fraction_of_mem = 0.05,
                .use_heuristics = true,
                .uses_external_tt = false,
                .reserve_seconds = 0.0,
            },
        .time_manager =
            {
                .prediction_policy =
                    {
                        .model = endgame_admission_default_model(),
                        .tt_fraction_of_mem = 0.05,
                        .use_heuristics = true,
                        .uses_external_tt = false,
                        .reserve_seconds = 0.0,
                    },
                .clock =
                    {
                        .turns_remaining = turns_remaining,
                        .safety_reserve_seconds = tm_safety_seconds,
                        .future_reserve_seconds = tm_future_seconds,
                        .future_value_per_second = tm_future_value,
                        .minimum_completion_confidence = 0.99,
                    },
                .regret_callback = endgame_admission_target5_regret_prior,
                .future_reserve_nodes = tm_future_reserve_nodes,
                .future_rate_safety_multiplier = tm_future_rate_multiplier,
            },
    };
    EndgameArgs args = {
        .game = game,
        .thread_control = config_get_thread_control(config),
        .plies = plies,
        .tt_fraction_of_mem = 0.05,
        .initial_small_move_arena_size = DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
        .num_threads = threads,
        .num_top_moves = 1,
        .use_heuristics = true,
        .forced_pass_bypass = true,
        .enable_pv_display = false,
        .before_search_callback = endgame_curve_record_root_moves,
        .before_search_callback_data = &trace,
        .depth_admission_callback =
            admission_shadow ? endgame_curve_admission_callback : NULL,
        .depth_admission_callback_data =
            admission_shadow ? &admission_policy : NULL,
        .depth_admission_min_depth = admission_shadow ? 3 : 0,
        .enforce_depth_admission = false,
        .has_player_clock = tm_total_clock_seconds > 0.0,
        .player_clock_seconds_at_start = tm_total_clock_seconds,
        .seed = 42,
        .progress_listener = listener,
    };
    if (max_seconds > 0.0) {
      args.external_deadline_ns =
          ctimer_monotonic_ns() + (int64_t)(max_seconds * 1.0e9);
    }
    err = error_stack_create();
    Timer solve_timer;
    ctimer_start(&solve_timer);
    const int64_t solve_cpu_start_ns = ctimer_process_cpu_ns();
    endgame_solve(&solver, &args, results, err);
    const int64_t solve_cpu_ns = ctimer_process_cpu_ns() - solve_cpu_start_ns;
    const int64_t solve_elapsed_ns =
        (int64_t)(ctimer_elapsed_seconds(&solve_timer) * 1.0e9);
    assert(error_stack_is_empty(err));
    error_stack_destroy(err);
    const PVLine *oracle_pv =
        endgame_results_get_pvline(results, ENDGAME_RESULT_BEST);
    for (int admission_idx = 0; admission_idx < trace.admission_count;
         admission_idx++) {
      const AnalysisProgressEvent *admission =
          &trace.admission_points[admission_idx];
      printf("EGADMITSHADOW pos=%d completed_depth=%d target_depth=%d "
             "nodes=%llu expected_nodes=%llu bound_nodes=%llu "
             "expected_seconds=%.9f bound_seconds=%.9f confidence=%.6f "
             "decision=%s safe=%d tm=%d regret=%.9f current_vps=%.9f "
             "future_vps=%.9f future_nodes=%llu future_rate_multiplier=%.3f "
             "max_current=%.9f deposit=%.9f\n",
             pos, admission->phase, admission->depth,
             (unsigned long long)admission->nodes,
             (unsigned long long)admission->expected_next_work_units,
             (unsigned long long)admission->completion_bound_work_units,
             admission->expected_next_seconds,
             admission->completion_bound_seconds,
             admission->completion_confidence,
             analysis_admission_name(admission->admission),
             admission->admission_safe_to_enforce ? 1 : 0,
             admission->has_time_manager_plan ? 1 : 0,
             admission->expected_regret_reduction,
             admission->current_value_per_second,
             admission->future_value_per_second,
             (unsigned long long)tm_future_reserve_nodes,
             tm_future_rate_multiplier, admission->maximum_current_seconds,
             admission->deposit_seconds);
    }
    if (!judge) {
      const int result_depth =
          endgame_results_get_depth(results, ENDGAME_RESULT_BEST);
      const uint64_t total_nodes = endgame_ctx_get_nodes_searched(solver);
      const bool censored = trace.finish_status == ANALYSIS_STATUS_TIME_LIMIT ||
                            trace.finish_status == ANALYSIS_STATUS_WORK_LIMIT ||
                            trace.finish_status == ANALYSIS_STATUS_INTERRUPTED;
      printf("EGADMITPOS pos=%d rack_tiles=%d points=%d depth=%d "
             "nodes=%llu elapsed_ns=%lld cpu_ns=%lld status=%d censored=%d\n",
             pos, rack_tiles, trace.count, result_depth,
             (unsigned long long)total_nodes, (long long)solve_elapsed_ns,
             (long long)solve_cpu_ns, trace.finish_status, censored ? 1 : 0);
      for (int point_idx = 0; point_idx < trace.count; point_idx++) {
        const EndgameCurvePoint *point = &trace.points[point_idx];
        printf("EGADMITPOINT pos=%d depth=%d nodes=%llu elapsed_ns=%lld "
               "cpu_ns=%lld root=%d/%d ply2=%d/%d tiny=%llu search=%d\n",
               pos, point->depth, (unsigned long long)point->nodes,
               (long long)point->elapsed_ns, (long long)point->cpu_ns,
               point->root_completed, point->root_total, point->ply2_completed,
               point->ply2_total, (unsigned long long)point->tiny_move,
               point->search_value);
      }
      if (censored) {
        const int prior_depth =
            trace.count > 0 ? trace.points[trace.count - 1].depth : 0;
        const uint64_t prior_nodes =
            trace.count > 0 ? trace.points[trace.count - 1].nodes : 0;
        printf("EGADMITCENSOR pos=%d target_depth=%d "
               "lower_added_nodes=%llu total_nodes=%llu\n",
               pos, prior_depth + 1,
               (unsigned long long)(total_nodes - prior_nodes),
               (unsigned long long)total_nodes);
      }
      (void)fflush(stdout);
      free(trace.root_moves);
      continue;
    }
    if (oracle_pv->num_moves <= 0 ||
        endgame_results_get_depth(results, ENDGAME_RESULT_BEST) < plies) {
      printf("EGCURVETRUNC pos=%d rack_tiles=%d points=%d depth=%d "
             "nodes=%llu elapsed_ns=%lld cpu_ns=%lld max_seconds=%.3f\n",
             pos, rack_tiles, trace.count,
             endgame_results_get_depth(results, ENDGAME_RESULT_BEST),
             (unsigned long long)endgame_ctx_get_nodes_searched(solver),
             (long long)solve_elapsed_ns, (long long)solve_cpu_ns, max_seconds);
      (void)fflush(stdout);
      free(trace.root_moves);
      continue;
    }
    const uint64_t search_nodes = endgame_ctx_get_nodes_searched(solver);
    uint64_t exact_tiny_moves[ENDGAME_CURVE_MAX_POINTS] = {0};
    int32_t exact_values[ENDGAME_CURVE_MAX_POINTS] = {0};
    int exact_count = 0;
    uint64_t judge_nodes = 0;
    bool judge_truncated = false;
    bool truncated_actual_found = false;
    uint64_t truncated_tiny_move = 0;
    uint64_t truncated_nodes = 0;
    int truncated_actual_depth = -1;
    int64_t truncated_elapsed_ns = 0;
    endgame_result_status_t truncated_status = ENDGAME_RESULT_STATUS_NONE;
    Timer judge_timer;
    ctimer_start(&judge_timer);
    const int64_t judge_cpu_start_ns = ctimer_process_cpu_ns();
    for (int point_idx = 0; point_idx < trace.count; point_idx++) {
      if (trace.points[point_idx].depth < judge_min_depth) {
        continue;
      }
      const uint64_t tiny_move = trace.points[point_idx].tiny_move;
      int32_t unused;
      // tiny_move == 0 is PASS, a legal root move that must be judged like
      // every other completed-depth nominee. It is not a missing identity.
      if (endgame_curve_find_exact_value(tiny_move, exact_tiny_moves,
                                         exact_values, exact_count, &unused)) {
        continue;
      }
      const SmallMove *small_move =
          endgame_curve_find_root_move(&trace, tiny_move);
      assert(small_move);
      Move actual_move = {0};
      small_move_to_move(&actual_move, small_move, game_get_board(game));
      args.progress_listener = (AnalysisProgressListener){0};
      args.before_search_callback = NULL;
      args.before_search_callback_data = NULL;
      args.external_deadline_ns =
          judge_max_seconds > 0.0
              ? ctimer_monotonic_ns() + (int64_t)(judge_max_seconds * 1.0e9)
              : 0;
      args.plies = judge_plies;
      args.actual_move = &actual_move;
      Timer candidate_judge_timer;
      ctimer_start(&candidate_judge_timer);
      err = error_stack_create();
      endgame_solve(&solver, &args, results, err);
      assert(error_stack_is_empty(err));
      error_stack_destroy(err);
      const int64_t candidate_elapsed_ns =
          (int64_t)(ctimer_elapsed_seconds(&candidate_judge_timer) * 1.0e9);
      const uint64_t candidate_nodes = endgame_ctx_get_nodes_searched(solver);
      judge_nodes += candidate_nodes;
      const bool actual_found = endgame_results_get_actual_move_found(results);
      const int actual_depth =
          actual_found
              ? endgame_results_get_depth(results, ENDGAME_RESULT_ACTUAL)
              : -1;
      if (!actual_found || actual_depth < judge_plies) {
        judge_truncated = true;
        truncated_actual_found = actual_found;
        truncated_tiny_move = tiny_move;
        truncated_nodes = candidate_nodes;
        truncated_actual_depth = actual_depth;
        truncated_elapsed_ns = candidate_elapsed_ns;
        truncated_status = endgame_results_get_status(results);
        break;
      }
      exact_tiny_moves[exact_count] = tiny_move;
      exact_values[exact_count] =
          endgame_results_get_value(results, ENDGAME_RESULT_ACTUAL);
      exact_count++;
    }
    const int64_t judge_cpu_ns = ctimer_process_cpu_ns() - judge_cpu_start_ns;
    const int64_t judge_elapsed_ns =
        (int64_t)(ctimer_elapsed_seconds(&judge_timer) * 1.0e9);
    int scored_points = 0;
    for (int point_idx = 0; point_idx < trace.count; point_idx++) {
      scored_points += trace.points[point_idx].depth >= judge_min_depth;
    }
    if (judge_truncated) {
      printf("EGCURVEJUDGETRUNC pos=%d rack_tiles=%d points=%d "
             "distinct_complete=%d failed_tiny=%llu actual_found=%d "
             "actual_depth=%d status=%d candidate_nodes=%llu "
             "candidate_elapsed_ns=%lld search_depth=%d search_nodes=%llu "
             "judge_depth=%d judge_nodes=%llu judge_elapsed_ns=%lld "
             "judge_cpu_ns=%lld "
             "max_seconds=%.3f judge_max_seconds=%.3f\n",
             pos, rack_tiles, scored_points, exact_count,
             (unsigned long long)truncated_tiny_move,
             truncated_actual_found ? 1 : 0, truncated_actual_depth,
             truncated_status, (unsigned long long)truncated_nodes,
             (long long)truncated_elapsed_ns, plies,
             (unsigned long long)search_nodes, judge_plies,
             (unsigned long long)judge_nodes, (long long)judge_elapsed_ns,
             (long long)judge_cpu_ns, max_seconds, judge_max_seconds);
      for (int point_idx = 0; point_idx < trace.count; point_idx++) {
        const EndgameCurvePoint *point = &trace.points[point_idx];
        const char *record_name = point->depth < judge_min_depth
                                      ? "EGCURVEJUDGECONTEXT"
                                      : "EGCURVEJUDGEPOINT";
        printf("%s pos=%d depth=%d nodes=%llu elapsed_ns=%lld cpu_ns=%lld "
               "root=%d/%d ply2=%d/%d tiny=%llu search=%d\n",
               record_name, pos, point->depth, (unsigned long long)point->nodes,
               (long long)point->elapsed_ns, (long long)point->cpu_ns,
               point->root_completed, point->root_total, point->ply2_completed,
               point->ply2_total, (unsigned long long)point->tiny_move,
               point->search_value);
      }
      (void)fflush(stdout);
      free(trace.root_moves);
      continue;
    }
    assert(exact_count > 0);

    // The fixed-depth root search can rank its own candidates imperfectly.
    // Once every distinct nominee has been evaluated at the same fixed depth,
    // the oracle is the best of those exact values, not automatically the
    // move that the unrestricted root search happened to return.
    int32_t oracle_value = exact_values[0];
    uint64_t oracle_tiny = exact_tiny_moves[0];
    for (int exact_idx = 1; exact_idx < exact_count; exact_idx++) {
      if (exact_values[exact_idx] > oracle_value) {
        oracle_value = exact_values[exact_idx];
        oracle_tiny = exact_tiny_moves[exact_idx];
      }
    }

    printf("EGCURVEPOS pos=%d rack_tiles=%d points=%d distinct=%d "
           "search_depth=%d search_nodes=%llu oracle_depth=%d "
           "oracle_nodes=%llu oracle_tiny=%llu oracle_value=%d "
           "elapsed_ns=%lld cpu_ns=%lld oracle_elapsed_ns=%lld "
           "oracle_cpu_ns=%lld\n",
           pos, rack_tiles, scored_points, exact_count, plies,
           (unsigned long long)search_nodes, judge_plies,
           (unsigned long long)judge_nodes, (unsigned long long)oracle_tiny,
           oracle_value, (long long)solve_elapsed_ns, (long long)solve_cpu_ns,
           (long long)judge_elapsed_ns, (long long)judge_cpu_ns);
    // The immediately preceding boundary supplies the observable
    // move/value-settling feature without paying to oracle-score an otherwise
    // irrelevant shallow nominee.
    if (judge_min_depth > 1) {
      for (int point_idx = 0; point_idx < trace.count; point_idx++) {
        const EndgameCurvePoint *point = &trace.points[point_idx];
        if (point->depth != judge_min_depth - 1) {
          continue;
        }
        printf("EGCURVECONTEXT pos=%d depth=%d nodes=%llu elapsed_ns=%lld "
               "cpu_ns=%lld root=%d/%d ply2=%d/%d tiny=%llu search=%d\n",
               pos, point->depth, (unsigned long long)point->nodes,
               (long long)point->elapsed_ns, (long long)point->cpu_ns,
               point->root_completed, point->root_total, point->ply2_completed,
               point->ply2_total, (unsigned long long)point->tiny_move,
               point->search_value);
        break;
      }
    }
    for (int point_idx = 0; point_idx < trace.count; point_idx++) {
      const EndgameCurvePoint *point = &trace.points[point_idx];
      if (point->depth < judge_min_depth) {
        continue;
      }
      int32_t exact_value = 0;
      assert(endgame_curve_find_exact_value(point->tiny_move, exact_tiny_moves,
                                            exact_values, exact_count,
                                            &exact_value));
      printf("EGCURVEPOINT pos=%d depth=%d nodes=%llu elapsed_ns=%lld "
             "cpu_ns=%lld root=%d/%d ply2=%d/%d tiny=%llu search=%d "
             "exact=%d regret=%d\n",
             pos, point->depth, (unsigned long long)point->nodes,
             (long long)point->elapsed_ns, (long long)point->cpu_ns,
             point->root_completed, point->root_total, point->ply2_completed,
             point->ply2_total, (unsigned long long)point->tiny_move,
             point->search_value, exact_value, oracle_value - exact_value);
    }
    (void)fflush(stdout);
    free(trace.root_moves);
  }

  free(cgp_lines);
  endgame_results_destroy(results);
  endgame_ctx_destroy(solver);
  config_destroy(config);
}

void test_endgame_speed_bench(void) {
  log_set_level(LOG_FATAL);

  const char *cgp_file = getenv("MAGPIE_BENCH_CGP");
  if (cgp_file == NULL || cgp_file[0] == '\0') {
    cgp_file = "/tmp/nonstuck_cgps.txt";
  }
  const char *lex = getenv("MAGPIE_BENCH_LEX");
  if (lex == NULL || lex[0] == '\0') {
    lex = "CSW21";
  }
  const char *tag = getenv("MAGPIE_BENCH_TAG");
  if (tag == NULL || tag[0] == '\0') {
    tag = "bench";
  }
  const int plies = env_int("MAGPIE_BENCH_PLIES", 4);
  const int threads = env_int("MAGPIE_BENCH_THREADS", 1);
  const int max_positions = env_int("MAGPIE_BENCH_MAX", 100);

  FILE *fp = fopen(cgp_file, "re");
  if (!fp) {
    printf("BENCHERR no CGP file at %s\n", cgp_file);
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

  printf("BENCHCFG tag=%s lex=%s plies=%d threads=%d positions=%d file=%s\n",
         tag, lex, plies, threads, num_cgps, cgp_file);

  double total_time = 0.0;
  uint64_t total_nodes = 0;

  for (int ci = 0; ci < num_cgps; ci++) {
    ErrorStack *err = error_stack_create();
    game_load_cgp(game, cgp_lines[ci], err);
    if (!error_stack_is_empty(err)) {
      error_stack_destroy(err);
      printf("BENCHROW %d SKIP_LOAD\n", ci);
      continue;
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
                        .enable_pv_display = false,
                        .seed = 42};

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

    printf("BENCHROW %d %d %llu %.6f\n", ci, value, (unsigned long long)nodes,
           elapsed);
    total_time += elapsed;
    total_nodes += nodes;
    if ((ci + 1) % 25 == 0) {
      (void)fflush(stdout);
    }
  }

  printf("BENCHSUM tag=%s positions=%d total_time=%.4f total_nodes=%llu "
         "nps=%.0f\n",
         tag, num_cgps, total_time, (unsigned long long)total_nodes,
         total_time > 0 ? (double)total_nodes / total_time : 0.0);
  (void)fflush(stdout);

  free(cgp_lines);
  endgame_ctx_destroy(solver);
  endgame_results_destroy(results);
  config_destroy(config);
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
//   MAGPIE_PO_CGP    CGP file        (default /tmp/nonstuck_cgps.txt)
//   MAGPIE_PO_LEX    lexicon         (default CSW21)
//   MAGPIE_PO_MAX    positions       (default 20)
//   MAGPIE_PO_TIMEMS per-move budget (default 100; 0 = no limit)
//   MAGPIE_PO_PLIES  depth ceiling   (default 25)
//   MAGPIE_PO_THREADS solver threads (default 1)
//   MAGPIE_PO_MAXMOVES per-playout move cap (default 40, safety)
//   MAGPIE_PO_TAG    label           (default "playout")
//   MAGPIE_PO_PRINTTRACE emit one POTURN row per searched game state
void test_endgame_playout_bench(void) {
  log_set_level(LOG_FATAL);

  const char *cgp_file = getenv("MAGPIE_PO_CGP");
  if (cgp_file == NULL || cgp_file[0] == '\0') {
    cgp_file = "/tmp/nonstuck_cgps.txt";
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
  const int max_moves = env_int("MAGPIE_PO_MAXMOVES", 40);

  FILE *fp = fopen(cgp_file, "re");
  if (!fp) {
    printf("POERR no CGP file at %s\n", cgp_file);
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

  printf("POCFG tag=%s lex=%s time_ms=%d plies=%d threads=%d positions=%d\n",
         tag, lex, time_ms, max_plies, threads, num_cgps);

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
      const int player_on_turn = game_get_player_on_turn_index(game);
      const int player_rack_tiles = (int)rack_get_total_letters(
          player_get_rack(game_get_player(game, player_on_turn)));
      const int combined_rack_tiles =
          (int)rack_get_total_letters(
              player_get_rack(game_get_player(game, 0))) +
          (int)rack_get_total_letters(
              player_get_rack(game_get_player(game, 1)));
      // Hard wall-clock cutoff: unlimited depth (plies = ceiling), IDS deepens
      // until the external deadline fires MID-depth (checked every 1024 nodes
      // via check_search_limits). soft/hard_time_limit stay 0 so the EBF
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
                          .enable_pv_display = false,
                          .soft_time_limit = 0,
                          .hard_time_limit = 0,
                          .external_deadline_ns = deadline_ns,
                          .seed = 42};

      Timer t;
      ctimer_start(&t);
      err = error_stack_create();
      endgame_solve(&solver, &args, results, err);
      const double turn_time = ctimer_elapsed_seconds(&t);
      pos_time += turn_time;
      assert(error_stack_is_empty(err));
      error_stack_destroy(err);

      const PVLine *pv =
          endgame_results_get_pvline(results, ENDGAME_RESULT_BEST);
      if (pv->num_moves == 0) {
        break;
      }
      const uint64_t turn_nodes = endgame_ctx_get_nodes_searched(solver);
      pos_nodes += turn_nodes;
      pos_depth += pv->negamax_depth;

      if (getenv("MAGPIE_PO_PRINTTRACE") != NULL) {
        printf("POTURN pos=%d ply=%d player=%d rack_tiles=%d "
               "player_rack_tiles=%d nodes=%llu depth=%d time=%.6f "
               "completed=%d tiny=%llu score=%d\n",
               ci, moves, player_on_turn, combined_rack_tiles,
               player_rack_tiles, (unsigned long long)turn_nodes,
               pv->negamax_depth, turn_time,
               endgame_results_get_status(results) ==
                       ENDGAME_RESULT_STATUS_FINISHED
                   ? 1
                   : 0,
               (unsigned long long)pv->moves[0].tiny_move, pv->score);
      }

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
