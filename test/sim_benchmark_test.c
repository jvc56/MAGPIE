// Sim benchmark: plays one full game with 5-second sim per turn.
// Uses the autoplay infrastructure with sim-based move selection.
// Expected runtime: ~2 minutes.
//
// Usage: ./bin/magpie_test simbench
//
// To compare RIT vs no-RIT:
//   time ./bin/magpie_test simbench    (with -rit true in settings)
//   time ./bin/magpie_test simbench    (with -rit false in settings)

#include "../src/impl/config.h"
#include "test_util.h"
#include <stdio.h>
#include <time.h>

#include "sim_benchmark_test.h"

void test_sim_benchmark(void) {
  struct timespec start, end;
  clock_gettime(CLOCK_MONOTONIC, &start);

  Config *config = config_create_or_die(
      "set -lex NWL23 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 15 -plies 2 -threads 10 -tlim 5 -seed 42 "
      "-sr tt -scond none");
  load_and_exec_config_or_die(config, "autoplay games 1");

  clock_gettime(CLOCK_MONOTONIC, &end);
  double elapsed = (double)(end.tv_sec - start.tv_sec) +
                   (double)(end.tv_nsec - start.tv_nsec) / 1e9;
  printf("Sim benchmark completed in %.1f seconds\n", elapsed);

  config_destroy(config);
}
