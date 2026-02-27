#include "timed_endgame_test.h"

#include "../src/compat/ctime.h"
#include "../src/compat/memory_info.h"
#include "../src/def/equity_defs.h"
#include "../src/def/game_defs.h"
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
#include "../src/ent/transposition_table.h"
#include "../src/impl/cgp.h"
#include "../src/impl/config.h"
#include "../src/impl/endgame.h"
#include "../src/impl/endgame_time.h"
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
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// Execute config command quietly (suppress stdout during execution)
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

// Timer thread: sleeps for the specified duration, then fires USER_INTERRUPT.
// Uses a done flag polled in short intervals so the thread can be stopped
// promptly when the solver finishes before the time limit.
typedef struct {
  ThreadControl *tc;
  double seconds;
  volatile bool done;
} TimerArgs;

static void *timer_thread_func(void *arg) {
  TimerArgs *ta = (TimerArgs *)arg;
  Timer t;
  ctimer_start(&t);
  while (!ta->done) {
    struct timespec ts = {0, 20L * 1000 * 1000}; // 20ms poll interval
    nanosleep(&ts, NULL);
    if (ctimer_elapsed_seconds(&t) >= ta->seconds) {
      break;
    }
  }
  if (!ta->done) {
    thread_control_set_status(ta->tc, THREAD_CONTROL_STATUS_USER_INTERRUPT);
  }
  return NULL;
}

// Core timed selfplay A/B benchmark: play stuck-tile endgame positions to
// completion using IDS with a per-turn time limit. Old vs New.
static void run_timed_selfplay_from(const char *cgp_file, int num_games,
                                    double time_limit_sec) {
  const int max_ply = 25;
  const int max_cgp_lines = 2000;
  FILE *fp = fopen(cgp_file, "re");
  if (!fp) {
    printf("No CGP file found at %s — run genstuck first.\n", cgp_file);
    return;
  }

  // Read all CGPs from file
  char (*cgp_lines)[4096] = malloc((size_t)max_cgp_lines * 4096);
  assert(cgp_lines);
  int num_cgps = 0;
  while (num_cgps < max_cgp_lines && fgets(cgp_lines[num_cgps], 4096, fp)) {
    size_t len = strlen(cgp_lines[num_cgps]);
    if (len > 0 && cgp_lines[num_cgps][len - 1] == '\n') {
      cgp_lines[num_cgps][len - 1] = '\0';
    }
    if (strlen(cgp_lines[num_cgps]) > 0) {
      num_cgps++;
    }
  }
  (void)fclose(fp);

  if (num_games > num_cgps) {
    num_games = num_cgps;
  }

  Config *config = config_create_or_die(
      "set -lex CSW21 -threads 6 -s1 score -s2 score -r1 small -r2 small");

  MoveList *move_list = move_list_create(1);
  exec_config_quiet(config, "new");
  Game *game = config_get_game(config);

  printf("\n");
  printf("==============================================================\n");
  printf("  Timed Selfplay A/B: %d games, %.1fs/turn, %d threads\n", num_games,
         time_limit_sec, 8);
  printf("  Old: IDS no bypass vs New: IDS with bypass\n");
  printf("  Positions: 100%% stuck from %s\n", cgp_file);
  printf("==============================================================\n");
  printf("  %4s  %10s %10s  %8s %8s  %6s  %5s %5s  %6s %6s\n", "Game",
         "Old Spread", "New Spread", "Old Time", "New Time", "Delta", "OTrns",
         "NTrns", "OMaxT", "NMaxT");
  printf("  ----  ---------- ----------  -------- --------  ------"
         "  ----- -----  ------ ------\n");
  (void)fflush(stdout);

  int total_delta = 0;
  int new_better = 0;
  int old_better = 0;
  int same_count = 0;
  double total_time_old = 0;
  double total_time_new = 0;
  double global_max_turn_old = 0;
  double global_max_turn_new = 0;
  int total_turns_old = 0;
  int total_turns_new = 0;
  int total_exceeded_old = 0;
  int total_exceeded_new = 0;

  for (int gi = 0; gi < num_games; gi++) {
    int final_spread[2] = {0, 0};
    double game_time[2] = {0, 0};
    double max_turn_time[2] = {0, 0};
    int turn_count[2] = {0, 0};

    for (int config_idx = 0; config_idx < 2; config_idx++) {
      bool use_bypass = (config_idx == 1);

      ErrorStack *err = error_stack_create();
      game_load_cgp(game, cgp_lines[gi], err);
      assert(error_stack_is_empty(err));
      error_stack_destroy(err);

      EndgameSolver *solver = endgame_solver_create();
      EndgameResults *results = endgame_results_create();

      ThreadControl *tc = config_get_thread_control(config);

      while (game_get_game_end_reason(game) == GAME_END_REASON_NONE) {
        EndgameArgs args = {.game = game,
                            .thread_control = tc,
                            .plies = max_ply,
                            .tt_fraction_of_mem = 0.50,
                            .initial_small_move_arena_size =
                                DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
                            .num_threads = 8,
                            .num_top_moves = 1,
                            .use_heuristics = true,
                            .per_ply_callback = NULL,
                            .per_ply_callback_data = NULL,
                            .forced_pass_bypass = use_bypass};

        thread_control_set_status(tc, THREAD_CONTROL_STATUS_STARTED);

        TimerArgs ta = {.tc = tc, .seconds = time_limit_sec, .done = false};
        pthread_t timer_tid; // NOLINT(misc-include-cleaner)
        pthread_create(&timer_tid, NULL, timer_thread_func, &ta);

        Timer t;
        ctimer_start(&t);
        err = error_stack_create();
        endgame_solve(solver, &args, results, err);
        double elapsed = ctimer_elapsed_seconds(&t);

        ta.done = true;
        pthread_join(timer_tid, NULL);

        assert(error_stack_is_empty(err));
        error_stack_destroy(err);

        game_time[config_idx] += elapsed;
        turn_count[config_idx]++;
        if (elapsed > max_turn_time[config_idx]) {
          max_turn_time[config_idx] = elapsed;
        }
        if (elapsed > time_limit_sec * 1.1) {
          if (config_idx == 0) {
            total_exceeded_old++;
          } else {
            total_exceeded_new++;
          }
        }

        const PVLine *pv =
            endgame_results_get_pvline(results, ENDGAME_RESULT_BEST);
        if (pv->num_moves == 0) {
          break;
        }

        SmallMove best = pv->moves[0];
        small_move_to_move(move_list->spare_move, &best, game_get_board(game));
        play_move(move_list->spare_move, game, NULL);
      }

      int s0 = equity_to_int(player_get_score(game_get_player(game, 0)));
      int s1 = equity_to_int(player_get_score(game_get_player(game, 1)));
      final_spread[config_idx] = s0 - s1;

      endgame_results_destroy(results);
      endgame_solver_destroy(solver);
    }

    int delta = final_spread[1] - final_spread[0];
    total_delta += delta;
    if (delta > 0) {
      new_better++;
    } else if (delta < 0) {
      old_better++;
    } else {
      same_count++;
    }

    total_time_old += game_time[0];
    total_time_new += game_time[1];
    total_turns_old += turn_count[0];
    total_turns_new += turn_count[1];
    if (max_turn_time[0] > global_max_turn_old) {
      global_max_turn_old = max_turn_time[0];
    }
    if (max_turn_time[1] > global_max_turn_new) {
      global_max_turn_new = max_turn_time[1];
    }

    printf("  %4d  %+10d %+10d  %7.2fs %7.2fs  %+5d  %5d %5d  %5.1fs %5.1fs\n",
           gi + 1, final_spread[0], final_spread[1], game_time[0], game_time[1],
           delta, turn_count[0], turn_count[1], max_turn_time[0],
           max_turn_time[1]);
    (void)fflush(stdout);
  }

  printf("  ----  ---------- ----------  -------- --------  ------"
         "  ----- -----  ------ ------\n");
  printf("\n");
  printf("  Results (%d games, %.1fs/turn):\n", num_games, time_limit_sec);
  printf("    New better: %d  |  Old better: %d  |  Same: %d\n", new_better,
         old_better, same_count);
  printf("    Total spread delta: %+d (avg %+.2f per game)\n", total_delta,
         num_games > 0 ? (double)total_delta / num_games : 0.0);
  printf("    Old total time: %.2fs  |  New total time: %.2fs\n",
         total_time_old, total_time_new);
  printf(
      "    Old turns: %d (avg %.1f/game)  |  New turns: %d (avg %.1f/game)\n",
      total_turns_old,
      num_games > 0 ? (double)total_turns_old / num_games : 0.0,
      total_turns_new,
      num_games > 0 ? (double)total_turns_new / num_games : 0.0);
  printf("    Max turn time: old=%.1fs  new=%.1fs  (limit=%.1fs)\n",
         global_max_turn_old, global_max_turn_new, time_limit_sec);
  printf("    Turns exceeding limit by >10%%: old=%d  new=%d\n",
         total_exceeded_old, total_exceeded_new);
  printf("==============================================================\n");
  (void)fflush(stdout);

  free(cgp_lines);
  move_list_destroy(move_list);
  config_destroy(config);
}

