// Sim benchmark: plays one full game with a timed sim per turn via
// autoplay and reports total sim iterations and iters/sec. The game
// trajectory is fixed via autoplay_set_bench_static_move so different
// RIT/BAI/cache variants traverse identical positions and the numbers
// are directly comparable.
//
// Run as simbench for the defaults, or simbench:<key>=<value>,... to change
// them:
//   plies    sim depth                      (default 2)
//   threads  worker threads                 (default detected hardware
//            concurrency)
//   tlim     seconds per turn               (default 2)
//   mi       -minplayiterations             (default 100000)
//   wmp      -wmp                           (default true)
//   rit      -rit                           (default true)
//   wit      -wit                           (default true)
// For example: simbench:plies=4,threads=16,wit=false

#include "sim_benchmark_test.h"

#include "../src/compat/ctime.h"
#include "../src/compat/memory_info.h"
#include "../src/impl/autoplay.h"
#include "../src/impl/config.h"
#include "../src/impl/play_chooser.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct SimBenchArgs {
  int plies;
  int threads;
  const char *tlim;
  const char *mi;
  const char *wmp;
  const char *rit;
  const char *wit;
} SimBenchArgs;

static SimBenchArgs sim_bench_default_args(void) {
  return (SimBenchArgs){.plies = 2,
                        .threads = get_num_cores(),
                        .tlim = "2",
                        .mi = "100000",
                        .wmp = "true",
                        .rit = "true",
                        .wit = "true"};
}

static void run_sim_benchmark(const SimBenchArgs *args) {
  Timer timer;
  ctimer_start(&timer);

  autoplay_reset_total_sim_iterations();
  // Fix the game trajectory by playing the top-equity static move at every
  // turn, regardless of what the sim picks. The sim still runs fully, so
  // per-turn iteration throughput is measured, but different RIT/BAI
  // variants now traverse identical positions.
  autoplay_set_bench_static_move(true);

  char cmd[256];
  (void)snprintf(cmd, sizeof(cmd),
                 "set -lex CSW24 -wmp %s -rit %s -wit %s -s1 equity "
                 "-s2 equity "
                 "-r1 all -r2 all -numplays 15 -plies %d -threads %d "
                 "-tlim %s -seed 42 -sr tt -minplayiterations %s",
                 args->wmp, args->rit, args->wit, args->plies, args->threads,
                 args->tlim, args->mi);
  Config *config = config_create_or_die(cmd);

  load_and_exec_config_or_die(config, "autoplay games 1");

  ctimer_stop(&timer);
  const double elapsed = ctimer_elapsed_seconds(&timer);
  const uint64_t iters = autoplay_get_total_sim_iterations();
  printf("plies=%d: %.1fs, iters=%llu (%.0f iters/sec)\n", args->plies,
         elapsed, (unsigned long long)iters, (double)iters / elapsed);

  config_destroy(config);
}

void test_sim_benchmark(void) {
  const SimBenchArgs args = sim_bench_default_args();
  run_sim_benchmark(&args);
}

// The integer after "<key>=" in item, a setting of the benchmark bench.
static int bench_spec_int(const char *bench, const char *item,
                          const char *key) {
  ErrorStack *error_stack = error_stack_create();
  const int value = string_to_int(item + strlen(key) + 1, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    log_fatal("%s %s must be an integer, got '%s'", bench, key, item);
  }
  error_stack_destroy(error_stack);
  return value;
}

// "<key>=<value>[,<key>=<value>...]", with the keys listed above.
void sim_benchmark_run_spec(const char *spec) {
  SimBenchArgs args = sim_bench_default_args();
  StringSplitter *items = split_string(spec, ',', true);
  const int num_items = string_splitter_get_number_of_items(items);
  for (int item_idx = 0; item_idx < num_items; item_idx++) {
    const char *item = string_splitter_get_item(items, item_idx);
    if (has_prefix("plies=", item)) {
      args.plies = bench_spec_int("simbench", item, "plies");
    } else if (has_prefix("threads=", item)) {
      args.threads = bench_spec_int("simbench", item, "threads");
    } else if (has_prefix("tlim=", item)) {
      args.tlim = item + strlen("tlim=");
    } else if (has_prefix("mi=", item)) {
      args.mi = item + strlen("mi=");
    } else if (has_prefix("wmp=", item)) {
      args.wmp = item + strlen("wmp=");
    } else if (has_prefix("rit=", item)) {
      args.rit = item + strlen("rit=");
    } else if (has_prefix("wit=", item)) {
      args.wit = item + strlen("wit=");
    } else {
      log_fatal("simbench spec takes plies, threads, tlim, mi, wmp, rit and "
                "wit, got '%s'",
                item);
    }
  }
  // The strings in args point into items, so it outlives the run.
  run_sim_benchmark(&args);
  string_splitter_destroy(items);
}

// Play chooser benchmark: autoplays games under a game clock, with each
// turn's move pinned to top static equity as in simbench, and reports how
// much search work the play chooser's sims, PEG solves and endgame solves
// complete.
//
// Run as pcbench for the defaults, or pcbench:<key>=<value>,... to change
// them:
//   games     games to play                  (default 4)
//   clock_ms  each player's clock, in ms     (default 5000)
//   threads   worker threads                 (default detected hardware
//             concurrency)
//   wit       -wit                           (default true)
// For example: pcbench:games=2,threads=16,wit=false
typedef struct PlayChooserBenchArgs {
  int games;
  int clock_ms;
  int threads;
  const char *wit;
} PlayChooserBenchArgs;

