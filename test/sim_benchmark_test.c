// Sim benchmark: plays one full game with timed sim per turn via autoplay.
// Compares iteration throughput with and without RIT.
//
// Usage: ./bin/magpie_test simbench

#include "sim_benchmark_test.h"

#include "../src/impl/config.h"
#include "test_util.h"
#include <stdio.h>
#include <time.h>

void test_sim_benchmark(void) {
  struct timespec start, end;
  clock_gettime(CLOCK_MONOTONIC, &start);

  Config *config = config_create_or_die(
      "set -lex CSW24 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays 15 -plies 2 -threads 10 -tlim 2 -seed 42 -sr tt");
  load_and_exec_config_or_die(config, "autoplay games 1");

  clock_gettime(CLOCK_MONOTONIC, &end);
  double elapsed = (double)(end.tv_sec - start.tv_sec) +
                   (double)(end.tv_nsec - start.tv_nsec) / 1e9;
  printf("Sim benchmark completed in %.1f seconds\n", elapsed);

  config_destroy(config);
}
