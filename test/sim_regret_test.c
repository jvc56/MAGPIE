// Sim move-choice regret against a high-budget reference.
//
// A paired, low-noise quality measure for changes to how a sim allocates its
// samples. For a fixed set of positions, a reference run gives every
// candidate play a precise value (a large round-robin sim, so every play gets
// the same number of samples). Each evaluation run then sims the same
// positions at a small budget under several seeds, and scores the play it
// chose by how much reference win% (and equity) it gives up against the
// reference's best play. Two builds evaluated against the same reference file
// see identical positions, candidates and seeds, so their mean regrets are
// directly comparable.
//
//   SIMREG_MODE=ref  SIMREG_FILE=ref.txt ./bin/magpie_test simregret
//   SIMREG_MODE=eval SIMREG_FILE=ref.txt ./bin/magpie_test simregret
//
// Every evaluated decision is printed as a "SIMREG decision" line, so two
// builds can also be compared decision by decision.
//
// Env vars:
//   SIMREG_MODE     ref or eval (default eval)
//   SIMREG_FILE     reference file (default simregret_ref.txt)
//   SIMREG_REFITERS samples per play in the reference (default 8000)
//   SIMREG_ITERS    -iterations per evaluated sim (default 1000)
//   SIMREG_SEEDS    seeds per position (default 8)
//   SIMREG_THREADS  sim threads (default <cores>)
//   SIMREG_PLIES    sim plies (default 2)
//   SIMREG_MINPLAY  -minplayiterations for evaluated sims (default 10)

#include "sim_regret_test.h"

#include "../src/compat/memory_info.h"
#include "../src/ent/move.h"
#include "../src/ent/sim_results.h"
#include "../src/impl/config.h"
#include "../src/str/move_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_constants.h"
#include "test_util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  SIMREG_NUM_PLAYS = 15,
  SIMREG_MAX_MOVE_STRING = 64,
};

typedef struct SimRegretPlay {
  char move[SIMREG_MAX_MOVE_STRING];
  double win_pct;
  double equity;
} SimRegretPlay;

static const char *simreg_env_or(const char *name, const char *fallback) {
  const char *value = getenv(name);
  return value != NULL && value[0] != '\0' ? value : fallback;
}

static const char *const simreg_positions[] = {
    DOUG_V_EMELY_CGP,  GUY_VS_BOT_ALMOST_COMPLETE_CGP,
    GUY_VS_BOT_CGP,    INCOMPLETE_3_CGP,
    INCOMPLETE4_CGP,   INCOMPLETE_ELISE_CGP,
    INCOMPLETE_CGP,    JOSH2_CGP,
    NOAH_VS_MISHU_CGP, NOAH_VS_PETER_CGP,
    SOME_ISC_GAME_CGP, UTF8_DOS_CGP,
    VS_FRENTZ_CGP,     KA_OPENING_CGP,
    AA_OPENING_CGP,    TRIPLE_LETTERS_CGP,
    TRIPLE_DOUBLE_CGP, LATER_BETWEEN_DOUBLE_WORDS_CGP,
    UEY_CGP,
};

static void simreg_move_string(const Config *config, const Move *move,
                               char *out) {
  StringBuilder *move_string_builder = string_builder_create();
  string_builder_add_move_description(move_string_builder, move,
                                      config_get_ld(config));
  (void)snprintf(out, SIMREG_MAX_MOVE_STRING, "%s",
                 string_builder_peek(move_string_builder));
  string_builder_destroy(move_string_builder);
}

// Loads the position, generates the candidates and sims them.
static Config *simreg_sim(const char *settings, const char *cgp) {
  Config *config = config_create_or_die(settings);
  char *cgp_command = get_formatted_string("cgp %s", cgp);
  load_and_exec_config_or_die(config, cgp_command);
  free(cgp_command);
  load_and_exec_config_or_die(config, "gen");
  load_and_exec_config_or_die(config, "sim");
  return config;
}

static void simreg_reference(const char *file_name, const char *threads,
                             const char *plies) {
  const int ref_iters_per_play =
      (int)strtol(simreg_env_or("SIMREG_REFITERS", "8000"), NULL, 10);
  FILE *file = fopen_or_die(file_name, "w");
  const int num_positions =
      (int)(sizeof(simreg_positions) / sizeof(simreg_positions[0]));
  for (int pos_idx = 0; pos_idx < num_positions; pos_idx++) {
    char *settings = get_formatted_string(
        "set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
        "-numplays %d -plies %s -threads %s -iterations %d "
        "-minplayiterations %d -scond none -sr rr -tlim 0 -seed 1 "
        "-savesettings false",
        SIMREG_NUM_PLAYS, plies, threads, SIMREG_NUM_PLAYS * ref_iters_per_play,
        ref_iters_per_play);
    Config *config = simreg_sim(settings, simreg_positions[pos_idx]);
    free(settings);
    const SimResults *sim_results = config_get_sim_results(config);
    const int num_plays = sim_results_get_number_of_plays(sim_results);
    for (int play_idx = 0; play_idx < num_plays; play_idx++) {
      const SimmedPlay *simmed_play =
          sim_results_get_simmed_play(sim_results, play_idx);
      char move[SIMREG_MAX_MOVE_STRING];
      simreg_move_string(config, simmed_play_get_move(simmed_play), move);
      fprintf_or_die(file, "%d\t%s\t%.9f\t%.9f\n", pos_idx, move,
                     simmed_play_get_exact_win_pct_mean(simmed_play),
                     simmed_play_get_exact_equity_mean(simmed_play));
    }
    printf("SIMREG ref pos=%d plays=%d\n", pos_idx, num_plays);
    (void)fflush(stdout);
    config_destroy(config);
  }
  fclose_or_die(file);
}

