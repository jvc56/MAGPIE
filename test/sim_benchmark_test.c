// Sim benchmark: plays one full game with timed sim per turn via autoplay.
// Compares iteration throughput with and without RIT.
//
// Usage: ./bin/magpie_test simbench

#include "sim_benchmark_test.h"

#include "../src/def/board_defs.h"
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
  const char *mi_env = getenv("SIMBENCH_MI");
  const char *mi = (mi_env != NULL) ? mi_env : "100000";
  char cmd[256];
  snprintf(cmd, sizeof(cmd),
           "set -lex CSW24 -wmp true -rit true -s1 equity -s2 equity "
           "-r1 all -r2 all -numplays 15 -plies %d -threads 10 -tlim 2 "
           "-seed 42 -sr tt -minplayiterations %s",
           plies, mi);
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

#ifdef ANCHOR_CACHE_INSTRUMENT
  const uint64_t ac_checks = anchor_cache_stat_checks();
  const uint64_t ac_hits = anchor_cache_stat_hits();
  const uint64_t ac_skips = anchor_cache_stat_skips();
  const uint64_t ac_stores = anchor_cache_stat_stores();
  if (ac_checks > 0) {
    printf("Anchor cache: checks=%llu hits=%llu (%.2f%%) skips=%llu (%.2f%%) "
           "stores=%llu\n",
           (unsigned long long)ac_checks, (unsigned long long)ac_hits,
           100.0 * (double)ac_hits / (double)ac_checks,
           (unsigned long long)ac_skips,
           100.0 * (double)ac_skips / (double)ac_checks,
           (unsigned long long)ac_stores);
    printf("  len | checks    | hit%%   | skip%%  |\n");
    for (int l = 2; l <= BOARD_DIM; l++) {
      const uint64_t lc = anchor_cache_stat_checks_for_length(l);
      if (lc == 0) {
        continue;
      }
      const uint64_t lh = anchor_cache_stat_hits_for_length(l);
      const uint64_t ls = anchor_cache_stat_skips_for_length(l);
      printf("  %2d  | %9llu | %5.2f%% | %5.2f%% |\n", l,
             (unsigned long long)lc,
             100.0 * (double)lh / (double)lc,
             100.0 * (double)ls / (double)lc);
    }
  }
#endif

#ifdef WMP_ANCHOR_INSTRUMENT
  uint64_t total_time_ns = 0;
  uint64_t total_calls = 0;
  for (int l = 0; l <= BOARD_DIM; l++) {
    total_time_ns += wmp_anchor_stat_time_ns(l);
    total_calls += wmp_anchor_stat_calls(l);
  }
  printf("wordmap_gen per-length (total calls=%llu, total time=%.3fs):\n",
         (unsigned long long)total_calls, (double)total_time_ns / 1e9);
  printf("  len | calls     | full%%  | pt%% | skip/iter | words/iter | "
         "recs/iter | time_ms | time%%\n");
  for (int l = 2; l <= BOARD_DIM; l++) {
    const uint64_t c = wmp_anchor_stat_calls(l);
    if (c == 0) {
      continue;
    }
    const uint64_t full = wmp_anchor_stat_fully_searched(l);
    const uint64_t si = wmp_anchor_stat_subrack_iters(l);
    const uint64_t ss = wmp_anchor_stat_subrack_skipped(l);
    const uint64_t wp = wmp_anchor_stat_words_produced(l);
    const uint64_t rc = wmp_anchor_stat_record_calls(l);
    const uint64_t pt = wmp_anchor_stat_playthrough_calls(l);
    const uint64_t tn = wmp_anchor_stat_time_ns(l);
    printf("  %2d  | %9llu | %5.1f%% | %4.1f%% | %9.3f | %10.3f | %9.3f | "
           "%7.2f | %5.2f%%\n",
           l, (unsigned long long)c, 100.0 * (double)full / (double)c,
           100.0 * (double)pt / (double)c,
           (double)ss / (double)c, (double)wp / (double)c,
           (double)rc / (double)c, (double)tn / 1e6,
           100.0 * (double)tn / (double)total_time_ns);
  }
#endif

  config_destroy(config);
}