// Play greedy moves until bag is empty, returning true if we get a valid
// endgame position (bag empty, both players have tiles, game not over).
static bool play_until_bag_empty(Game *game, MoveList *move_list) {
  while (bag_get_letters(game_get_bag(game)) > 0) {
    Move *move = get_top_equity_move(game, 0, move_list);
    play_move(move, game, NULL);
    if (game_get_game_end_reason(game) != GAME_END_REASON_NONE) {
      return false;
    }
  }
  const Rack *rack0 = player_get_rack(game_get_player(game, 0));
  const Rack *rack1 = player_get_rack(game_get_player(game, 1));
  return !rack_is_empty(rack0) && !rack_is_empty(rack1);
}

// Timed A/B benchmark: generate random endgame positions on the fly,
// play each to completion with and without cross-set precheck.
// Tracks first-turn time separately and cumulative overage from time limit.
static void run_timed_precheck_ab(int num_games, double time_limit_sec,
                                  uint64_t base_seed) {
  const int max_ply = 25;

  Config *config = config_create_or_die(
      "set -lex CSW21 -threads 6 -s1 score -s2 score -r1 small -r2 small");

  MoveList *move_list = move_list_create(1);
  exec_config_quiet(config, "new");
  Game *game = config_get_game(config);

  printf("\n");
  printf("====================================================================="
         "====="
         "==========\n");
  printf("  Timed Precheck A/B: up to %d games, %.1fs/turn, 8 threads, "
         "seed=%llu\n",
         num_games, time_limit_sec, (unsigned long long)base_seed);
  printf("  Old: no cross-set precheck  vs  New: with cross-set precheck\n");
  printf("  Positions: random endgames (greedy self-play to bag-empty)\n");
  printf("====================================================================="
         "====="
         "==========\n");
  printf("  %4s  %6s  %7s %7s  %4s %4s  %7s %7s  %5s  %-8s %-8s"
         "  %7s %7s %5s %5s  %-24s\n",
         "Game", "Delta", "OldT1", "NewT1", "OD1", "ND1", "OldTot", "NewTot",
         "Trns", "P1", "P2", "OLook", "NLook", "OHit%", "NHit%", "Cumulative");
  printf("  ----  ------  ------- -------  ---- ----  ------- -------"
         "  -----  -------- --------"
         "  ------- ------- ----- -----  ------------------------\n");
  (void)fflush(stdout);

  int total_delta = 0;
  int new_better = 0;
  int old_better = 0;
  int same_count = 0;
  double total_time_old = 0;
  double total_time_new = 0;
  double total_t1_old = 0;
  double total_t1_new = 0;
  double total_overage_old = 0;
  double total_overage_new = 0;
  int total_turns_old = 0;
  int total_turns_new = 0;
  int games_played = 0;
  int attempts = 0;

  for (int gi = 0; games_played < num_games; gi++) {
    attempts++;
    // Generate random endgame position
    game_reset(game);
    game_seed(game, base_seed + (uint64_t)gi);
    draw_starting_racks(game);

    if (!play_until_bag_empty(game, move_list)) {
      continue;
    }
    if (game_get_game_end_reason(game) != GAME_END_REASON_NONE) {
      continue;
    }

    // Save CGP for replay
    char *cgp = game_get_cgp(game, true);

    // Capture rack strings for logging
    const LetterDistribution *ld = game_get_ld(game);
    const Rack *r0 = player_get_rack(game_get_player(game, 0));
    const Rack *r1 = player_get_rack(game_get_player(game, 1));
    StringBuilder *sb0 = string_builder_create();
    StringBuilder *sb1 = string_builder_create();
    string_builder_add_rack(sb0, r0, ld, false);
    string_builder_add_rack(sb1, r1, ld, false);
    char rack_str0[32] = {0};
    char rack_str1[32] = {0};
    (void)snprintf(rack_str0, sizeof(rack_str0), "%s", string_builder_peek(sb0));
    (void)snprintf(rack_str1, sizeof(rack_str1), "%s", string_builder_peek(sb1));
    string_builder_destroy(sb0);
    string_builder_destroy(sb1);

    int final_spread[2] = {0, 0};
    double game_time[2] = {0, 0};
    double first_turn_time[2] = {0, 0};
    double overage[2] = {0, 0};
    int turn_count[2] = {0, 0};
    int first_turn_depth[2] = {0, 0};
    int max_depth[2] = {0, 0};
    int tt_lookups[2] = {0, 0};
    int tt_hits[2] = {0, 0};

    for (int config_idx = 0; config_idx < 2; config_idx++) {
      ErrorStack *err = error_stack_create();
      game_load_cgp(game, cgp, err);
      assert(error_stack_is_empty(err));
      error_stack_destroy(err);

      EndgameSolver *solver = endgame_solver_create();
      EndgameResults *results = endgame_results_create();

      ThreadControl *tc = config_get_thread_control(config);

      while (game_get_game_end_reason(game) == GAME_END_REASON_NONE) {
        EndgameArgs args = {.game = game,
                            .thread_control = tc,
                            .plies = max_ply,
                            .tt_fraction_of_mem = 0.50,
                            .initial_small_move_arena_size =
                                DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
                            .num_threads = 8,
                            .num_top_moves = 1,
                            .use_heuristics = true,
                            .per_ply_callback = NULL,
                            .per_ply_callback_data = NULL,
                            .forced_pass_bypass = false};

        thread_control_set_status(tc, THREAD_CONTROL_STATUS_STARTED);

        TimerArgs ta = {.tc = tc, .seconds = time_limit_sec, .done = false};
        pthread_t timer_tid; // NOLINT(misc-include-cleaner)
        pthread_create(&timer_tid, NULL, timer_thread_func, &ta);

        Timer t;
        ctimer_start(&t);
        err = error_stack_create();
        endgame_solve(solver, &args, results, err);
        double elapsed = ctimer_elapsed_seconds(&t);

        ta.done = true;
        pthread_join(timer_tid, NULL);

        assert(error_stack_is_empty(err));
        error_stack_destroy(err);

        game_time[config_idx] += elapsed;
        int depth = endgame_results_get_depth(results, ENDGAME_RESULT_BEST);
        if (turn_count[config_idx] == 0) {
          first_turn_time[config_idx] = elapsed;
          first_turn_depth[config_idx] = depth;
        }
        if (depth > max_depth[config_idx]) {
          max_depth[config_idx] = depth;
        }
        if (elapsed > time_limit_sec) {
          overage[config_idx] += elapsed - time_limit_sec;
        }
        turn_count[config_idx]++;

        const PVLine *pv =
            endgame_results_get_pvline(results, ENDGAME_RESULT_BEST);
        if (pv->num_moves == 0) {
          break;
        }

        SmallMove best = pv->moves[0];
        small_move_to_move(move_list->spare_move, &best, game_get_board(game));
        play_move(move_list->spare_move, game, NULL);
      }

      int s0 = equity_to_int(player_get_score(game_get_player(game, 0)));
      int s1 = equity_to_int(player_get_score(game_get_player(game, 1)));
      final_spread[config_idx] = s0 - s1;

      const TranspositionTable *tt =
          endgame_solver_get_transposition_table(solver);
      if (tt) {
        tt_lookups[config_idx] = atomic_load(&tt->lookups);
        tt_hits[config_idx] = atomic_load(&tt->hits);
      }

      endgame_results_destroy(results);
      endgame_solver_destroy(solver);
    }

    free(cgp);

    int delta = final_spread[1] - final_spread[0];
    total_delta += delta;
    if (delta > 0) {
      new_better++;
    } else if (delta < 0) {
      old_better++;
    } else {
      same_count++;
    }

    total_time_old += game_time[0];
    total_time_new += game_time[1];
    total_t1_old += first_turn_time[0];
    total_t1_new += first_turn_time[1];
    total_overage_old += overage[0];
    total_overage_new += overage[1];
    total_turns_old += turn_count[0];
    total_turns_new += turn_count[1];
    games_played++;

    double ohit_pct =
        tt_lookups[0] > 0 ? 100.0 * tt_hits[0] / tt_lookups[0] : 0;
    double nhit_pct =
        tt_lookups[1] > 0 ? 100.0 * tt_hits[1] / tt_lookups[1] : 0;
    printf("  %4d  %+5d  %6.2fs %6.2fs  %4d %4d  %6.1fs %6.1fs  %3d/%d  "
           "%-8s %-8s  %5.1fM %5.1fM %4.1f%% %4.1f%%  "
           "N+%d O+%d =%d  d=%+d\n",
           games_played, delta, first_turn_time[0], first_turn_time[1],
           first_turn_depth[0], first_turn_depth[1], game_time[0], game_time[1],
           turn_count[0], turn_count[1], rack_str0, rack_str1,
           tt_lookups[0] / 1e6, tt_lookups[1] / 1e6, ohit_pct, nhit_pct,
           new_better, old_better, same_count, total_delta);
    (void)fflush(stdout);
  }

  printf("  ----  ------  ------- -------  ------- -------  ----- -----"
         "  -----  ------------------------\n");
  printf("\n");
  printf("  Results (%d games from %d attempts, %.1fs/turn, seed=%llu):\n",
         games_played, attempts, time_limit_sec, (unsigned long long)base_seed);
  printf("    New better: %d  |  Old better: %d  |  Same: %d\n", new_better,
         old_better, same_count);
  printf("    Total spread delta: %+d (avg %+.2f per game)\n", total_delta,
         games_played > 0 ? (double)total_delta / games_played : 0.0);
  printf("    Old total time: %.2fs  |  New total time: %.2fs\n",
         total_time_old, total_time_new);
  printf("    Old T1 total: %.2fs (avg %.2fs)  |  New T1 total: %.2fs "
         "(avg %.2fs)\n",
         total_t1_old, games_played > 0 ? total_t1_old / games_played : 0.0,
         total_t1_new, games_played > 0 ? total_t1_new / games_played : 0.0);
  printf("    Old overage: %.2fs  |  New overage: %.2fs\n", total_overage_old,
         total_overage_new);
  printf(
      "    Old turns: %d (avg %.1f/game)  |  New turns: %d (avg %.1f/game)\n",
      total_turns_old,
      games_played > 0 ? (double)total_turns_old / games_played : 0.0,
      total_turns_new,
      games_played > 0 ? (double)total_turns_new / games_played : 0.0);
  printf("====================================================================="
         "====="
         "==========\n");
  (void)fflush(stdout);

  move_list_destroy(move_list);
  config_destroy(config);
}