// Reads the reference plays for one position. Returns the count.
static int simreg_read_position(const char *file_name, const int pos_idx,
                                SimRegretPlay *plays) {
  FILE *file = fopen_or_die(file_name, "r");
  char line[256];
  int num_plays = 0;
  while (fgets(line, sizeof(line), file) != NULL &&
         num_plays < SIMREG_NUM_PLAYS) {
    char *fields[4];
    int num_fields = 0;
    char *cursor = line;
    while (num_fields < 4) {
      fields[num_fields++] = cursor;
      char *tab = strchr(cursor, '\t');
      if (tab == NULL) {
        break;
      }
      *tab = '\0';
      cursor = tab + 1;
    }
    if (num_fields < 4 || (int)strtol(fields[0], NULL, 10) != pos_idx) {
      continue;
    }
    SimRegretPlay *play = &plays[num_plays++];
    (void)snprintf(play->move, SIMREG_MAX_MOVE_STRING, "%s", fields[1]);
    play->win_pct = strtod(fields[2], NULL);
    play->equity = strtod(fields[3], NULL);
  }
  fclose_or_die(file);
  return num_plays;
}

static void simreg_evaluate(const char *file_name, const char *threads,
                            const char *plies) {
  const char *iters = simreg_env_or("SIMREG_ITERS", "1000");
  const char *min_play = simreg_env_or("SIMREG_MINPLAY", "10");
  const int num_seeds =
      (int)strtol(simreg_env_or("SIMREG_SEEDS", "8"), NULL, 10);
  const int num_positions =
      (int)(sizeof(simreg_positions) / sizeof(simreg_positions[0]));
  double total_win_pct_regret = 0.0;
  double total_equity_regret = 0.0;
  int num_decisions = 0;
  int num_best = 0;
  for (int pos_idx = 0; pos_idx < num_positions; pos_idx++) {
    SimRegretPlay plays[SIMREG_NUM_PLAYS];
    const int num_plays = simreg_read_position(file_name, pos_idx, plays);
    assert(num_plays > 0);
    int best_idx = 0;
    for (int play_idx = 1; play_idx < num_plays; play_idx++) {
      if (plays[play_idx].win_pct > plays[best_idx].win_pct ||
          (plays[play_idx].win_pct == plays[best_idx].win_pct &&
           plays[play_idx].equity > plays[best_idx].equity)) {
        best_idx = play_idx;
      }
    }
    double position_regret = 0.0;
    for (int seed = 1; seed <= num_seeds; seed++) {
      char *settings = get_formatted_string(
          "set -lex CSW21 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
          "-numplays %d -plies %s -threads %s -iterations %s "
          "-minplayiterations %s -scond 99 -sr tt -tlim 0 -seed %d "
          "-savesettings false",
          SIMREG_NUM_PLAYS, plies, threads, iters, min_play, seed);
      Config *config = simreg_sim(settings, simreg_positions[pos_idx]);
      free(settings);
      char chosen[SIMREG_MAX_MOVE_STRING];
      simreg_move_string(
          config, sim_results_get_best_move(config_get_sim_results(config)),
          chosen);
      config_destroy(config);
      int chosen_idx = -1;
      for (int play_idx = 0; play_idx < num_plays; play_idx++) {
        if (strings_equal(plays[play_idx].move, chosen)) {
          chosen_idx = play_idx;
        }
      }
      assert(chosen_idx >= 0);
      const double win_pct_regret =
          plays[best_idx].win_pct - plays[chosen_idx].win_pct;
      total_win_pct_regret += win_pct_regret;
      total_equity_regret += plays[best_idx].equity - plays[chosen_idx].equity;
      position_regret += win_pct_regret;
      printf("SIMREG decision pos=%d seed=%d win%%_regret=%.9f "
             "equity_regret=%.9f\n",
             pos_idx, seed, 100.0 * win_pct_regret,
             plays[best_idx].equity - plays[chosen_idx].equity);
      num_best += chosen_idx == best_idx;
      num_decisions++;
    }
    printf("SIMREG pos=%d mean win%% regret %.4f\n", pos_idx,
           100.0 * position_regret / num_seeds);
    (void)fflush(stdout);
  }
  printf("SIMREG iters=%s decisions=%d best=%d (%.1f%%) mean win%% regret "
         "%.4f mean equity regret %.4f\n",
         iters, num_decisions, num_best, 100.0 * num_best / num_decisions,
         100.0 * total_win_pct_regret / num_decisions,
         total_equity_regret / num_decisions);
}

void test_sim_regret(void) {
  char default_threads[16];
  (void)snprintf(default_threads, sizeof(default_threads), "%d",
                 get_num_cores());
  const char *mode = simreg_env_or("SIMREG_MODE", "eval");
  const char *file_name = simreg_env_or("SIMREG_FILE", "simregret_ref.txt");
  const char *threads = simreg_env_or("SIMREG_THREADS", default_threads);
  const char *plies = simreg_env_or("SIMREG_PLIES", "2");
  if (strings_equal(mode, "ref")) {
    simreg_reference(file_name, threads, plies);
  } else {
    simreg_evaluate(file_name, threads, plies);
  }
}
