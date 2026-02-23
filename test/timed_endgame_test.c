#include "timed_endgame_test.h"

#include "../src/compat/ctime.h"
#include "../src/def/equity_defs.h"
#include "../src/def/game_defs.h"
#include "../src/def/move_defs.h"
#include "../src/def/thread_control_defs.h"
#include "../src/ent/endgame_results.h"
#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/thread_control.h"
#include "../src/impl/cgp.h"
#include "../src/impl/config.h"
#include "../src/impl/endgame.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/move_gen.h"
#include "../src/impl/exec.h"
#include "../src/util/io_util.h"
#include "test_util.h"
#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
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
  double remaining = ta->seconds;
  while (remaining > 0 && !ta->done) {
    double sleep_time = remaining > 0.05 ? 0.05 : remaining;
    struct timespec ts;
    ts.tv_sec = (time_t)sleep_time;
    ts.tv_nsec = (long)((sleep_time - (double)ts.tv_sec) * 1e9);
    nanosleep(&ts, NULL);
    remaining -= sleep_time;
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
  int new_better = 0, old_better = 0, same_count = 0;
  double total_time_old = 0, total_time_new = 0;
  double global_max_turn_old = 0, global_max_turn_new = 0;
  int total_turns_old = 0, total_turns_new = 0;
  int total_exceeded_old = 0, total_exceeded_new = 0;

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
                            .tt_fraction_of_mem = 0.05,
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
        pthread_t timer_tid;
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

    printf(
        "  %4d  %+10d %+10d  %7.2fs %7.2fs  %+5d  %5d %5d  %5.1fs %5.1fs\n",
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
