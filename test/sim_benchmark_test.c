// Sim benchmark: plays one full game with a timed sim per turn via
// autoplay and reports total sim iterations and iters/sec. The game
// trajectory is fixed via autoplay_set_bench_static_move so different
// RIT/BAI/cache variants traverse identical positions and the numbers
// are directly comparable.
//
// Usage: ./bin/magpie_test simbench
// Env vars:
//   SIMBENCH_PLIES  sim depth (default 2)
//   SIMBENCH_MI     -minplayiterations (default 100000)
//   SIMBENCH_RIT    "true" / "false" — toggles the RIT file (default true)
//   SIMBENCH_TLIM   seconds per turn (default 2)

#include "sim_benchmark_test.h"

#include "../src/impl/autoplay.h"
#include "../src/impl/config.h"
#include "../src/impl/play_chooser.h"
#include "test_util.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void test_sim_benchmark(void) {
  struct timespec start;
  struct timespec end;
  clock_gettime(CLOCK_MONOTONIC, &start); // NOLINT(misc-include-cleaner)

  autoplay_reset_total_sim_iterations();
  // Fix the game trajectory by playing the top-equity static move at every
  // turn, regardless of what the sim picks. The sim still runs fully, so
  // per-turn iteration throughput is measured, but different RIT/BAI
  // variants now traverse identical positions.
  autoplay_set_bench_static_move(true);

  const char *plies_env = getenv("SIMBENCH_PLIES");
  const int plies = (plies_env != NULL) ? (int)strtol(plies_env, NULL, 10) : 2;
  const char *mi_env = getenv("SIMBENCH_MI");
  const char *mi = (mi_env != NULL) ? mi_env : "100000";
  const char *rit_env = getenv("SIMBENCH_RIT");
  const char *rit = (rit_env != NULL) ? rit_env : "true";
  const char *wmp_env = getenv("SIMBENCH_WMP");
  const char *wmp = (wmp_env != NULL) ? wmp_env : "true";
  const char *tlim_env = getenv("SIMBENCH_TLIM");
  const char *tlim = (tlim_env != NULL) ? tlim_env : "2";
  char cmd[256];
  (void)snprintf(cmd, sizeof(cmd),
                 "set -lex CSW24 -wmp %s -rit %s -s1 equity -s2 equity "
                 "-r1 all -r2 all -numplays 15 -plies %d -threads 10 "
                 "-tlim %s -seed 42 -sr tt -minplayiterations %s",
                 wmp, rit, plies, tlim, mi);
  Config *config = config_create_or_die(cmd);
  load_and_exec_config_or_die(config, "autoplay games 1");

  clock_gettime(CLOCK_MONOTONIC, &end);
  double elapsed = (double)(end.tv_sec - start.tv_sec) +
                   (double)(end.tv_nsec - start.tv_nsec) / 1e9;
  const uint64_t iters = autoplay_get_total_sim_iterations();
  printf("plies=%d: %.1fs, iters=%llu (%.0f iters/sec)\n", plies, elapsed,
         (unsigned long long)iters, (double)iters / elapsed);

  config_destroy(config);
}

void test_play_chooser_benchmark(void) {
  const char *games_env = getenv("PCBENCH_GAMES");
  const int games = games_env != NULL ? (int)strtol(games_env, NULL, 10) : 4;
  const char *clock_env = getenv("PCBENCH_CLOCK_MS");
  const int clock_ms =
      clock_env != NULL ? (int)strtol(clock_env, NULL, 10) : 5000;

  Config *config = config_create_or_die(
      "set -lex CSW24 -wmp true -rit true -ritmmap true -s1 equity "
      "-s2 equity -r1 best -r2 best -numplays 1 -threads 4 "
      "-pfrequency 0 -hr false -savesettings false -autosavegcg false "
      "-fgrequired false");
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
  PlayChooserPegCandidateEvent *peg_events =
      peg_event_count > 0 ? malloc_or_die(peg_event_count * sizeof(*peg_events))
                          : NULL;
  const size_t copied_peg_event_count =
      play_chooser_benchmark_get_peg_candidate_events(peg_events,
                                                      peg_event_count);
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
