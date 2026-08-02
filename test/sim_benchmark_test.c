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
//   SIMBENCH_THREADS worker threads (default detected hardware concurrency)
//   SIMBENCH_WIT    "true" / "false" — toggles the WIT file (default false)
//   SIMBENCH_KLV    optional KLV/KLV3 name for both players
//   SIMBENCH_SPREAD_FORECAST optional terminal-spread forecast name

#include "sim_benchmark_test.h"

#include "../src/compat/memory_info.h"
#include "../src/impl/autoplay.h"
#include "../src/impl/config.h"
#include "../src/impl/play_chooser.h"
#include "../src/util/io_util.h"
#include "test_util.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static bool benchmark_env_true(const char *name) {
  const char *value = getenv(name);
  return value != NULL &&
         (strcmp(value, "1") == 0 || strcmp(value, "true") == 0 ||
          strcmp(value, "yes") == 0);
}

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
  const char *rit_mmap_env = getenv("SIMBENCH_RIT_MMAP");
  const char *rit_mmap = (rit_mmap_env != NULL) ? rit_mmap_env : "true";
  const char *wmp_env = getenv("SIMBENCH_WMP");
  const char *wmp = (wmp_env != NULL) ? wmp_env : "true";
  const char *wit_env = getenv("SIMBENCH_WIT");
  const char *wit = (wit_env != NULL) ? wit_env : "false";
  const char *tlim_env = getenv("SIMBENCH_TLIM");
  const char *tlim = (tlim_env != NULL) ? tlim_env : "2";
  const char *threads_env = getenv("SIMBENCH_THREADS");
  const int threads = threads_env != NULL ? (int)strtol(threads_env, NULL, 10)
                                          : get_num_cores();
  const char *klv = getenv("SIMBENCH_KLV");
  char klv_args[160] = "";
  if (klv != NULL) {
    (void)snprintf(klv_args, sizeof(klv_args), "-k1 %s -k2 %s ", klv, klv);
  }
  const char *spread_forecast = getenv("SIMBENCH_SPREAD_FORECAST");
  char spread_forecast_args[160] = "";
  if (spread_forecast != NULL) {
    (void)snprintf(spread_forecast_args, sizeof(spread_forecast_args),
                   "-spreadforecast %s ", spread_forecast);
  }
  char cmd[672];
  (void)snprintf(cmd, sizeof(cmd),
                 "set -lex CSW24 %s%s-wmp %s -rit %s -ritmmap %s -wit %s "
                 "-s1 equity "
                 "-s2 equity "
                 "-r1 all -r2 all -numplays 15 -plies %d -threads %d "
                 "-tlim %s -seed 42 -sr tt -minplayiterations %s",
                 klv_args, spread_forecast_args, wmp, rit, rit_mmap, wit, plies,
                 threads, tlim, mi);
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
  const char *threads_env = getenv("PCBENCH_THREADS");
  const int threads = threads_env != NULL ? (int)strtol(threads_env, NULL, 10)
                                          : get_num_cores();
  const char *wit_env = getenv("PCBENCH_WIT");
  const char *wit = wit_env != NULL ? wit_env : "false";
  const char *seed_env = getenv("PCBENCH_SEED");
  const int seed = seed_env != NULL ? (int)strtol(seed_env, NULL, 10) : 24301;
  const bool game_pairs = benchmark_env_true("PCBENCH_GAME_PAIR");
  const bool play_analyzed_moves =
      game_pairs || benchmark_env_true("PCBENCH_PLAY_MOVES");
  const bool detail_events = benchmark_env_true("PCBENCH_DETAIL_EVENTS");

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
                 "autoplay games %d -pc1 %d -pc2 %d -mtmode igp -gp %s "
                 "-otpenalty %d -otperiod 1 -seed %d",
                 games, clock_ms, clock_ms, game_pairs ? "true" : "false",
                 game_pairs ? 1 : 0, seed);

  // The ordinary benchmark pins the played move to top static equity, as
  // simbench does, so binaries see the same positions. Complete-trajectory
  // collection may opt into the analyzed move to sample the state
  // distribution induced by PlayChooser itself.
  autoplay_set_bench_static_move(!play_analyzed_moves);
  play_chooser_benchmark_reset();
  load_and_exec_config_or_die(config, command);
  PlayChooserBenchmarkStats stats;
  play_chooser_benchmark_get(&stats);
  const size_t sim_event_count =
      play_chooser_benchmark_get_sim_candidate_events(NULL, 0);
  PlayChooserSimCandidateEvent *sim_events = NULL;
  size_t copied_sim_event_count = 0;
  if (detail_events && sim_event_count > 0) {
    sim_events = malloc_or_die(sim_event_count * sizeof(*sim_events));
    copied_sim_event_count = play_chooser_benchmark_get_sim_candidate_events(
        sim_events, sim_event_count);
  }
  const size_t peg_event_count =
      play_chooser_benchmark_get_peg_candidate_events(NULL, 0);
  PlayChooserPegCandidateEvent *peg_events = NULL;
  size_t copied_peg_event_count = 0;
  if (peg_event_count > 0) {
    peg_events = malloc_or_die(peg_event_count * sizeof(*peg_events));
    copied_peg_event_count = play_chooser_benchmark_get_peg_candidate_events(
        peg_events, peg_event_count);
  }
  const size_t endgame_event_count =
      play_chooser_benchmark_get_endgame_events(NULL, 0);
  PlayChooserEndgameEvent *endgame_events = NULL;
  size_t copied_endgame_event_count = 0;
  if (detail_events && endgame_event_count > 0) {
    endgame_events =
        malloc_or_die(endgame_event_count * sizeof(*endgame_events));
    copied_endgame_event_count = play_chooser_benchmark_get_endgame_events(
        endgame_events, endgame_event_count);
  }
  play_chooser_benchmark_stop();
  autoplay_set_bench_static_move(false);

  printf("PCBENCH games=%d clock_ms=%d played_analysis=%d static=%llu "
         "fallbacks=%llu "
         "sim_calls=%llu sim_iters=%llu sim_nodes=%llu sim_event_drops=%llu "
         "peg_calls=%llu "
         "peg_candidate_completions=%llu peg_event_drops=%llu "
         "peg_stages=%llu peg_candidates=%llu peg_scenarios=%llu "
         "peg_endgame_nodes=%llu peg_partials=%llu peg_tm_admissions=%llu "
         "peg_tm_false_starts=%llu eg_calls=%llu eg_nodes=%llu "
         "eg_depth=%llu eg_event_drops=%llu\n",
         games, clock_ms, play_analyzed_moves ? 1 : 0,
         (unsigned long long)stats.static_moves,
         (unsigned long long)stats.fallback_moves,
         (unsigned long long)stats.sim_calls,
         (unsigned long long)stats.sim_iterations,
         (unsigned long long)stats.sim_nodes,
         (unsigned long long)stats.sim_candidate_events_dropped,
         (unsigned long long)stats.peg_calls,
         (unsigned long long)stats.peg_candidate_completions,
         (unsigned long long)stats.peg_candidate_events_dropped,
         (unsigned long long)stats.peg_completed_stages,
         (unsigned long long)stats.peg_final_candidates,
         (unsigned long long)stats.peg_final_scenarios,
         (unsigned long long)stats.peg_endgame_nodes,
         (unsigned long long)stats.peg_partial_calls,
         (unsigned long long)stats.peg_time_manager_admissions,
         (unsigned long long)stats.peg_time_manager_false_starts,
         (unsigned long long)stats.endgame_calls,
         (unsigned long long)stats.endgame_nodes,
         (unsigned long long)stats.endgame_depth,
         (unsigned long long)stats.endgame_events_dropped);

  for (size_t i = 0; i < copied_sim_event_count; i++) {
    const PlayChooserSimCandidateEvent *event = &sim_events[i];
    printf("PCSIMCAND call=%llu rank=%d iterations=%llu item=%llu "
           "selected=%d win=%.9f utility=%.9f equity=%+.6f\n",
           (unsigned long long)event->call_index, event->candidate_rank,
           (unsigned long long)event->iterations,
           (unsigned long long)event->item_id, event->selected ? 1 : 0,
           event->win_pct, event->utility, event->equity);
  }

  for (size_t i = 0; i < copied_peg_event_count; i++) {
    const PlayChooserPegCandidateEvent *event = &peg_events[i];
    printf("PCPEGCAND call=%llu stage=%d rank=%d scenarios=%d "
           "endgame_nodes=%llu item=%llu win=%.6f spread=%+.3f "
           "elapsed_ms=%.3f cpu_ms=%.3f\n",
           (unsigned long long)event->call_index, event->stage_index,
           event->candidate_rank, event->scenarios_completed,
           (unsigned long long)event->endgame_nodes,
           (unsigned long long)event->item_id, event->win_pct,
           event->mean_spread, (double)event->elapsed_ns / 1.0e6,
           (double)event->cpu_ns / 1.0e6);
  }
  for (size_t i = 0; i < copied_endgame_event_count; i++) {
    const PlayChooserEndgameEvent *event = &endgame_events[i];
    printf("PCEG call=%llu depth=%d nodes=%llu completed=%d windowed=%d "
           "elapsed_ms=%.3f\n",
           (unsigned long long)event->call_index, event->depth,
           (unsigned long long)event->nodes, event->completed ? 1 : 0,
           event->windowed ? 1 : 0, (double)event->elapsed_ns / 1.0e6);
  }
  free(sim_events);
  free(peg_events);
  free(endgame_events);

  config_destroy(config);
}