void test_benchmark_timed_precheck(void) {
  log_set_level(LOG_FATAL);
  run_timed_precheck_ab(10000, 2.0, 27182828);
}

void test_benchmark_timed_selfplay(void) {
  log_set_level(LOG_FATAL);
  run_timed_selfplay_from("/tmp/stuck_100pct_cgps.txt", 20, 30.0);
}

void test_benchmark_timed_overnight(void) {
  log_set_level(LOG_FATAL);
  run_timed_selfplay_from("/tmp/stuck_100pct_cgps.txt", 1500, 30.0);
}

void test_benchmark_timed_hard(void) {
  log_set_level(LOG_FATAL);
  run_timed_selfplay_from("/tmp/stuck_hard_cgps.txt", 188, 150.0);
}

// ============================================================
// Shared round-robin tournament infrastructure
// ============================================================
// Both the 3-way O/B/F and 4-way Static/Bullet/Blitz/Classical
// tournaments share this state and helpers for per-pairing tracking
// and cross-table output.
//
// Greppable prefixes for extracting partial results from long logs:
//   grep "^CTROW:"  output.log   # latest cross-table rows
//   grep "^RESULT:" output.log   # per-game result summary lines

enum { RR_MAX_CFGS = 4, RR_MAX_PAIRINGS = 6 };

// Cumulative statistics for a running round-robin tournament.
// Supports up to RR_MAX_CFGS configs and RR_MAX_PAIRINGS C(n,2) pairs.
typedef struct {
  int num_cfgs;
  int num_pairings;
  int pair_a[RR_MAX_PAIRINGS]; // index of first config in each pairing
  int pair_b[RR_MAX_PAIRINGS]; // index of second config in each pairing
  const char *cfg_names[RR_MAX_CFGS];
  const char *pair_labels[RR_MAX_PAIRINGS];
  int pair_net[RR_MAX_PAIRINGS];    // cumul net spread (>0 means pair_a wins)
  int pair_wins_a[RR_MAX_PAIRINGS]; // games where pair_a had higher net
  int pair_wins_b[RR_MAX_PAIRINGS]; // games where pair_b had higher net
  int pair_ties[RR_MAX_PAIRINGS];
  double cumul_time[RR_MAX_CFGS];     // total solver wall-time (seconds)
  double cumul_overtime[RR_MAX_CFGS]; // total overtime-penalty seconds
  int games_played;
  int stuck_count; // positions where either player had stuck tiles
} RRState;

// Record one pairing's net result for a game. net>0 means pair_a won.
static void rr_record_pairing(RRState *rr, int pi, int net) {
  rr->pair_net[pi] += net;
  if (net > 0) {
    rr->pair_wins_a[pi]++;
  } else if (net < 0) {
    rr->pair_wins_b[pi]++;
  } else {
    rr->pair_ties[pi]++;
  }
}

// Print a cross-table ranked by W-L. Each data row is prefixed "CTROW:"
// so partial logs can be analyzed with: grep "^CTROW:" output.log
static void rr_print_crosstable(const RRState *rr) {
  int spread[RR_MAX_CFGS];
  int wins[RR_MAX_CFGS];
  int losses[RR_MAX_CFGS];
  int ties[RR_MAX_CFGS];
  int matrix[RR_MAX_CFGS][RR_MAX_CFGS];
  memset(spread, 0, sizeof(spread));
  memset(wins, 0, sizeof(wins));
  memset(losses, 0, sizeof(losses));
  memset(ties, 0, sizeof(ties));
  memset(matrix, 0, sizeof(matrix));

  for (int pi = 0; pi < rr->num_pairings; pi++) {
    int cfg_a = rr->pair_a[pi];
    int cfg_b = rr->pair_b[pi];
    int net = rr->pair_net[pi];
    matrix[cfg_a][cfg_b] = net;
    matrix[cfg_b][cfg_a] = -net;
    spread[cfg_a] += net;
    spread[cfg_b] -= net;
    wins[cfg_a] += rr->pair_wins_a[pi];
    losses[cfg_a] += rr->pair_wins_b[pi];
    ties[cfg_a] += rr->pair_ties[pi];
    wins[cfg_b] += rr->pair_wins_b[pi];
    losses[cfg_b] += rr->pair_wins_a[pi];
    ties[cfg_b] += rr->pair_ties[pi];
  }

  // Sort configs by W-L descending (selection sort; small N).
  int order[RR_MAX_CFGS];
  for (int cfg_idx = 0; cfg_idx < rr->num_cfgs; cfg_idx++) {
    order[cfg_idx] = cfg_idx;
  }
  for (int outer_cfg = 0; outer_cfg < rr->num_cfgs - 1; outer_cfg++) {
    for (int inner_cfg = outer_cfg + 1; inner_cfg < rr->num_cfgs; inner_cfg++) {
      if (wins[order[inner_cfg]] - losses[order[inner_cfg]] >
          wins[order[outer_cfg]] - losses[order[outer_cfg]]) {
        int tmp = order[outer_cfg];
        order[outer_cfg] = order[inner_cfg];
        order[inner_cfg] = tmp;
      }
    }
  }

  int games_played = rr->games_played;
  printf("Cross-table (%d game(s), %d stuck, %d nonstuck):\n", games_played,
         rr->stuck_count, games_played - rr->stuck_count);
  printf("  %-9s ", "");
  for (int col_cfg = 0; col_cfg < rr->num_cfgs; col_cfg++) {
    printf("  %8s", rr->cfg_names[col_cfg]);
  }
  printf("  | %3s %3s %3s %4s | %7s %7s  %8s %7s\n", "W", "L", "T", "W-L",
         "Spread", "Avg", "Time", "OT");
  printf("  %-9s ", "");
  for (int col_cfg = 0; col_cfg < rr->num_cfgs; col_cfg++) {
    printf("  --------");
  }
  printf("  | --- --- --- ---- | ------- -------  -------- -------\n");

  for (int ri = 0; ri < rr->num_cfgs; ri++) {
    int row_cfg = order[ri];
    int wl = wins[row_cfg] - losses[row_cfg];
    double avg =
        games_played > 0 ? (double)spread[row_cfg] / games_played : 0.0;
    // CTROW: prefix makes this row greppable in incomplete logs.
    printf("CTROW: %-9s", rr->cfg_names[row_cfg]);
    for (int col_cfg = 0; col_cfg < rr->num_cfgs; col_cfg++) {
      if (row_cfg == col_cfg) {
        printf("  %8s", "-");
      } else {
        printf("  %+8d", matrix[row_cfg][col_cfg]);
      }
    }
    printf("  | %3d %3d %3d %+4d | %+7d %+7.2f  %7.1fs %6.2fs\n",
           wins[row_cfg], losses[row_cfg], ties[row_cfg], wl, spread[row_cfg],
           avg, rr->cumul_time[row_cfg], rr->cumul_overtime[row_cfg]);
  }
}

