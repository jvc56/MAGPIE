// BAI scheduling measurement harness.
//
// Plays autoplay games under -mtmode igp, so each sim gets every thread, with
// the game trajectory pinned to the top static move (as simbench does) so that
// every configuration sims identical positions. Prints one row per
// configuration of the matrix below.
//
// Throughput (samples/s over the whole autoplay command) is reported by any
// build. The scheduling breakdown -- idle, barrier and tail time, lock waits,
// fold cost, sample cost spread, staleness and overshoot -- needs the
// instrumented build:
//
//   make magpie_test BUILD=bai_stats && ./bin/magpie_test baisched
//
// The matrix is 1, 2, 4, 8 and <cores> threads per sim by -iterations 300,
// 1000, 3000 and 10000, each one CSW21 game simming 15 plays at 2 plies with
// -minplayiterations 10, -sr tt and -scond 99.

#include "bai_sched_test.h"

#include "../src/compat/ctime.h"
#include "../src/compat/memory_info.h"
#include "../src/ent/bai_sched_stats.h"
#include "../src/impl/autoplay.h"
#include "../src/impl/config.h"
#include "../src/util/io_util.h"
#include "test_util.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

enum {
  BAISCHED_MIN_SAMPLES_FOR_TAIL_QUANTILE = 1000,
};

static int compare_doubles(const void *a, const void *b) {
  const double value_a = *(const double *)a;
  const double value_b = *(const double *)b;
  return (value_a > value_b) - (value_a < value_b);
}

static double baisched_pct(const int64_t part, const double whole) {
  return whole > 0 ? 100.0 * (double)part / whole : 0.0;
}

static void baisched_print_header(void) {
  printf("%4s %7s %5s %9s %8s %9s | %6s %6s %6s %6s %6s %6s %6s %6s %6s | "
         "%6s %6s %6s %6s | %7s %7s %6s %7s %7s %5s\n",
         "thr", "iters", "sims", "samples", "wall_s", "samp/s", "busy%",
         "idle%", "barr%", "start%", "tail%", "end%", "lock%", "fold%",
         "other%", "q99md", "q999md", "q999mx", "qmaxmx", "ovsh/sm", "spec/sm",
         "hit%", "staleav", "stalemx", "R");
}

