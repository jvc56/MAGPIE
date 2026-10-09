#include "autoplay_solvers_test.h"

#include "../src/compat/cpthread.h"
#include "../src/compat/ctime.h"
#include "../src/def/cpthread_defs.h"
#include "../src/def/thread_control_defs.h"
#include "../src/ent/autoplay_solver_settings.h"
#include "../src/ent/game.h"
#include "../src/ent/move.h"
#include "../src/ent/thread_control.h"
#include "../src/impl/autoplay_solvers.h"
#include "../src/impl/config.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

// Sets the run's stop after a delay, as the user's `stop` or a contribute
// task's time limit does, and notes when; then waits for the solve to come
// back, ending the test binary if it has not within a minute, so a stop the
// solve never sees fails the test rather than hanging it.
typedef struct Stopper {
  ThreadControl *run_thread_control;
  double delay_seconds;
  int64_t stopped_at_ns;
  atomic_int solve_returned;
} Stopper;

static void *stopper_main(void *arg) {
  Stopper *stopper = (Stopper *)arg;
  ctime_nap(stopper->delay_seconds);
  stopper->stopped_at_ns = ctimer_monotonic_ns();
  thread_control_set_status(stopper->run_thread_control,
                            THREAD_CONTROL_STATUS_USER_INTERRUPT);
  for (int i = 0; i < 600 && !atomic_load(&stopper->solve_returned); i++) {
    ctime_nap(0.1);
  }
  if (!atomic_load(&stopper->solve_returned)) {
    log_fatal("a solve was still running a minute after the run was stopped");
  }
  return NULL;
}

// Solves `cgp` with `settings` on 2 threads, stopping the run `delay`
// seconds in, and checks the solve comes back within 2 seconds of the stop,
// without a move or an error: autoplay then plays the turn statically.
static void assert_a_stop_ends_the_solve(const char *name, const char *cgp,
                                         const AutoplaySolverSettings *settings,
                                         double delay) {
  Config *config = config_create_or_die(
      "set -s1 equity -s2 equity -r1 best -r2 best -threads 2");
  load_and_exec_config_or_die(config, cgp);
  Game *game = config_get_game(config);
  ThreadControl *run_thread_control = thread_control_create();
  thread_control_set_status(run_thread_control, THREAD_CONTROL_STATUS_STARTED);
  AutoplaySolverCtx *ctx = autoplay_solver_ctx_create();
  ErrorStack *error_stack = error_stack_create();

  Stopper stopper = {.run_thread_control = run_thread_control,
                     .delay_seconds = delay};
  atomic_init(&stopper.solve_returned, 0);
  cpthread_t stopper_thread;
  const int64_t started_at_ns = ctimer_monotonic_ns();
  cpthread_create(&stopper_thread, stopper_main, &stopper);
  const Move *move =
      autoplay_solver_solve(ctx, settings, game, /*shared_tt=*/NULL,
                            /*num_threads=*/2, /*seed=*/7, run_thread_control,
                            /*record=*/true, error_stack);
  const int64_t returned_at_ns = ctimer_monotonic_ns();
  atomic_store(&stopper.solve_returned, 1);
  cpthread_join(stopper_thread);

  const double ran = (double)(returned_at_ns - started_at_ns) / 1e9;
  const double after_stop =
      (double)(returned_at_ns - stopper.stopped_at_ns) / 1e9;
  printf("%s: stopped %.2f s in, returned %.3f s after the stop\n", name, delay,
         after_stop);
  // It was still solving when the stop came, and came back promptly.
  assert(ran >= delay);
  assert(after_stop < 2.0);
  assert(!move);
  assert(error_stack_is_empty(error_stack));

  // Stopped before it starts, a solve does not start.
  move = autoplay_solver_solve(ctx, settings, game, NULL, 2, 7,
                               run_thread_control, false, error_stack);
  assert(!move && error_stack_is_empty(error_stack));

  error_stack_destroy(error_stack);
  autoplay_solver_ctx_destroy(ctx);
  thread_control_destroy(run_thread_control);
  config_destroy(config);
}

