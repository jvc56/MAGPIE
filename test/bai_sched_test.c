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
// Env vars (comma-separated lists sweep the matrix):
//   BAISCHED_THREADS  per-sim thread counts (default 1,2,4,8,<cores>)
//   BAISCHED_ITERS    -iterations sample budgets (default 300,1000,3000,10000)
//   BAISCHED_PLIES    sim plies (default 2)
//   BAISCHED_PLAYS    -numplays (default 15)
//   BAISCHED_MINPLAY  -minplayiterations (default 10)
//   BAISCHED_SR       sampling rule, tt or rr (default tt)
//   BAISCHED_SCOND    stop condition, a percentage or none (default 99)
//   BAISCHED_GAMES    games per configuration (default 1)
//   BAISCHED_LEX      lexicon (default CSW21)

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
  BAISCHED_MAX_LIST = 32,
  BAISCHED_MIN_SAMPLES_FOR_TAIL_QUANTILE = 1000,
};

static const char *baisched_env_or(const char *name, const char *fallback) {
  const char *value = getenv(name);
  return value != NULL && value[0] != '\0' ? value : fallback;
}

// Parses a comma-separated list of positive integers. Returns the count.
static int baisched_parse_list(const char *list, int *values) {
  int count = 0;
  const char *cursor = list;
  while (*cursor != '\0' && count < BAISCHED_MAX_LIST) {
    char *end = NULL;
    const long value = strtol(cursor, &end, 10);
    if (end == cursor) {
      break;
    }
    values[count++] = (int)value;
    cursor = (*end == ',') ? end + 1 : end;
  }
  return count;
}

static int compare_doubles(const void *a, const void *b) {
  const double value_a = *(const double *)a;
  const double value_b = *(const double *)b;
  return (value_a > value_b) - (value_a < value_b);
}

static double baisched_pct(const int64_t part, const double whole) {
  return whole > 0 ? 100.0 * (double)part / whole : 0.0;
}

static void baisched_print_header(void) {
  printf("%4s %7s %5s %9s %8s %9s | %6s %6s %6s %6s %6s %6s %6s | %6s %6s "
         "%6s %6s | %7s %7s %7s %5s\n",
         "thr", "iters", "sims", "samples", "wall_s", "samp/s", "busy%",
         "idle%", "barr%", "tail%", "lock%", "fold%", "other%", "q99md",
         "q999md", "q999mx", "qmaxmx", "ovsh/sm", "staleav", "stalemx", "R");
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
  int64_t tail_ns = 0;
  int64_t lock_ns = 0;
  int64_t fold_ns = 0;
  uint64_t overshoot = 0;
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
    tail_ns += sim_stats.tail_ns;
    lock_ns += sim_stats.lock_wait_ns;
    fold_ns += sim_stats.fold_ns + sim_stats.schedule_ns;
    overshoot += sim_stats.overshoot_samples;
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
  const int64_t accounted =
      busy_ns + idle_ns + barrier_ns + tail_ns + lock_ns + fold_ns;
  printf(" %6.2f %6.2f %6.2f %6.2f %6.2f %6.2f %6.2f | %6.2f %6.2f %6.2f "
         "%6.1f | %7.1f %7.0f %7llu %5d\n",
         baisched_pct(busy_ns, capacity_ns), baisched_pct(idle_ns, capacity_ns),
         baisched_pct(barrier_ns, capacity_ns),
         baisched_pct(tail_ns, capacity_ns), baisched_pct(lock_ns, capacity_ns),
         baisched_pct(fold_ns, capacity_ns),
         100.0 - baisched_pct(accounted, capacity_ns), q99_median, q999_median,
         q999_max, qmax_max, (double)overshoot / (double)num_sims,
         stale_count > 0 ? (double)stale_sum / (double)stale_count : 0.0,
         (unsigned long long)stale_max, round_size);
}

void test_bai_sched(void) {
  char default_threads[64];
  (void)snprintf(default_threads, sizeof(default_threads), "1,2,4,8,%d",
                 get_num_cores());
  int thread_counts[BAISCHED_MAX_LIST];
  int iteration_counts[BAISCHED_MAX_LIST];
  const int num_thread_counts = baisched_parse_list(
      baisched_env_or("BAISCHED_THREADS", default_threads), thread_counts);
  const int num_iteration_counts = baisched_parse_list(
      baisched_env_or("BAISCHED_ITERS", "300,1000,3000,10000"),
      iteration_counts);
  const char *plies = baisched_env_or("BAISCHED_PLIES", "2");
  const char *plays = baisched_env_or("BAISCHED_PLAYS", "15");
  const char *min_play = baisched_env_or("BAISCHED_MINPLAY", "10");
  const char *sampling_rule = baisched_env_or("BAISCHED_SR", "tt");
  const char *stop_cond = baisched_env_or("BAISCHED_SCOND", "99");
  const char *games = baisched_env_or("BAISCHED_GAMES", "1");
  const char *lexicon = baisched_env_or("BAISCHED_LEX", "CSW21");

  printf("baisched: lex=%s plies=%s plays=%s minplay=%s sr=%s scond=%s "
         "games=%s\n",
         lexicon, plies, plays, min_play, sampling_rule, stop_cond, games);
  baisched_print_header();

  autoplay_set_bench_static_move(true);
  for (int iter_idx = 0; iter_idx < num_iteration_counts; iter_idx++) {
    for (int thread_idx = 0; thread_idx < num_thread_counts; thread_idx++) {
      char settings[512];
      (void)snprintf(
          settings, sizeof(settings),
          "set -lex %s -wmp true -s1 equity -s2 equity -r1 all -r2 all "
          "-numplays %s -plies %s -threads %d -iterations %d "
          "-minplayiterations %s -scond %s -sr %s -tlim 0 -seed 42 "
          "-mtmode igp -savesettings false -autosavegcg false",
          lexicon, plays, plies, thread_counts[thread_idx],
          iteration_counts[iter_idx], min_play, stop_cond, sampling_rule);
      Config *config = config_create_or_die(settings);
      char command[64];
      (void)snprintf(command, sizeof(command), "autoplay games %s -gp false",
                     games);
      bai_sched_stats_reset();
      autoplay_reset_total_sim_iterations();
      Timer timer;
      ctimer_start(&timer);
      load_and_exec_config_or_die(config, command);
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