static PlayChooserBenchArgs play_chooser_bench_default_args(void) {
  return (PlayChooserBenchArgs){
      .games = 4, .clock_ms = 5000, .threads = get_num_cores(), .wit = "true"};
}

static void run_play_chooser_benchmark(const PlayChooserBenchArgs *args) {
  const int games = args->games;
  const int clock_ms = args->clock_ms;
  const int threads = args->threads;
  const char *wit = args->wit;

  char settings[512];
  (void)snprintf(
      settings, sizeof(settings),
      "set -lex CSW24 -wmp true -rit true -ritmmap true -wit %s -s1 equity "
      "-s2 equity -r1 best -r2 best -numplays 1 -threads %d "
      "-pfrequency 0 -hr false -savesettings false -autosavegcg false "
      "-fgrequired false",
      wit, threads);
  Config *config = config_create_or_die(settings);
  char command[256];
  (void)snprintf(command, sizeof(command),
                 "autoplay games %d -pc1 %d -pc2 %d -mtmode igp -gp false "
                 "-otpenalty 0 -otperiod 1 -seed 24301",
                 games, clock_ms, clock_ms);

  // Run every analysis but pin the played move to top static equity, as
  // simbench does. Every binary therefore sees the same positions; the
  // comparison is how much search work it completes under the same clocks.
  autoplay_set_bench_static_move(true);
  play_chooser_benchmark_reset();
  load_and_exec_config_or_die(config, command);
  PlayChooserBenchmarkStats stats;
  play_chooser_benchmark_get(&stats);
  const size_t peg_event_count =
      play_chooser_benchmark_get_peg_candidate_events(NULL, 0);
  PlayChooserPegCandidateEvent *peg_events = NULL;
  size_t copied_peg_event_count = 0;
  if (peg_event_count > 0) {
    peg_events = malloc_or_die(peg_event_count * sizeof(*peg_events));
    copied_peg_event_count = play_chooser_benchmark_get_peg_candidate_events(
        peg_events, peg_event_count);
  }
  play_chooser_benchmark_stop();
  autoplay_set_bench_static_move(false);

  printf("PCBENCH games=%d clock_ms=%d static=%llu fallbacks=%llu "
         "sim_calls=%llu sim_iters=%llu sim_nodes=%llu peg_calls=%llu "
         "peg_candidate_completions=%llu peg_event_drops=%llu "
         "peg_stages=%llu peg_candidates=%llu peg_scenarios=%llu "
         "peg_partials=%llu eg_calls=%llu eg_nodes=%llu eg_depth=%llu\n",
         games, clock_ms, (unsigned long long)stats.static_moves,
         (unsigned long long)stats.fallback_moves,
         (unsigned long long)stats.sim_calls,
         (unsigned long long)stats.sim_iterations,
         (unsigned long long)stats.sim_nodes,
         (unsigned long long)stats.peg_calls,
         (unsigned long long)stats.peg_candidate_completions,
         (unsigned long long)stats.peg_candidate_events_dropped,
         (unsigned long long)stats.peg_completed_stages,
         (unsigned long long)stats.peg_final_candidates,
         (unsigned long long)stats.peg_final_scenarios,
         (unsigned long long)stats.peg_partial_calls,
         (unsigned long long)stats.endgame_calls,
         (unsigned long long)stats.endgame_nodes,
         (unsigned long long)stats.endgame_depth);

  for (size_t i = 0; i < copied_peg_event_count; i++) {
    const PlayChooserPegCandidateEvent *event = &peg_events[i];
    printf("PCPEGCAND call=%llu stage=%d rank=%d scenarios=%d "
           "elapsed_ms=%.3f\n",
           (unsigned long long)event->call_index, event->stage_index,
           event->candidate_rank, event->scenarios_completed,
           (double)event->elapsed_ns / 1.0e6);
  }
  free(peg_events);

  config_destroy(config);
}

void test_play_chooser_benchmark(void) {
  const PlayChooserBenchArgs args = play_chooser_bench_default_args();
  run_play_chooser_benchmark(&args);
}

// "<key>=<value>[,<key>=<value>...]", with the keys listed above
// PlayChooserBenchArgs.
void play_chooser_benchmark_run_spec(const char *spec) {
  PlayChooserBenchArgs args = play_chooser_bench_default_args();
  StringSplitter *items = split_string(spec, ',', true);
  const int num_items = string_splitter_get_number_of_items(items);
  for (int item_idx = 0; item_idx < num_items; item_idx++) {
    const char *item = string_splitter_get_item(items, item_idx);
    if (has_prefix("games=", item)) {
      args.games = bench_spec_int("pcbench", item, "games");
    } else if (has_prefix("clock_ms=", item)) {
      args.clock_ms = bench_spec_int("pcbench", item, "clock_ms");
    } else if (has_prefix("threads=", item)) {
      args.threads = bench_spec_int("pcbench", item, "threads");
    } else if (has_prefix("wit=", item)) {
      args.wit = item + strlen("wit=");
    } else {
      log_fatal("pcbench spec takes games, clock_ms, threads and wit, got '%s'",
                item);
    }
  }
  // The strings in args point into items, so it outlives the run.
  run_play_chooser_benchmark(&args);
  string_splitter_destroy(items);
}