// The solvers make their own thread controls, which a stop of the run
// (autoplay's thread control: the user's `stop`, a contribute task's time
// limit) used to never reach: a long endgame or PEG solve ran on to its end
// however long that took. Each now sees the run's stop through its own
// thread control and comes back at once without a move.
static void test_a_stop_ends_a_running_solve(void) {
  // Both racks full at 20 plies: minutes of search.
  AutoplaySolverSettings endgame_settings;
  autoplay_solver_settings_set_defaults(&endgame_settings);
  endgame_settings.endgame_plies = 20;
  assert_a_stop_ends_the_solve(
      "endgame",
      "cgp 7F3QINS/7E3E2H/6ORBITED1O/3JAMBU2OM2R/1ROATE1L2FAV1T/"
      "GI5ED1T1OPE/OD6R3WEN/EL5RUNTY2S/1E6N2U3/1Y5AKA1C3/ES1VAgUIsH1C3/"
      "X2I7A3/I2G11/LOOING9/E2A11 ADLRSTZ/EIINPW 451/296 0 -lex CSW24",
      &endgame_settings, 0.5);

  // Four tiles in the bag, both racks full, the full default schedule
  // without nested lookahead: far longer than the stop leaves it.
  AutoplaySolverSettings peg_settings;
  autoplay_solver_settings_set_defaults(&peg_settings);
  peg_settings.endgame_plies = 4;
  peg_settings.peg_max_bag = 4;
  peg_settings.peg_nested = false;
  assert_a_stop_ends_the_solve(
      "PEG",
      "cgp 3V3W6L/1BEATY1U5GI/2XU3S4FEN/3TA2H4LOY/2GEN1DUCAT1AD1/"
      "2O1I1I2WRITE1/2V1M1ZOAEA4/3JAGER2DRILL/2BOtONE5O1/1FERER7Q1/4S8U1/"
      "12NaM/12ATE/13ST/14H ACEINOP/DEIINOS 361/397 0 -lex CSW24",
      &peg_settings, 0.5);

  // A solve on a run that is not stopped still chooses its move.
  Config *config = config_create_or_die(
      "set -s1 equity -s2 equity -r1 best -r2 best -threads 2");
  load_and_exec_config_or_die(
      config, "cgp 7F3QINS/7E3E2H/6ORBITED1O/3JAMBU2OM2R/1ROATE1L2FAV1T/"
              "GI5ED1T1OPE/OD6R3WEN/EL5RUNTY2S/1E6N2U3/1Y5AKA1C3/ES1VAgUIsH1C3/"
              "X2I7A3/I2G11/LOOING9/E2A11 ADLRSTZ/EIINPW 451/296 0 -lex CSW24");
  ThreadControl *run_thread_control = thread_control_create();
  thread_control_set_status(run_thread_control, THREAD_CONTROL_STATUS_STARTED);
  endgame_settings.endgame_plies = 2;
  AutoplaySolverCtx *ctx = autoplay_solver_ctx_create();
  ErrorStack *error_stack = error_stack_create();
  const Move *move =
      autoplay_solver_solve(ctx, &endgame_settings, config_get_game(config),
                            NULL, 2, 7, run_thread_control, false, error_stack);
  assert(move && error_stack_is_empty(error_stack));
  error_stack_destroy(error_stack);
  autoplay_solver_ctx_destroy(ctx);
  thread_control_destroy(run_thread_control);
  config_destroy(config);
}

// A child thread control reads its parent's stop as its own, and keeps its own
// status to itself: setting it never touches the parent's.
static void test_a_thread_control_sees_its_parents_stop(void) {
  ThreadControl *parent = thread_control_create();
  ThreadControl *child = thread_control_create();
  thread_control_set_status(parent, THREAD_CONTROL_STATUS_STARTED);
  thread_control_set_parent(child, parent);
  thread_control_set_status(child, THREAD_CONTROL_STATUS_STARTED);
  assert(thread_control_get_status(child) == THREAD_CONTROL_STATUS_STARTED);
  thread_control_set_status(child, THREAD_CONTROL_STATUS_FINISHED);
  assert(thread_control_get_status(parent) == THREAD_CONTROL_STATUS_STARTED);
  thread_control_set_status(child, THREAD_CONTROL_STATUS_STARTED);
  thread_control_set_status(parent, THREAD_CONTROL_STATUS_USER_INTERRUPT);
  assert(thread_control_get_status(child) ==
         THREAD_CONTROL_STATUS_USER_INTERRUPT);
  // The parent's stop lifted (a task's time limit puts the run's status
  // back), the child runs again.
  thread_control_set_status(parent, THREAD_CONTROL_STATUS_STARTED);
  assert(thread_control_get_status(child) == THREAD_CONTROL_STATUS_STARTED);
  // A child stopped itself stays stopped whatever the parent says.
  thread_control_set_status(child, THREAD_CONTROL_STATUS_USER_INTERRUPT);
  assert(thread_control_get_status(child) ==
         THREAD_CONTROL_STATUS_USER_INTERRUPT);
  assert(thread_control_get_status(parent) == THREAD_CONTROL_STATUS_STARTED);
  thread_control_destroy(child);
  thread_control_destroy(parent);
}

void test_autoplay_solvers(void) {
  test_a_thread_control_sees_its_parents_stop();
  test_a_stop_ends_a_running_solve();
}
