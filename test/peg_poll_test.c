#include "peg_poll_test.h"

#include "../src/compat/cpthread.h"
#include "../src/def/cpthread_defs.h"
#include "../src/ent/board.h"
#include "../src/ent/game.h"
#include "../src/ent/letter_distribution.h"
#include "../src/impl/config.h"
#include "../src/impl/peg.h"
#include "../src/str/move_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

// A 2-in-bag PEG position (from notes/peg_positions/random_2peg.txt). Stage 0
// runs greedy playouts over the full candidate field, so the poll's leaderboard
// fills in quickly; a short time budget keeps the whole test ~1-2s wall.
static const char *const PEG_POLL_CGP =
    "cgp 4WILI3JANE/4A3AQUAE2/2C1N3K2Y3/2RUG3E2G3/2O1L3B1REV2/2U1EL2I1EEL2/"
    "2T1SI2A1S1Y2/R1O2PWNS1T4/E1N2I4A4/AH1MED4T4/GI1OPE4E4/IT1ZO2VOUDON2/N3D10/"
    "I3E10/COIFS10 BDFHMX?/AAORTT? 420/412 0 -lex CSW24";

typedef struct PollerArgs {
  PegPoll *poll;
  atomic_bool stop;
  int reads;
  int max_entries_seen;
  int max_stage_seen;
  uint64_t max_version_seen;
} PollerArgs;

// Polls the live leaderboard ~1 kHz, like a render loop would, until the solve
// reports done (or the caller asks it to stop). Records what it observed so the
// test can assert the poll actually advanced while peg_solve ran on the main
// thread.
static void *poller_main(void *arg) {
  PollerArgs *poller = (PollerArgs *)arg;
  PegPollSnapshot snap;
  while (!atomic_load(&poller->stop)) {
    peg_poll_read(poller->poll, &snap);
    poller->reads++;
    if (snap.n_entries > poller->max_entries_seen) {
      poller->max_entries_seen = snap.n_entries;
    }
    if (snap.stage > poller->max_stage_seen) {
      poller->max_stage_seen = snap.stage;
    }
    if (snap.version > poller->max_version_seen) {
      poller->max_version_seen = snap.version;
    }
    if (snap.done) {
      break;
    }
    const struct timespec nap = {0, 1L * 1000 * 1000}; // 1 ms
    nanosleep(&nap, NULL);
  }
  return NULL;
}

void test_peg_poll(void) {
  log_set_level(LOG_FATAL);
  Config *config =
      config_create_or_die("set -lex CSW24 -threads 4 -s1 score -s2 score");
  load_and_exec_config_or_die(config, PEG_POLL_CGP);
  Game *game = config_get_game(config);

  PegPoll *poll = peg_poll_create();

  // Initial state before the solve: nothing published yet.
  PegPollSnapshot initial;
  peg_poll_read(poll, &initial);
  assert(initial.stage == -1);
  assert(initial.n_entries == 0);
  assert(!initial.done);

  PollerArgs poller;
  memset(&poller, 0, sizeof(poller));
  poller.poll = poll;
  atomic_init(&poller.stop, false);
  cpthread_t poller_thread;
  cpthread_create(&poller_thread, poller_main, &poller);

  static const int stage_top_k[] = {4, 2};
  PegArgs args;
  memset(&args, 0, sizeof(args));
  args.game = game;
  args.thread_control = config_get_thread_control(config);
  args.num_threads = 4;
  args.time_budget_seconds = 1.5; // short; the deadline gate bounds the solve
  args.stage_top_k = stage_top_k;
  args.num_stages = 2;
  args.poll = poll;

  PegResult result;
  memset(&result, 0, sizeof(result));
  ErrorStack *error_stack = error_stack_create();
  peg_solve(&args, &result, error_stack);
  assert(error_stack_is_empty(error_stack));

  atomic_store(&poller.stop, true);
  cpthread_join(poller_thread);

  // The poller ran concurrently and saw the leaderboard populate and the
  // version counter advance while peg_solve was working.
  assert(poller.reads > 0);
  assert(poller.max_entries_seen >= 1);
  assert(poller.max_version_seen >= 1);
  assert(poller.max_stage_seen >= 0);

  // Final poll: done, non-empty, and its top entry matches the solver's
  // published best move (the poll and PegResult are refreshed together at each
  // stage boundary, so they must agree at the end).
  PegPollSnapshot final;
  peg_poll_read(poll, &final);
  assert(final.done);
  assert(final.n_entries >= 1);

  const Board *board = game_get_board(game);
  const LetterDistribution *ld = game_get_ld(game);
  StringBuilder *poll_best = string_builder_create();
  StringBuilder *solve_best = string_builder_create();
  string_builder_add_move(poll_best, board, &final.entries[0].move, ld, false);
  string_builder_add_move(solve_best, board, &result.best_move, ld, false);
  assert(strings_equal(string_builder_peek(poll_best),
                       string_builder_peek(solve_best)));

  printf("peg-poll: reads=%d max_entries=%d max_stage=%d versions=%llu "
         "best=%s win=%.1f\n",
         poller.reads, poller.max_entries_seen, poller.max_stage_seen,
         (unsigned long long) final.version, string_builder_peek(poll_best),
         100.0 * final.entries[0].win_pct);

  string_builder_destroy(poll_best);
  string_builder_destroy(solve_best);
  error_stack_destroy(error_stack);
  peg_result_destroy(&result);
  peg_poll_destroy(poll);
  config_destroy(config);
}