// ============================================================
// 3-way round robin: O vs B vs F (from CGP file)
// ============================================================
// Head-to-head pairings with side-swapping.
// For each position, plays all C(3,2)=3 pairings twice (swap sides).
// In each game, one config controls P1's turns, the other controls P2's.
// Net per pair = spread(A_as_P1) - spread(B_as_P1). Positive = A stronger.
//
// Configs: O = Old (no precheck, 80% hard limit)
//          B = Precheck + baseline (80% hard limit)
//          F = Precheck + EBF + 75% mid-depth bail
//
// Greppable output: "RESULT:" per-game summary, "CTROW:" cross-table rows.
static void run_timed_round_robin(const char *cgp_file, int start_game,
                                  int num_games, double p1_budget_sec,
                                  double p2_budget_sec) {
  const int max_ply = 25;
  const int max_turns = 50;
  // 2GB per TT (two TTs active simultaneously during a game)
  const double tt_frac =
      (2.0 * 1024.0 * 1024.0 * 1024.0) / (double)get_total_memory();

  // Read positions from file
  const int max_cgp_lines = 2000;
  FILE *fp = fopen(cgp_file, "re");
  if (!fp) {
    printf("No CGP file found at %s — run genstuck first.\n", cgp_file);
    return;
  }
  char (*cgp_lines)[4096] = malloc((size_t)max_cgp_lines * 4096);
  assert(cgp_lines);
  int num_cgps = 0;
  while (num_cgps < max_cgp_lines && fgets(cgp_lines[num_cgps], 4096, fp)) {
    size_t len = strlen(cgp_lines[num_cgps]);
    if (len > 0 && cgp_lines[num_cgps][len - 1] == '\n') {
      cgp_lines[num_cgps][len - 1] = '\0';
    }
    if (strlen(cgp_lines[num_cgps]) > 0) {
      num_cgps++;
    }
  }
  (void)fclose(fp);
  if (start_game >= num_cgps) {
    printf("start_game=%d >= num_cgps=%d, nothing to run.\n", start_game,
           num_cgps);
    free(cgp_lines);
    return;
  }
  if (start_game + num_games > num_cgps) {
    num_games = num_cgps - start_game;
  }

  // 3 configs:
  //   O = Old: no cross-set precheck, baseline timing (80% hard limit)
  //   B = Precheck + baseline timing (80% hard limit)
  //   F = Precheck + flexible EBF timing (60%/90% soft/hard)
  const char *cfg_names[] = {"O", "B", "F"};
  const int cfg_time_mode[] = {0, 0, 1};
  const bool cfg_precheck[] = {false, true, true};

  // 3 pairings: O-B isolates precheck effect, O-F shows combined gain,
  // B-F isolates EBF effect.
  const int num_pairings = 3;
  const int pair_a[] = {0, 0, 1};
  const int pair_b[] = {1, 2, 2};
  const char *pair_labels[] = {"O-B", "O-F", "B-F"};

  Config *config = config_create_or_die(
      "set -lex CSW21 -threads 6 -s1 score -s2 score -r1 small -r2 small");

  MoveList *move_list = move_list_create(1);
  exec_config_quiet(config, "new");
  Game *game = config_get_game(config);

  printf("\n");
  printf("=================================================================="
         "======================================\n");
  printf("  3-Way Round Robin: %d games, P1=%.0fs P2=%.0fs budget, "
         "8 threads\n",
         num_games, p1_budget_sec, p2_budget_sec);
  printf("  Positions: %s\n", cgp_file);
  printf("  O=old (no precheck, 80%% hard)  B=precheck+baseline  "
         "F=precheck+EBF+75%%bail\n");
  printf("  O-B=precheck effect  O-F=combined effect  B-F=EBF effect. "
         "Net + = first config stronger.\n");
  printf("=================================================================="
         "======================================\n");
  (void)fflush(stdout);

  RRState rr;
  memset(&rr, 0, sizeof(rr));
  rr.num_cfgs = 3;
  rr.num_pairings = num_pairings;
  for (int cfg_idx = 0; cfg_idx < 3; cfg_idx++) {
    rr.cfg_names[cfg_idx] = cfg_names[cfg_idx];
  }
  for (int pi = 0; pi < num_pairings; pi++) {
    rr.pair_a[pi] = pair_a[pi];
    rr.pair_b[pi] = pair_b[pi];
    rr.pair_labels[pi] = pair_labels[pi];
  }

  int games_played = 0;
  GameStringOptions *gso = game_string_options_create_default();

  for (int gi = 0; gi < num_games; gi++) {
    const char *cgp = cgp_lines[start_game + gi];
    games_played++;
    rr.games_played++;

    // Print position
    printf("\n=== Game %d ===\n", gi + 1);
    {
      ErrorStack *err = error_stack_create();
      game_load_cgp(game, cgp, err);
      assert(error_stack_is_empty(err));
      error_stack_destroy(err);
      StringBuilder *game_sb = string_builder_create();
      string_builder_add_game(game, NULL, gso, NULL, game_sb);
      printf("%s\n", string_builder_peek(game_sb));
      string_builder_destroy(game_sb);
    }

    int net[6];
    const LetterDistribution *ld = game_get_ld(game);

    for (int pi = 0; pi < num_pairings; pi++) {
      int ca = pair_a[pi];
      int cb = pair_b[pi];
      int spread[2];

      printf("  %s:\n", pair_labels[pi]);

      for (int dir = 0; dir < 2; dir++) {
        int cfg_for_player[2];
        cfg_for_player[0] = dir == 0 ? ca : cb;
        cfg_for_player[1] = dir == 0 ? cb : ca;

        ErrorStack *err = error_stack_create();
        game_load_cgp(game, cgp, err);
        assert(error_stack_is_empty(err));
        error_stack_destroy(err);

        EndgameSolver *solvers[2] = {endgame_solver_create(),
                                     endgame_solver_create()};
        EndgameResults *res[2] = {endgame_results_create(),
                                  endgame_results_create()};
        ThreadControl *tc = config_get_thread_control(config);

        // Per-player remaining time budgets
        double budget[2] = {p1_budget_sec, p2_budget_sec};
        // Per-player turn counts for the halving floor
        int player_turn_count[2] = {0, 0};

        StringBuilder *move_log = string_builder_create();
        int turn_num = 0;

        while (game_get_game_end_reason(game) == GAME_END_REASON_NONE &&
               turn_num < max_turns) {
          int player_on_turn = game_get_player_on_turn_index(game);
          int cfg = cfg_for_player[player_on_turn];

          int tiles_on_rack = rack_get_total_letters(
              player_get_rack(game_get_player(game, player_on_turn)));
          EndgameTurnLimits limits = endgame_compute_turn_limits(
              budget[player_on_turn], player_turn_count[player_on_turn],
              tiles_on_rack, cfg_time_mode[cfg]);
          double turn_limit = limits.turn_limit;
          double soft_limit = limits.soft_limit;
          double hard_limit = limits.hard_limit;

          EndgameArgs args = {.game = game,
                              .thread_control = tc,
                              .plies = max_ply,
                              .tt_fraction_of_mem = tt_frac,
                              .initial_small_move_arena_size =
                                  DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
                              .num_threads = 8,
                              .num_top_moves = 1,
                              .use_heuristics = true,
                              .per_ply_callback = NULL,
                              .per_ply_callback_data = NULL,
                              .skip_pruned_cross_sets = !cfg_precheck[cfg],
                              .forced_pass_bypass = false,
                              .soft_time_limit = soft_limit,
                              .hard_time_limit = hard_limit};

          thread_control_set_status(tc, THREAD_CONTROL_STATUS_STARTED);

          Timer turn_timer;
          ctimer_start(&turn_timer);

          TimerArgs ta = {.tc = tc, .seconds = turn_limit, .done = false};
          pthread_t timer_tid; // NOLINT(misc-include-cleaner)
          pthread_create(&timer_tid, NULL, timer_thread_func, &ta);

          err = error_stack_create();
          endgame_solve(solvers[player_on_turn], &args,
                        res[player_on_turn], err);

          ta.done = true;
          pthread_join(timer_tid, NULL);
          double elapsed = ctimer_elapsed_seconds(&turn_timer);
          budget[player_on_turn] -= elapsed;
          rr.cumul_time[cfg] += elapsed;

          assert(error_stack_is_empty(err));
          error_stack_destroy(err);

          int depth = endgame_results_get_depth(res[player_on_turn],
                                                   ENDGAME_RESULT_BEST);

          const PVLine *pv = endgame_results_get_pvline(res[player_on_turn],
                                                        ENDGAME_RESULT_BEST);
          if (pv->num_moves == 0) {
            break;
          }

          SmallMove best = pv->moves[0];
          small_move_to_move(move_list->spare_move, &best,
                             game_get_board(game));

          if (turn_num > 0) {
            string_builder_add_string(move_log, " | ");
          }
          char player_label[16];
          (void)snprintf(player_label, sizeof(player_label), "P%d(%s)", player_on_turn + 1,
                         cfg_names[cfg]);
          string_builder_add_string(move_log, player_label);
          string_builder_add_string(move_log, ": ");
          string_builder_add_move(move_log, game_get_board(game),
                                  move_list->spare_move, ld, true);
          char depth_str[32];
          (void)snprintf(depth_str, sizeof(depth_str), " d%d %.1fs", depth,
                         elapsed);
          string_builder_add_string(move_log, depth_str);

          play_move(move_list->spare_move, game, NULL);
          player_turn_count[player_on_turn]++;
          turn_num++;
        }

        int s0 = equity_to_int(player_get_score(game_get_player(game, 0)));
        int s1 = equity_to_int(player_get_score(game_get_player(game, 1)));
        // Apply 10 pts/sec overtime penalty for each player over budget
        for (int p = 0; p < 2; p++) {
          if (budget[p] < 0) {
            double ot = -budget[p];
            int penalty = (int)(ot * 10.0 + 0.999); // ceil(10 * overtime_secs)
            printf("    [P%d(%s) overtime: %.2fs, -%d pts]\n", p + 1,
                   cfg_names[cfg_for_player[p]], ot, penalty);
            if (p == 0) {
              s0 -= penalty;
            } else {
              s1 -= penalty;
            }
            rr.cumul_overtime[cfg_for_player[p]] += ot;
          }
        }
        spread[dir] = s0 - s1;

        printf("    G%d P1=%s P2=%s: %s => %+d\n", dir + 1,
               cfg_names[cfg_for_player[0]], cfg_names[cfg_for_player[1]],
               string_builder_peek(move_log), spread[dir]);

        string_builder_destroy(move_log);
        for (int p = 0; p < 2; p++) {
          endgame_results_destroy(res[p]);
          endgame_solver_destroy(solvers[p]);
        }
      }

      net[pi] = spread[0] - spread[1];
      rr_record_pairing(&rr, pi, net[pi]);
      printf("    Net: %+d  (cumul %+d)\n", net[pi], rr.pair_net[pi]);
    }

    // RESULT: prefix: grep "^RESULT:" to extract per-game nets from long logs.
    printf("RESULT: game=%d", gi + 1);
    for (int pi = 0; pi < num_pairings; pi++) {
      printf(" %s=%+d(cumul%+d)", pair_labels[pi], net[pi], rr.pair_net[pi]);
    }
    printf("\n");
    rr_print_crosstable(&rr);
    (void)fflush(stdout);
  }

  printf("\n");
  printf("=================================================================="
         "======================================\n");
  printf("  Final results (%d games, P1=%.0fs P2=%.0fs):\n", games_played,
         p1_budget_sec, p2_budget_sec);
  rr_print_crosstable(&rr);
  printf("=================================================================="
         "======================================\n");
  (void)fflush(stdout);

  game_string_options_destroy(gso);
  free(cgp_lines);
  move_list_destroy(move_list);
  config_destroy(config);
}

