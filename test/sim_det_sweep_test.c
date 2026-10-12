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
// The sweep covers 1, 2, 4, 8 and <cores> threads, -iterations 500 and 3000,
// -scond 99 and none, and -sr tt and rr, all at 15 plays, 2 plies and
// -minplayiterations 10.

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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static uint64_t simdet_run(const char *settings, const char *cgp) {
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
  config_destroy(config);
  return hash;
}

void test_sim_determinism_sweep(void) {
  const char *const positions[] = {
      DOUG_V_EMELY_CGP, JOSH2_CGP,   NOAH_VS_MISHU_CGP,
      VS_FRENTZ_CGP,    VS_ANDY_CGP, SOME_ISC_GAME_CGP,
  };
  const int num_positions = (int)(sizeof(positions) / sizeof(positions[0]));

  const int thread_counts[] = {1, 2, 4, 8, get_num_cores()};
  const int num_threads =
      (int)(sizeof(thread_counts) / sizeof(thread_counts[0]));
  const char *const iter_tokens[] = {"500", "3000"};
  const int num_iters = (int)(sizeof(iter_tokens) / sizeof(iter_tokens[0]));
  const char *const scond_tokens[] = {"99", "none"};
  const int num_sconds = (int)(sizeof(scond_tokens) / sizeof(scond_tokens[0]));
  const char *const sr_tokens[] = {"tt", "rr"};
  const int num_srs = (int)(sizeof(sr_tokens) / sizeof(sr_tokens[0]));

  uint64_t combined = FNV_64_OFFSET_BASIS;
  int num_mismatches = 0;
  for (int pos_idx = 0; pos_idx < num_positions; pos_idx++) {
    for (int iter_idx = 0; iter_idx < num_iters; iter_idx++) {
      for (int scond_idx = 0; scond_idx < num_sconds; scond_idx++) {
        for (int sr_idx = 0; sr_idx < num_srs; sr_idx++) {
          uint64_t reference = 0;
          for (int thread_idx = 0; thread_idx < num_threads; thread_idx++) {
            char settings[512];
            (void)snprintf(
                settings, sizeof(settings),
                "set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all "
                "-r2 all -numplays 15 -plies 2 -threads %d -iterations %s "
                "-minplayiterations 10 -scond %s -sr %s -tlim 0 -seed 42 "
                "-savesettings false -hr false",
                thread_counts[thread_idx], iter_tokens[iter_idx],
                scond_tokens[scond_idx], sr_tokens[sr_idx]);
            const uint64_t fingerprint =
                simdet_run(settings, positions[pos_idx]);
            if (thread_idx == 0) {
              reference = fingerprint;
              combined = fnv64a_step(combined, fingerprint);
            } else if (fingerprint != reference) {
              num_mismatches++;
              printf("SIMDET MISMATCH pos=%d iters=%s scond=%s sr=%s "
                     "threads=%d: %016llx != %016llx\n",
                     pos_idx, iter_tokens[iter_idx], scond_tokens[scond_idx],
                     sr_tokens[sr_idx], thread_counts[thread_idx],
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
