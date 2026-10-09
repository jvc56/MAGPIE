// Sim determinism sweep.
//
// Runs fixed-seed, fixed-budget sims (no clock) on a spread of positions,
// opening through late game, and fingerprints each result: per play the
// sample count and means, plus the best move, BAI status and iteration count.
// Every thread count must give the fingerprint the 1-thread run gives. The
// combined hash printed at the end identifies the exact results, so two
// builds that should behave identically can be compared by running this
// sweep under each.
//
// Env vars (comma-separated lists):
//   SIMDET_THREADS  thread counts (default 1,2,4,8,<cores>)
//   SIMDET_ITERS    -iterations budgets (default 500,3000)
//   SIMDET_SCOND    stop conditions (default 99,none)
//   SIMDET_SR       sampling rules (default tt,rr)
//   SIMDET_PLAYS    -numplays (default 15)
//   SIMDET_PLIES    sim plies (default 2)
//   SIMDET_MINPLAY  -minplayiterations (default 10)
//   SIMDET_POS      position indices to run (default all)
//   SIMDET_VERBOSE  1 prints every play's count and means for every run

#include "sim_det_sweep_test.h"

#include "../src/compat/memory_info.h"
#include "../src/ent/bai_result.h"
#include "../src/ent/sim_results.h"
#include "../src/ent/stats.h"
#include "../src/impl/config.h"
#include "../src/util/fnv.h"
#include "../src/util/io_util.h"
#include "test_constants.h"
#include "test_util.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  SIMDET_MAX_LIST = 16,
  SIMDET_MAX_TOKEN = 32,
};

static const char *simdet_env_or(const char *name, const char *fallback) {
  const char *value = getenv(name);
  return value != NULL && value[0] != '\0' ? value : fallback;
}

// Splits a comma-separated list into tokens. Returns the count.
static int simdet_split(const char *list,
                        char tokens[SIMDET_MAX_LIST][SIMDET_MAX_TOKEN]) {
  int count = 0;
  const char *cursor = list;
  while (*cursor != '\0' && count < SIMDET_MAX_LIST) {
    const char *comma = strchr(cursor, ',');
    size_t length = comma != NULL ? (size_t)(comma - cursor) : strlen(cursor);
    if (length >= SIMDET_MAX_TOKEN) {
      length = SIMDET_MAX_TOKEN - 1;
    }
    memcpy(tokens[count], cursor, length);
    tokens[count][length] = '\0';
    count++;
    if (comma == NULL) {
      break;
    }
    cursor = comma + 1;
  }
  return count;
}

// The exact means are bit-reproducible for a given sample multiset, so their
// raw bits are the fingerprint. (The Stat means are Welford means, whose last
// bits depend on merge order; rounding them to a grid is not a fix either,
// because a mean such as 17.1678125 sits exactly on a half-step of a 1e-6
// grid and the last bit then decides which way it rounds.)
static uint64_t simdet_bits(const double value) {
  uint64_t bits;
  memcpy(&bits, &value, sizeof(bits));
  return bits;
}

static uint64_t simdet_run(const char *settings, const char *cgp,
                           const bool verbose) {
  Config *config = config_create_or_die(settings);
  char *cgp_command = get_formatted_string("cgp %s", cgp);
  load_and_exec_config_or_die(config, cgp_command);
  free(cgp_command);
  load_and_exec_config_or_die(config, "gen");
  load_and_exec_config_or_die(config, "sim");
  const SimResults *sim_results = config_get_sim_results(config);
  uint64_t hash = FNV_64_OFFSET_BASIS;
  const int num_plays = sim_results_get_number_of_plays(sim_results);
  for (int play_idx = 0; play_idx < num_plays; play_idx++) {
    const SimmedPlay *simmed_play =
        sim_results_get_simmed_play(sim_results, play_idx);
    const uint64_t num_samples =
        stat_get_num_samples(simmed_play_get_equity_stat(simmed_play));
    const double equity_mean = simmed_play_get_exact_equity_mean(simmed_play);
    const double win_pct_mean = simmed_play_get_exact_win_pct_mean(simmed_play);
    const double utility_mean = simmed_play_get_exact_utility_mean(simmed_play);
    if (verbose) {
      printf("  play %2d: n=%llu eq=%.17g wp=%.17g bu=%.17g\n", play_idx,
             (unsigned long long)num_samples, equity_mean, win_pct_mean,
             utility_mean);
    }
    hash = fnv64a_step(hash, num_samples);
    hash = fnv64a_step(hash, simdet_bits(equity_mean));
    hash = fnv64a_step(hash, simdet_bits(win_pct_mean));
    hash = fnv64a_step(hash, simdet_bits(utility_mean));
  }
  hash =
      fnv64a_step(hash, (uint64_t)sim_results_get_best_move_index(sim_results));
  hash = fnv64a_step(hash, (uint64_t)bai_result_get_status(
                               sim_results_get_bai_result(sim_results)));
  hash = fnv64a_step(hash, sim_results_get_iteration_count(sim_results));
  if (verbose) {
    printf("  best=%d status=%d iterations=%llu\n",
           sim_results_get_best_move_index(sim_results),
           (int)bai_result_get_status(sim_results_get_bai_result(sim_results)),
           (unsigned long long)sim_results_get_iteration_count(sim_results));
  }
  config_destroy(config);
  return hash;
}