void test_benchmark_timed_round_robin(void) {
  log_set_level(LOG_FATAL);
  run_timed_round_robin("/tmp/stuck_hard_endgame_cgps.txt", 0, 500, 20.0, 12.0);
}

void test_benchmark_timed_round_robin_nonstuck(void) {
  log_set_level(LOG_FATAL);
  run_timed_round_robin("/tmp/nonstuck_cgps2.txt", 0, 500, 1.0, 0.4);
}

// ============================================================
// 4-way round robin: Static vs Bullet vs Blitz vs Classical
// ============================================================
// Generates random endgame positions via greedy self-play to bag-empty.
// Each position is classified as stuck or nonstuck; per-player budgets
// are set accordingly.  All solver players use F config (precheck+EBF).
//
//   Static    — greedy best move from movegen, no solver
//   Bullet    — nonstuck P1=1s/P2=0.4s   stuck P1=3s/P2=2s
//   Blitz     — nonstuck P1=3s/P2=1s     stuck P1=9s/P2=6s
//   Classical — nonstuck P1=10s/P2=4s    stuck P1=90s/P2=60s
//
// 6 pairings × 2 directions = 12 sub-games per position.
// Cross-table (ranked by W-L) is printed after every position so results
// are usable on CTRL+C.
//
// Greppable output:
//   grep "^RESULT:" output.log   # per-game summaries
//   grep "^CTROW:"  output.log   # cross-table rows

