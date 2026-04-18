// Sim benchmark: plays one full game with timed sim per turn via autoplay.
// Compares iteration throughput with and without RIT.
//
// Usage: ./bin/magpie_test simbench

#include "sim_benchmark_test.h"

#include "../src/impl/autoplay.h"
#include "../src/impl/config.h"
#include "../src/impl/move_gen.h"
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
  const int plies = (plies_env != NULL) ? atoi(plies_env) : 2;
  char cmd[256];
  snprintf(cmd, sizeof(cmd),
           "set -lex CSW24 -wmp true -rit true -s1 equity -s2 equity "
           "-r1 all -r2 all -numplays 15 -plies %d -threads 10 -tlim 2 "
           "-seed 42 -sr tt -minplayiterations 100000",
           plies);
  Config *config = config_create_or_die(cmd);
  load_and_exec_config_or_die(config, "autoplay games 1");

  clock_gettime(CLOCK_MONOTONIC, &end);
  double elapsed = (double)(end.tv_sec - start.tv_sec) +
                   (double)(end.tv_nsec - start.tv_nsec) / 1e9;
  const uint64_t iters = autoplay_get_total_sim_iterations();
  printf("plies=%d: %.1fs, iters=%llu (%.0f iters/sec)",
         plies, elapsed, (unsigned long long)iters,
         (double)iters / elapsed);
#ifdef RIT_CACHE_INSTRUMENT
  const uint64_t h = rit_cache_stat_hits();
  const uint64_t m = rit_cache_stat_misses();
  if (h + m > 0) {
    printf(", hit_rate=%.2f%%",
           100.0 * (double)h / (double)(h + m));
  }
#endif
  printf("\n");

  config_destroy(config);
}