void test_sim_determinism_sweep(void) {
  const char *const positions[] = {
      DOUG_V_EMELY_CGP, JOSH2_CGP,   NOAH_VS_MISHU_CGP,
      VS_FRENTZ_CGP,    VS_ANDY_CGP, SOME_ISC_GAME_CGP,
  };
  const int num_positions = (int)(sizeof(positions) / sizeof(positions[0]));

  char default_threads[64];
  (void)snprintf(default_threads, sizeof(default_threads), "1,2,4,8,%d",
                 get_num_cores());
  char thread_tokens[SIMDET_MAX_LIST][SIMDET_MAX_TOKEN];
  char iter_tokens[SIMDET_MAX_LIST][SIMDET_MAX_TOKEN];
  char scond_tokens[SIMDET_MAX_LIST][SIMDET_MAX_TOKEN];
  char sr_tokens[SIMDET_MAX_LIST][SIMDET_MAX_TOKEN];
  const int num_threads = simdet_split(
      simdet_env_or("SIMDET_THREADS", default_threads), thread_tokens);
  const int num_iters =
      simdet_split(simdet_env_or("SIMDET_ITERS", "500,3000"), iter_tokens);
  const int num_sconds =
      simdet_split(simdet_env_or("SIMDET_SCOND", "99,none"), scond_tokens);
  const int num_srs =
      simdet_split(simdet_env_or("SIMDET_SR", "tt,rr"), sr_tokens);
  bool run_position[SIMDET_MAX_LIST] = {false};
  const char *position_list = getenv("SIMDET_POS");
  if (position_list == NULL || position_list[0] == '\0') {
    for (int pos_idx = 0; pos_idx < num_positions; pos_idx++) {
      run_position[pos_idx] = true;
    }
  } else {
    char pos_tokens[SIMDET_MAX_LIST][SIMDET_MAX_TOKEN];
    const int num_pos_tokens = simdet_split(position_list, pos_tokens);
    for (int token_idx = 0; token_idx < num_pos_tokens; token_idx++) {
      const int pos_idx = (int)strtol(pos_tokens[token_idx], NULL, 10);
      if (pos_idx >= 0 && pos_idx < num_positions) {
        run_position[pos_idx] = true;
      }
    }
  }
  const bool verbose = strcmp(simdet_env_or("SIMDET_VERBOSE", "0"), "1") == 0;
  const char *plays = simdet_env_or("SIMDET_PLAYS", "15");
  const char *plies = simdet_env_or("SIMDET_PLIES", "2");
  const char *min_play = simdet_env_or("SIMDET_MINPLAY", "10");

  uint64_t combined = FNV_64_OFFSET_BASIS;
  int num_mismatches = 0;
  for (int pos_idx = 0; pos_idx < num_positions; pos_idx++) {
    if (!run_position[pos_idx]) {
      continue;
    }
    for (int iter_idx = 0; iter_idx < num_iters; iter_idx++) {
      for (int scond_idx = 0; scond_idx < num_sconds; scond_idx++) {
        for (int sr_idx = 0; sr_idx < num_srs; sr_idx++) {
          uint64_t reference = 0;
          for (int thread_idx = 0; thread_idx < num_threads; thread_idx++) {
            char settings[512];
            (void)snprintf(
                settings, sizeof(settings),
                "set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all "
                "-r2 all -numplays %s -plies %s -threads %s -iterations %s "
                "-minplayiterations %s -scond %s -sr %s -tlim 0 -seed 42 "
                "-savesettings false -hr false",
                plays, plies, thread_tokens[thread_idx], iter_tokens[iter_idx],
                min_play, scond_tokens[scond_idx], sr_tokens[sr_idx]);
            if (verbose) {
              printf("SIMDET run pos=%d iters=%s scond=%s sr=%s threads=%s\n",
                     pos_idx, iter_tokens[iter_idx], scond_tokens[scond_idx],
                     sr_tokens[sr_idx], thread_tokens[thread_idx]);
            }
            const uint64_t fingerprint =
                simdet_run(settings, positions[pos_idx], verbose);
            if (thread_idx == 0) {
              reference = fingerprint;
              combined = fnv64a_step(combined, fingerprint);
            } else if (fingerprint != reference) {
              num_mismatches++;
              printf("SIMDET MISMATCH pos=%d iters=%s scond=%s sr=%s "
                     "threads=%s: %016llx != %016llx\n",
                     pos_idx, iter_tokens[iter_idx], scond_tokens[scond_idx],
                     sr_tokens[sr_idx], thread_tokens[thread_idx],
                     (unsigned long long)fingerprint,
                     (unsigned long long)reference);
            }
          }
          printf("SIMDET pos=%d iters=%s scond=%s sr=%s: %016llx\n", pos_idx,
                 iter_tokens[iter_idx], scond_tokens[scond_idx],
                 sr_tokens[sr_idx], (unsigned long long)reference);
          (void)fflush(stdout);
        }
      }
    }
  }
  printf("SIMDET combined %016llx, %d mismatches\n",
         (unsigned long long)combined, num_mismatches);
  (void)fflush(stdout);
  assert(num_mismatches == 0);
}
