// Sim benchmark: plays one full game with timed sim per turn via autoplay.
// Compares iteration throughput with and without RIT.
//
// Usage: ./bin/magpie_test simbench

#include "sim_benchmark_test.h"

#include "../src/impl/autoplay.h"
#include "../src/impl/config.h"
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
  char cmd[256];
  (void)snprintf(cmd, sizeof(cmd),
                 "set -lex CSW24 -wmp true -rit %s -s1 equity -s2 equity "
                 "-r1 all -r2 all -numplays 15 -plies %d -threads 10 -tlim 2 "
                 "-seed 42 -sr tt -minplayiterations %s",
                 rit, plies, mi);
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