// Detect whether either player has at least one stuck tile.
// Uses MOVE_RECORD_TILES_PLAYED to find which machine letters have legal
// single-player plays; any rack tile absent from that set is stuck.
static bool position_any_stuck(Game *game, MoveList *move_list) {
  const LetterDistribution *ld = game_get_ld(game);
  int ld_size = ld_get_size(ld);
  int saved_pot = game_get_player_on_turn_index(game);

  for (int player_idx = 0; player_idx < 2; player_idx++) {
    if (player_idx != saved_pot) {
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

    if (player_idx != saved_pot) {
      game_set_player_on_turn_index(game, saved_pot);
    }

    const Rack *rack = player_get_rack(game_get_player(game, player_idx));
    for (int ml = 0; ml < ld_size; ml++) {
      if (rack_get_letter(rack, ml) > 0 && !(tiles_bv & ((uint64_t)1 << ml))) {
        return true;
      }
    }
  }
  return false;
}

static void run_four_way_round_robin(int num_games, uint64_t base_seed) {
  const int max_ply = 25;
  const int max_turns = 50;
  const double tt_frac =
      (2.0 * 1024.0 * 1024.0 * 1024.0) / (double)get_total_memory();

  const char *cfg_names[] = {"Static", "Bullet", "Blitz", "Classical"};

  // cfg_budgets[cfg][is_stuck][player_on_turn]: initial per-game budget.
  //   cfg=0 (Static) has no budget — greedy movegen, no solver.
  //   is_stuck: 0=nonstuck, 1=stuck.
  //   player_on_turn: 0=P1, 1=P2.
  const double cfg_budgets[4][2][2] = {
      {{0.0, 0.0}, {0.0, 0.0}},    // Static: unused
      {{1.0, 0.4}, {3.0, 2.0}},    // Bullet
      {{3.0, 1.0}, {9.0, 6.0}},    // Blitz
      {{10.0, 4.0}, {90.0, 60.0}}, // Classical
  };

  // All 6 C(4,2) pairings.
  const int num_pairings = 6;
  const int pair_a[] = {0, 0, 0, 1, 1, 2};
  const int pair_b[] = {1, 2, 3, 2, 3, 3};
  const char *pair_labels[] = {"St-Bu", "St-Bl", "St-Cl",
                               "Bu-Bl", "Bu-Cl", "Bl-Cl"};

  RRState rr;
  memset(&rr, 0, sizeof(rr));
  rr.num_cfgs = 4;
  rr.num_pairings = num_pairings;
  for (int cfg_idx = 0; cfg_idx < 4; cfg_idx++) {
    rr.cfg_names[cfg_idx] = cfg_names[cfg_idx];
  }
  for (int pi = 0; pi < num_pairings; pi++) {
    rr.pair_a[pi] = pair_a[pi];
    rr.pair_b[pi] = pair_b[pi];
    rr.pair_labels[pi] = pair_labels[pi];
  }

  Config *config = config_create_or_die(
      "set -lex CSW21 -threads 6 -s1 score -s2 score -r1 small -r2 small");
  MoveList *move_list = move_list_create(1);
  exec_config_quiet(config, "new");
  Game *game = config_get_game(config);

  printf("\n");
  printf("========================================================"
         "========================\n");
  printf("  4-Way Round Robin: up to %d games, 8 threads, seed=%llu\n",
         num_games, (unsigned long long)base_seed);
  printf("  Static    = greedy movegen (no solver)\n");
  printf("  Bullet    = nonstuck P1=1s/P2=0.4s   stuck P1=3s/P2=2s\n");
  printf("  Blitz     = nonstuck P1=3s/P2=1s     stuck P1=9s/P2=6s\n");
  printf("  Classical = nonstuck P1=10s/P2=4s    stuck P1=90s/P2=60s\n");
  printf("  Solver configs use F (precheck+EBF+75%%bail). 12 sub-games/pos.\n");
  printf("========================================================"
         "========================\n");
  (void)fflush(stdout);

  int games_played = 0;
  int attempts = 0;

  for (int gi = 0; games_played < num_games; gi++) {
    attempts++;
    game_reset(game);
    game_seed(game, base_seed + (uint64_t)gi);
    draw_starting_racks(game);

    if (!play_until_bag_empty(game, move_list)) {
      continue;
    }
    if (game_get_game_end_reason(game) != GAME_END_REASON_NONE) {
      continue;
    }

    // Detect stuckness before saving CGP (generate_moves may update
    // cross-sets, but the board/rack CGP fields are unaffected).
    bool is_stuck = position_any_stuck(game, move_list);
    char *cgp = game_get_cgp(game, true);

    if (is_stuck) {
      rr.stuck_count++;
    }
    games_played++;
    rr.games_played++;

    printf("\n=== Game %d (attempt %d, %s) ===\n", games_played, attempts,
           is_stuck ? "stuck" : "nonstuck");
    (void)fflush(stdout);

    int net[RR_MAX_PAIRINGS];
    for (int pi = 0; pi < num_pairings; pi++) {
      int ca = pair_a[pi];
      int cb = pair_b[pi];
      int spread[2];

      for (int dir = 0; dir < 2; dir++) {
        int cfg_for_player[2];
        cfg_for_player[0] = dir == 0 ? ca : cb;
        cfg_for_player[1] = dir == 0 ? cb : ca;

        ErrorStack *err = error_stack_create();
        game_load_cgp(game, cgp, err);
        assert(error_stack_is_empty(err));
        error_stack_destroy(err);

        EndgameSolver *solvers[2] = {endgame_solver_create(),
                                     endgame_solver_create()};
        EndgameResults *res[2] = {endgame_results_create(),
                                  endgame_results_create()};
        ThreadControl *tc = config_get_thread_control(config);

        // Each player's initial budget depends on their config, role (P1/P2),
        // and whether the position is stuck.
        int sk = is_stuck ? 1 : 0;
        double budget[2] = {
            cfg_budgets[cfg_for_player[0]][sk][0],
            cfg_budgets[cfg_for_player[1]][sk][1],
        };
        int player_turn_count[2] = {0, 0};
        int turn_num = 0;

        while (game_get_game_end_reason(game) == GAME_END_REASON_NONE &&
               turn_num < max_turns) {
          int player_on_turn = game_get_player_on_turn_index(game);
          int cfg = cfg_for_player[player_on_turn];

          if (cfg == 0) {
            // Static: greedy best move from movegen, no time management.
            Timer t;
            ctimer_start(&t);
            Move *move = get_top_equity_move(game, 0, move_list);
            play_move(move, game, NULL);
            rr.cumul_time[0] += ctimer_elapsed_seconds(&t);
          } else {
            // Solver player: F config — precheck enabled, EBF time mode.
            int tiles_on_rack = rack_get_total_letters(
                player_get_rack(game_get_player(game, player_on_turn)));
            EndgameTurnLimits limits = endgame_compute_turn_limits(
                budget[player_on_turn], player_turn_count[player_on_turn],
                tiles_on_rack, 1);

            EndgameArgs args = {.game = game,
                                .thread_control = tc,
                                .plies = max_ply,
                                .tt_fraction_of_mem = tt_frac,
                                .initial_small_move_arena_size =
                                    DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
                                .num_threads = 8,
                                .num_top_moves = 1,
                                .use_heuristics = true,
                                .per_ply_callback = NULL,
                                .per_ply_callback_data = NULL,
                                .skip_pruned_cross_sets =
                                    false, // F: precheck enabled
                                .forced_pass_bypass = false,
                                .soft_time_limit = limits.soft_limit,
                                .hard_time_limit = limits.hard_limit};

            thread_control_set_status(tc, THREAD_CONTROL_STATUS_STARTED);

            TimerArgs ta = {
                .tc = tc, .seconds = limits.turn_limit, .done = false};
            pthread_t timer_tid; // NOLINT(misc-include-cleaner)
            pthread_create(&timer_tid, NULL, timer_thread_func, &ta);

            Timer t;
            ctimer_start(&t);
            err = error_stack_create();
            endgame_solve(solvers[player_on_turn], &args,
                          res[player_on_turn], err);
            ta.done = true;
            pthread_join(timer_tid, NULL);
            double elapsed = ctimer_elapsed_seconds(&t);

            budget[player_on_turn] -= elapsed;
            rr.cumul_time[cfg] += elapsed;

            assert(error_stack_is_empty(err));
            error_stack_destroy(err);

            const PVLine *pv =
                endgame_results_get_pvline(res[player_on_turn],
                                           ENDGAME_RESULT_BEST);
            if (pv->num_moves == 0) {
              break;
            }
            SmallMove best = pv->moves[0];
            small_move_to_move(move_list->spare_move, &best,
                               game_get_board(game));
            play_move(move_list->spare_move, game, NULL);
          }
          player_turn_count[player_on_turn]++;
          turn_num++;
        }

        int s0 = equity_to_int(player_get_score(game_get_player(game, 0)));
        int s1 = equity_to_int(player_get_score(game_get_player(game, 1)));
        for (int p = 0; p < 2; p++) {
          int cfg = cfg_for_player[p];
          if (cfg != 0 && budget[p] < 0) {
            double ot = -budget[p];
            rr.cumul_overtime[cfg] += ot;
            int penalty = (int)(ot * 10.0 + 0.999);
            printf("    [P%d(%s) overtime: %.2fs, -%d pts]\n", p + 1,
                   cfg_names[cfg], ot, penalty);
            if (p == 0) {
              s0 -= penalty;
            } else {
              s1 -= penalty;
            }
          }
        }
        spread[dir] = s0 - s1;

        printf("  %s G%d P1=%-9s P2=%-9s => %+d (%d turns)\n", pair_labels[pi],
               dir + 1, cfg_names[cfg_for_player[0]],
               cfg_names[cfg_for_player[1]], spread[dir], turn_num);
        (void)fflush(stdout);

        for (int p = 0; p < 2; p++) {
          endgame_results_destroy(res[p]);
          endgame_solver_destroy(solvers[p]);
        }
      } // dir

      net[pi] = spread[0] - spread[1];
      rr_record_pairing(&rr, pi, net[pi]);
      printf("  %s net: %+d  cumul: %+d\n", pair_labels[pi], net[pi],
             rr.pair_net[pi]);
    } // pi

    free(cgp);

    // RESULT: prefix: grep "^RESULT:" for per-game summaries in long logs.
    printf("RESULT: game=%d %s", games_played, is_stuck ? "stuck" : "nonstuck");
    for (int pi = 0; pi < num_pairings; pi++) {
      printf(" %s=%+d(cumul%+d)", pair_labels[pi], net[pi], rr.pair_net[pi]);
    }
    printf("\n");
    rr_print_crosstable(&rr);
    (void)fflush(stdout);
  }

  printf("\n");
  printf("========================================================"
         "========================\n");
  printf("  Final results (%d games from %d attempts, seed=%llu):\n",
         games_played, attempts, (unsigned long long)base_seed);
  rr_print_crosstable(&rr);
  printf("========================================================"
         "========================\n");
  (void)fflush(stdout);

  move_list_destroy(move_list);
  config_destroy(config);
}

void test_benchmark_four_way_round_robin(void) {
  log_set_level(LOG_FATAL);
  run_four_way_round_robin(500, 271828);
}

// --- Overnight precheck game-pair benchmark with per-depth logging ---

// Callback context for per-depth logging
typedef struct {
  Timer turn_timer;            // Started at beginning of each turn
  FILE *log_fp;                // Log file for per-depth data
  const EndgameSolver *solver; // For querying root_total in callback
  double turn_limit_s;         // Time budget allocated for this turn
  int game_id;                 // Current seed number
  int dir;                     // Direction: 0 or 1 (side-swap)
  int config_idx;              // 0=no-precheck, 1=with-precheck
  int turn_num;                // Turn number within this game
  int player_on_turn;          // Which player (0 or 1)
} DepthLogContext;

// Per-depth callback: logs depth completion with value and best move
static void depth_log_callback(int depth, int32_t value, const PVLine *pv_line,
                               const Game *game, const PVLine *ranked_pvs,
                               int num_ranked_pvs, void *user_data) {
  DepthLogContext *ctx = (DepthLogContext *)user_data;
  double elapsed = ctimer_elapsed_seconds(&ctx->turn_timer);

  // Format best move from PV
  char move_str[64] = "?";
  if (pv_line->num_moves > 0) {
    const LetterDistribution *ld = game_get_ld(game);
    Move move;
    small_move_to_move(&move, &pv_line->moves[0], game_get_board(game));
    StringBuilder *sb = string_builder_create();
    string_builder_add_move(sb, game_get_board(game), &move, ld, true);
    (void)snprintf(move_str, sizeof(move_str), "%s", string_builder_peek(sb));
    string_builder_destroy(sb);
  }

  // Query actual root move count from solver
  int cur_depth_ignored = 0;
  int root_done_ignored = 0;
  int root_total = 0;
  int p2_done_ignored = 0;
  int p2_total_ignored = 0;
  endgame_solver_get_progress(ctx->solver, &cur_depth_ignored,
                              &root_done_ignored, &root_total, &p2_done_ignored,
                              &p2_total_ignored);

  const char *cfg_name = ctx->config_idx == 0 ? "nopc" : "pc";
  (void)fprintf(ctx->log_fp, "%d,%d,%s,%d,P%d,d,%d,%.4f,%d,%d,,,,%s,,\n",
                ctx->game_id, ctx->dir, cfg_name, ctx->turn_num,
                ctx->player_on_turn + 1, depth, elapsed, value, root_total,
                move_str);

  (void)ranked_pvs;
  (void)num_ranked_pvs;
}

// Poll thread: samples TT stats once per second during a search
typedef struct {
  Timer *turn_timer;
  FILE *log_fp;
  const EndgameSolver *solver;
  const EndgameResults *results;
  int game_id;
  int dir;
  int config_idx;
  int turn_num;
  int player_on_turn;
  volatile bool done;
} PollContext;

static void *poll_thread_func(void *arg) {
  PollContext *ctx = (PollContext *)arg;
  const char *cfg_name = ctx->config_idx == 0 ? "nopc" : "pc";

  while (!ctx->done) {
    struct timespec ts = {1, 0}; // 1 second
    nanosleep(&ts, NULL);
    if (ctx->done) {
      break;
    }

    double elapsed = ctimer_elapsed_seconds(ctx->turn_timer);

    int cur_depth = 0;
    int root_done = 0;
    int root_total = 0;
    int p2_done = 0;
    int p2_total = 0;
    endgame_solver_get_progress(ctx->solver, &cur_depth, &root_done,
                                &root_total, &p2_done, &p2_total);

    const TranspositionTable *tt =
        endgame_solver_get_transposition_table(ctx->solver);
    int lookups = 0;
    int hits = 0;
    int created = 0;
    if (tt) {
      lookups = atomic_load(&tt->lookups);
      hits = atomic_load(&tt->hits);
      created = atomic_load(&tt->created);
    }

    // type=p, depth=current_depth, value=root_done/root_total
    (void)fprintf(ctx->log_fp,
                  "%d,%d,%s,%d,P%d,p,%d,%.4f,%d/%d,,%d,%d,%d,,%d/%d,,\n",
                  ctx->game_id, ctx->dir, cfg_name, ctx->turn_num,
                  ctx->player_on_turn + 1, cur_depth, elapsed, root_done,
                  root_total, lookups, hits, created, p2_done, p2_total);
    (void)fflush(ctx->log_fp);
  }
  return NULL;
}

// Game-pair benchmark: precheck vs no-precheck with side-swapping.
// For each seed position, plays two games alternating strategies:
//   Game 1: P1=precheck, P2=no-precheck
//   Game 2: P1=no-precheck, P2=precheck
// Net = spread(G1) - spread(G2). Positive = precheck stronger.
// Each player has their own solver/TT. Simple 80% time management.
// Per-depth progress logged for EBF analysis.
static void run_overnight_gamepairs(int num_games, double p1_budget_sec,
                                    double p2_budget_sec, uint64_t base_seed) {
  const int max_ply = 25;
  const int max_turns = 50;
  const double tt_frac =
      (2.0 * 1024.0 * 1024.0 * 1024.0) / (double)get_total_memory();

  Config *config = config_create_or_die(
      "set -lex CSW21 -threads 6 -s1 score -s2 score -r1 small -r2 small");

  MoveList *move_list = move_list_create(1);
  exec_config_quiet(config, "new");
  Game *game = config_get_game(config);

  // Open log file for per-depth data
  FILE *log_fp = fopen("/tmp/overnight_depth_log.csv", "we");
  assert(log_fp);
  (void)fprintf(log_fp,
                "seed,dir,config,turn,player,type,depth,elapsed_s,value,"
                "n_root_moves,tt_lookups,tt_hits,tt_created,best_move,"
                "ply2_progress,turn_limit_s,timed_out\n");
  (void)fflush(log_fp);

  printf("\n");
  printf("=================================================================="
         "======================================\n");
  printf("  Overnight Game Pairs: up to %d seeds, P1=%.0fs P2=%.0fs budget, "
         "8 threads, seed=%llu\n",
         num_games, p1_budget_sec, p2_budget_sec,
         (unsigned long long)base_seed);
  printf("  Side-swapping: G1=pc/nopc  G2=nopc/pc\n");
  printf("  Time management: 80%% of remaining budget per turn (simple)\n");
  printf("  Per-depth log: /tmp/overnight_depth_log.csv\n");
  printf("=================================================================="
         "======================================\n");
  (void)fflush(stdout);

  int total_net = 0;
  int pc_better = 0;
  int nopc_better = 0;
  int same_count = 0;
  int games_played = 0;
  int attempts = 0;
  GameStringOptions *gso = game_string_options_create_default();

  for (int gi = 0; games_played < num_games; gi++) {
    attempts++;
    game_reset(game);
    game_seed(game, base_seed + (uint64_t)gi);
    draw_starting_racks(game);

    if (!play_until_bag_empty(game, move_list)) {
      continue;
    }
    if (game_get_game_end_reason(game) != GAME_END_REASON_NONE) {
      continue;
    }

    char *cgp = game_get_cgp(game, true);
    games_played++;

    // Print position
    printf("\n=== Seed %d (attempt %d) ===\n", games_played, attempts);
    {
      ErrorStack *err = error_stack_create();
      game_load_cgp(game, cgp, err);
      assert(error_stack_is_empty(err));
      error_stack_destroy(err);
      StringBuilder *game_sb = string_builder_create();
      string_builder_add_game(game, NULL, gso, NULL, game_sb);
      printf("%s\n", string_builder_peek(game_sb));
      string_builder_destroy(game_sb);
    }

    // Log CGP to file for reproducibility
    (void)fprintf(log_fp, "# seed %d: %s\n", games_played, cgp);
    (void)fflush(log_fp);

    // Play two games with swapped sides:
    //   dir=0: P1=precheck(skip=false), P2=no-precheck(skip=true)
    //   dir=1: P1=no-precheck(skip=true), P2=precheck(skip=false)
    int spread[2] = {0, 0};

    DepthLogContext depth_ctx;
    depth_ctx.log_fp = log_fp;
    depth_ctx.game_id = games_played;

    for (int dir = 0; dir < 2; dir++) {
      // skip_precheck per player: dir=0 → P1=pc P2=nopc, dir=1 → P1=nopc P2=pc
      bool skip_precheck_for_player[2];
      const char *label_for_player[2];
      if (dir == 0) {
        skip_precheck_for_player[0] = false; // P1 = precheck
        skip_precheck_for_player[1] = true;  // P2 = no precheck
        label_for_player[0] = "pc";
        label_for_player[1] = "nopc";
      } else {
        skip_precheck_for_player[0] = true;  // P1 = no precheck
        skip_precheck_for_player[1] = false; // P2 = precheck
        label_for_player[0] = "nopc";
        label_for_player[1] = "pc";
      }

      depth_ctx.dir = dir;

      ErrorStack *err = error_stack_create();
      game_load_cgp(game, cgp, err);
      assert(error_stack_is_empty(err));
      error_stack_destroy(err);

      // Each player gets their own solver (separate TT)
      EndgameSolver *solvers[2] = {endgame_solver_create(),
                                   endgame_solver_create()};
      EndgameResults *results[2] = {endgame_results_create(),
                                    endgame_results_create()};
      ThreadControl *tc = config_get_thread_control(config);

      double budget[2] = {p1_budget_sec, p2_budget_sec};
      int turns = 0;

      printf("  G%d: P1=%s P2=%s\n", dir + 1, label_for_player[0],
             label_for_player[1]);

      while (game_get_game_end_reason(game) == GAME_END_REASON_NONE &&
             turns < max_turns) {
        int player_on_turn = game_get_player_on_turn_index(game);

        // Simple time management: 80% of remaining budget
        double turn_limit = budget[player_on_turn] * 0.80;
        if (turn_limit < 0.1) {
          turn_limit = 0.1;
        }

        depth_ctx.config_idx = skip_precheck_for_player[player_on_turn] ? 0 : 1;
        depth_ctx.turn_num = turns;
        depth_ctx.player_on_turn = player_on_turn;
        depth_ctx.solver = solvers[player_on_turn];
        depth_ctx.turn_limit_s = turn_limit;
        ctimer_start(&depth_ctx.turn_timer);

        const char *cfg_name_start = depth_ctx.config_idx == 0 ? "nopc" : "pc";
        (void)fprintf(log_fp, "%d,%d,%s,%d,P%d,start,,,,,,,,,,%.4f,\n",
                      games_played, dir, cfg_name_start, turns,
                      player_on_turn + 1, turn_limit);

        EndgameArgs args = {.game = game,
                            .thread_control = tc,
                            .plies = max_ply,
                            .tt_fraction_of_mem = tt_frac,
                            .initial_small_move_arena_size =
                                DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE,
                            .num_threads = 8,
                            .num_top_moves = 1,
                            .use_heuristics = true,
                            .per_ply_callback = depth_log_callback,
                            .per_ply_callback_data = &depth_ctx,
                            .forced_pass_bypass = false,
                            .soft_time_limit = 0,
                            .hard_time_limit = 0};

        thread_control_set_status(tc, THREAD_CONTROL_STATUS_STARTED);

        TimerArgs ta = {.tc = tc, .seconds = turn_limit, .done = false};
        pthread_t timer_tid; // NOLINT(misc-include-cleaner)
        pthread_create(&timer_tid, NULL, timer_thread_func, &ta);

        PollContext poll_ctx = {.turn_timer = &depth_ctx.turn_timer,
                                .log_fp = log_fp,
                                .solver = solvers[player_on_turn],
                                .results = results[player_on_turn],
                                .game_id = games_played,
                                .dir = dir,
                                .config_idx = depth_ctx.config_idx,
                                .turn_num = turns,
                                .player_on_turn = player_on_turn,
                                .done = false};
        pthread_t poll_tid; // NOLINT(misc-include-cleaner)
        pthread_create(&poll_tid, NULL, poll_thread_func, &poll_ctx);

        Timer t;
        ctimer_start(&t);
        err = error_stack_create();
        endgame_solve(solvers[player_on_turn], &args,
                      results[player_on_turn], err);
        double elapsed = ctimer_elapsed_seconds(&t);

        poll_ctx.done = true;
        pthread_join(poll_tid, NULL);
        ta.done = true;
        pthread_join(timer_tid, NULL);

        assert(error_stack_is_empty(err));
        error_stack_destroy(err);

        // Log type=end: final depth, elapsed, and whether timed out
        int timed_out = (thread_control_get_status(tc) ==
                         THREAD_CONTROL_STATUS_USER_INTERRUPT)
                            ? 1
                            : 0;
        int end_depth =
            endgame_results_get_depth(results[player_on_turn],
                                      ENDGAME_RESULT_BEST);
        const char *cfg_name_end = depth_ctx.config_idx == 0 ? "nopc" : "pc";
        (void)fprintf(log_fp, "%d,%d,%s,%d,P%d,end,%d,%.4f,,,,,,,,,%d\n",
                      games_played, dir, cfg_name_end, turns,
                      player_on_turn + 1, end_depth, elapsed, timed_out);
        (void)fflush(log_fp);

        budget[player_on_turn] -= elapsed;

        int depth = end_depth;

        const PVLine *pv =
            endgame_results_get_pvline(results[player_on_turn],
                                       ENDGAME_RESULT_BEST);

        // Format move for display
        char move_str[64] = "none";
        if (pv->num_moves > 0) {
          const LetterDistribution *ld = game_get_ld(game);
          small_move_to_move(move_list->spare_move, &pv->moves[0],
                             game_get_board(game));
          StringBuilder *sb = string_builder_create();
          string_builder_add_move(sb, game_get_board(game),
                                  move_list->spare_move, ld, true);
          (void)snprintf(move_str, sizeof(move_str), "%s",
                         string_builder_peek(sb));
          string_builder_destroy(sb);
        }

        printf("    t%d P%d(%s) d%d %.2fs bud=%.1f %s\n", turns,
               player_on_turn + 1, label_for_player[player_on_turn], depth,
               elapsed, budget[player_on_turn], move_str);

        if (pv->num_moves == 0) {
          break;
        }

        SmallMove best = pv->moves[0];
        small_move_to_move(move_list->spare_move, &best, game_get_board(game));
        play_move(move_list->spare_move, game, NULL);
        turns++;
      }

      int s0 = equity_to_int(player_get_score(game_get_player(game, 0)));
      int s1 = equity_to_int(player_get_score(game_get_player(game, 1)));
      spread[dir] = s0 - s1;

      printf("  G%d spread: %+d (%d turns)\n", dir + 1, spread[dir], turns);

      for (int p = 0; p < 2; p++) {
        endgame_results_destroy(results[p]);
        endgame_solver_destroy(solvers[p]);
      }
    }

    free(cgp);

    // Net = spread(G1) - spread(G2).
    // G1 has precheck as P1, G2 has nopc as P1.
    // Positive net = precheck is stronger.
    int net = spread[0] - spread[1];
    total_net += net;
    if (net > 0) {
      pc_better++;
    } else if (net < 0) {
      nopc_better++;
    } else {
      same_count++;
    }

    printf("  Net: %+d  (cumul %+d, pc+%d nopc+%d =%d)\n", net, total_net,
           pc_better, nopc_better, same_count);
    (void)fflush(stdout);
    (void)fflush(log_fp);
  }

  printf("\n");
  printf("=================================================================="
         "======================================\n");
  printf("  Results (%d seeds from %d attempts, P1=%.0fs P2=%.0fs, "
         "seed=%llu):\n",
         games_played, attempts, p1_budget_sec, p2_budget_sec,
         (unsigned long long)base_seed);
  printf("    Precheck better: %d  |  No-precheck better: %d  |  Same: %d\n",
         pc_better, nopc_better, same_count);
  printf("    Total net spread: %+d (avg %+.2f per seed)\n", total_net,
         games_played > 0 ? (double)total_net / games_played : 0.0);
  printf("  Per-depth log written to: /tmp/overnight_depth_log.csv\n");
  printf("=================================================================="
         "======================================\n");
  (void)fflush(stdout);

  (void)fclose(log_fp);
  game_string_options_destroy(gso);
  move_list_destroy(move_list);
  config_destroy(config);
}

void test_benchmark_overnight_gamepairs(void) {
  log_set_level(LOG_FATAL);
  run_overnight_gamepairs(100, 15.0, 9.0, 31415926);
}