static void baisched_print_row(const int threads, const int iters,
                               const uint64_t samples, const double wall_s) {
  const int num_sims = bai_sched_stats_get_count();
  printf("%4d %7d %5d %9llu %8.2f %9.0f |", threads, iters, num_sims,
         (unsigned long long)samples, wall_s,
         wall_s > 0 ? (double)samples / wall_s : 0.0);
  if (num_sims == 0) {
    printf(" (build with BUILD=bai_stats for the scheduling breakdown)\n");
    return;
  }
  double capacity_ns = 0;
  int64_t busy_ns = 0;
  int64_t idle_ns = 0;
  int64_t barrier_ns = 0;
  int64_t startup_ns = 0;
  int64_t tail_ns = 0;
  int64_t end_ns = 0;
  int64_t lock_ns = 0;
  int64_t fold_ns = 0;
  uint64_t overshoot = 0;
  uint64_t speculations = 0;
  uint64_t speculation_hits = 0;
  uint64_t stale_sum = 0;
  uint64_t stale_count = 0;
  uint64_t stale_max = 0;
  int round_size = 0;
  double *q99 = malloc_or_die(sizeof(double) * (size_t)num_sims);
  double *q999 = malloc_or_die(sizeof(double) * (size_t)num_sims);
  double *qmax = malloc_or_die(sizeof(double) * (size_t)num_sims);
  int num_tail_sims = 0;
  for (int sim_idx = 0; sim_idx < num_sims; sim_idx++) {
    BAISchedSimStats sim_stats;
    bai_sched_stats_get(sim_idx, &sim_stats);
    capacity_ns += (double)sim_stats.wall_ns * (double)sim_stats.num_threads;
    busy_ns += sim_stats.busy_ns;
    idle_ns += sim_stats.idle_ns;
    barrier_ns +=
        sim_stats.initial_barrier_ns + sim_stats.avoid_prune_barrier_ns;
    startup_ns += sim_stats.startup_ns;
    tail_ns += sim_stats.tail_ns;
    end_ns += sim_stats.end_ns;
    lock_ns += sim_stats.lock_wait_ns;
    fold_ns += sim_stats.fold_ns + sim_stats.schedule_ns;
    overshoot += sim_stats.overshoot_samples;
    speculations += sim_stats.speculations;
    speculation_hits += sim_stats.speculation_hits;
    stale_sum += sim_stats.stale_sum;
    stale_count += sim_stats.stale_count;
    if (sim_stats.stale_max > stale_max) {
      stale_max = sim_stats.stale_max;
    }
    if (sim_stats.round_size > round_size) {
      round_size = sim_stats.round_size;
    }
    const uint64_t num_samples =
        sim_stats.initial_samples + sim_stats.bai_samples;
    if (sim_stats.sample_mean_ns > 0 &&
        num_samples >= BAISCHED_MIN_SAMPLES_FOR_TAIL_QUANTILE) {
      const double mean = (double)sim_stats.sample_mean_ns;
      q99[num_tail_sims] = (double)sim_stats.sample_p99_ns / mean;
      q999[num_tail_sims] = (double)sim_stats.sample_p999_ns / mean;
      qmax[num_tail_sims] = (double)sim_stats.sample_max_ns / mean;
      num_tail_sims++;
    }
  }
  double q99_median = 0;
  double q999_median = 0;
  double q999_max = 0;
  double qmax_max = 0;
  if (num_tail_sims > 0) {
    qsort(q99, (size_t)num_tail_sims, sizeof(double), compare_doubles);
    qsort(q999, (size_t)num_tail_sims, sizeof(double), compare_doubles);
    qsort(qmax, (size_t)num_tail_sims, sizeof(double), compare_doubles);
    q99_median = q99[num_tail_sims / 2];
    q999_median = q999[num_tail_sims / 2];
    q999_max = q999[num_tail_sims - 1];
    qmax_max = qmax[num_tail_sims - 1];
  }
  free(q99);
  free(q999);
  free(qmax);
  const int64_t accounted = busy_ns + idle_ns + barrier_ns + startup_ns +
                            tail_ns + end_ns + lock_ns + fold_ns;
  printf(" %6.2f %6.2f %6.2f %6.2f %6.2f %6.2f %6.2f %6.2f %6.2f | %6.2f %6.2f "
         "%6.2f %6.1f | %7.1f %7.1f %6.1f %7.0f %7llu %5d\n",
         baisched_pct(busy_ns, capacity_ns), baisched_pct(idle_ns, capacity_ns),
         baisched_pct(barrier_ns, capacity_ns),
         baisched_pct(startup_ns, capacity_ns),
         baisched_pct(tail_ns, capacity_ns), baisched_pct(end_ns, capacity_ns),
         baisched_pct(lock_ns, capacity_ns), baisched_pct(fold_ns, capacity_ns),
         100.0 - baisched_pct(accounted, capacity_ns), q99_median, q999_median,
         q999_max, qmax_max, (double)overshoot / (double)num_sims,
         (double)speculations / (double)num_sims,
         speculations > 0
             ? 100.0 * (double)speculation_hits / (double)speculations
             : 0.0,
         stale_count > 0 ? (double)stale_sum / (double)stale_count : 0.0,
         (unsigned long long)stale_max, round_size);
}

void test_bai_sched(void) {
  const int thread_counts[] = {1, 2, 4, 8, get_num_cores()};
  const int num_thread_counts =
      (int)(sizeof(thread_counts) / sizeof(thread_counts[0]));
  const int iteration_counts[] = {300, 1000, 3000, 10000};
  const int num_iteration_counts =
      (int)(sizeof(iteration_counts) / sizeof(iteration_counts[0]));

  printf("baisched: lex=CSW21 plies=2 plays=15 minplay=10 sr=tt scond=99 "
         "games=1\n");
  baisched_print_header();

  autoplay_set_bench_static_move(true);
  for (int iter_idx = 0; iter_idx < num_iteration_counts; iter_idx++) {
    for (int thread_idx = 0; thread_idx < num_thread_counts; thread_idx++) {
      char settings[512];
      (void)snprintf(
          settings, sizeof(settings),
          "set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
          "-numplays 15 -plies 2 -threads %d -iterations %d "
          "-minplayiterations 10 -scond 99 -sr tt -tlim 0 -seed 42 "
          "-mtmode igp -savesettings false -autosavegcg false",
          thread_counts[thread_idx], iteration_counts[iter_idx]);
      Config *config = config_create_or_die(settings);
      bai_sched_stats_reset();
      autoplay_reset_total_sim_iterations();
      Timer timer;
      ctimer_start(&timer);
      load_and_exec_config_or_die(config, "autoplay games 1 -gp false");
      ctimer_stop(&timer);
      baisched_print_row(thread_counts[thread_idx], iteration_counts[iter_idx],
                         autoplay_get_total_sim_iterations(),
                         ctimer_elapsed_seconds(&timer));
      (void)fflush(stdout);
      config_destroy(config);
    }
  }
  autoplay_set_bench_static_move(false);
  bai_sched_stats_reset();
}
