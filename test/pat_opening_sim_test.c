#include "pat_opening_sim_test.h"

#include "../src/def/rack_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/game.h"
#include "../src/ent/move.h"
#include "../src/ent/sim_results.h"
#include "../src/ent/stats.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/str/move_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

// Opening-length diagnostic by simulation (Astra's C, the user's
// suggestion of sim regret over final-score residuals): for seeded opening
// racks, the top PAT_OPENING_SIM_NUM_PLAYS static candidates under the
// champion are each simmed PAT_OPENING_SIM_PLIES plies for
// PAT_OPENING_SIM_ITERATIONS_PER_PLAY iterations (round robin, so every
// candidate gets the same budget). The gap sim - static per candidate,
// averaged by the candidate's tile count, is what static evaluation
// misses about opening length; the table fitted on the dev racks (even
// attempts) is scored on the eval racks (odd attempts) by the paired
// difference sim(choice with table) - sim(static choice), which is
// unbiased because neither choice looks at the sim noise -- unlike a
// regret against the sim-best play, which the max over noisy means
// inflates.
//
// The table for a lexicon is measured under that lexicon's base file --
// one WITHOUT an opening table, or the gap would be measured against a
// static equity that already carries it -- with
// "patopeningsim:<lexicon>:<pat>[:<racks>]"; the ready-to-paste rows are
// printed at the end. The plain "patopeningsim" runs the defaults below
// (the measurement pat_dls_champion_v5's table came from).
#define PAT_OPENING_SIM_NUM_RACKS 1000
#define PAT_OPENING_SIM_NUM_PLAYS 12
#define PAT_OPENING_SIM_PLIES 4
#define PAT_OPENING_SIM_ITERATIONS_PER_PLAY 400
#define PAT_OPENING_SIM_THREADS 10
#define PAT_OPENING_SIM_SEED_BASE 7300000000ULL
#define PAT_OPENING_SIM_LEXICON "CSW21"
#define PAT_OPENING_SIM_PAT "pat_dls_champion_v4"

typedef struct OpeningCandidate {
  int tiles;
  double static_equity;
  double sim_equity;
} OpeningCandidate;

typedef struct OpeningRack {
  int num_candidates;
  OpeningCandidate candidates[PAT_OPENING_SIM_NUM_PLAYS];
} OpeningRack;

static int opening_best_index(const OpeningRack *rack, const double *table) {
  int best = -1;
  double best_value = 0.0;
  for (int i = 0; i < rack->num_candidates; i++) {
    const OpeningCandidate *c = &rack->candidates[i];
    const double value = c->static_equity + (table ? table[c->tiles] : 0.0);
    if (best < 0 || value > best_value) {
      best = i;
      best_value = value;
    }
  }
  return best;
}

void pat_opening_sim_run(const char *lexicon, const char *pat_name,
                         int max_racks) {
  // The speed-only tables when the lexicon has them (they change no
  // move choice); asking for a missing one is an error, so check first.
  char *wmp_path = get_formatted_string("./data/lexica/%s.wmp", lexicon);
  char *rit_path = get_formatted_string("./data/lexica/%s.rit", lexicon);
  char *wit_path = get_formatted_string("./data/lexica/%s.wit", lexicon);
  const bool have_wmp = access(wmp_path, R_OK) == 0;
  const bool have_rit = access(rit_path, R_OK) == 0;
  const bool have_wit = access(wit_path, R_OK) == 0;
  free(wmp_path);
  free(rit_path);
  free(wit_path);
  char *set_cmd = get_formatted_string(
      "set -lex %s -wmp %s %s %s -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays %d -plies %d -threads %d -iter %d -sr rr -scond none "
      "-threshold none -pat %s",
      lexicon, have_wmp ? "true" : "false",
      have_rit ? "-rit true -ritmmap true" : "", have_wit ? "-wit true" : "",
      PAT_OPENING_SIM_NUM_PLAYS, PAT_OPENING_SIM_PLIES, PAT_OPENING_SIM_THREADS,
      PAT_OPENING_SIM_NUM_PLAYS * PAT_OPENING_SIM_ITERATIONS_PER_PLAY,
      pat_name);
  printf("rack info table %s, word info table %s\n", have_rit ? "on" : "absent",
         have_wit ? "on" : "absent");
  Config *config = config_create_or_die(set_cmd);
  free(set_cmd);
  // The empty board for this build's dimension.
  StringBuilder *cgp_sb = string_builder_create();
  string_builder_add_string(cgp_sb, "cgp ");
  for (int row = 0; row < BOARD_DIM; row++) {
    string_builder_add_formatted_string(cgp_sb, "%s%d", row > 0 ? "/" : "",
                                        BOARD_DIM);
  }
  string_builder_add_string(cgp_sb, " / 0/0 0");
  load_and_exec_config_or_die(config, string_builder_peek(cgp_sb));
  string_builder_destroy(cgp_sb);
  Game *game = config_get_game(config);
  OpeningRack *racks = malloc_or_die(sizeof(OpeningRack) * max_racks);
  int num_racks = 0;
  for (int attempt = 0; attempt < max_racks; attempt++) {
    game_reset(game);
    game_seed(game, PAT_OPENING_SIM_SEED_BASE + (uint64_t)attempt);
    draw_starting_racks(game);
    load_and_exec_config_or_die(config, "gen");
    SimResults *sim_results = config_get_sim_results(config);
    const error_code_t status =
        config_simulate_and_return_status(config, NULL, NULL, sim_results);
    if (status != ERROR_STATUS_SUCCESS) {
      printf("attempt %d: sim status %d, skipped\n", attempt, (int)status);
      continue;
    }
    OpeningRack *rack = &racks[num_racks];
    rack->num_candidates = 0;
    const int num_plays = sim_results_get_number_of_plays(sim_results);
    for (int i = 0;
         i < num_plays && rack->num_candidates < PAT_OPENING_SIM_NUM_PLAYS;
         i++) {
      const SimmedPlay *play = sim_results_get_simmed_play(sim_results, i);
      const Move *move = simmed_play_get_move(play);
      if (move_get_type(move) != GAME_EVENT_TILE_PLACEMENT_MOVE) {
        // Exchanges and passes: tile count 0.
      }
      OpeningCandidate *c = &rack->candidates[rack->num_candidates++];
      c->tiles = (move_get_type(move) == GAME_EVENT_TILE_PLACEMENT_MOVE)
                     ? move_get_tiles_played(move)
                     : 0;
      c->static_equity = equity_to_double(move_get_equity(move));
      c->sim_equity = stat_get_mean(simmed_play_get_equity_stat(play));
      if (attempt == 0) {
        StringBuilder *sb = string_builder_create();
        string_builder_add_move_description(sb, move, config_get_ld(config));
        printf("  rack 0 candidate %s: tiles %d static %.2f sim %.2f (%llu "
               "samples)\n",
               string_builder_peek(sb), c->tiles, c->static_equity,
               c->sim_equity,
               (unsigned long long)stat_get_num_samples(
                   simmed_play_get_equity_stat(play)));
        string_builder_destroy(sb);
      }
    }
    if (rack->num_candidates > 0) {
      num_racks++;
    }
    if ((attempt + 1) % 50 == 0) {
      printf("  %d racks simmed\n", attempt + 1);
      fflush(stdout);
    }
  }

  // The gap by tile count over every candidate (dev and eval), and the
  // table from the dev racks alone.
  Stat *gap_all[RACK_SIZE + 1];
  Stat *gap_dev[RACK_SIZE + 1];
  Stat *gap_best[RACK_SIZE + 1]; // the static-best candidate only
  for (int t = 0; t <= RACK_SIZE; t++) {
    gap_all[t] = stat_create(true);
    gap_dev[t] = stat_create(true);
    gap_best[t] = stat_create(true);
  }
  long static_best_tiles[RACK_SIZE + 1] = {0};
  long sim_best_tiles[RACK_SIZE + 1] = {0};
  int agree = 0;
  for (int r = 0; r < num_racks; r++) {
    const OpeningRack *rack = &racks[r];
    const int sb = opening_best_index(rack, NULL);
    int sim_best = 0;
    for (int i = 1; i < rack->num_candidates; i++) {
      if (rack->candidates[i].sim_equity >
          rack->candidates[sim_best].sim_equity) {
        sim_best = i;
      }
    }
    static_best_tiles[rack->candidates[sb].tiles]++;
    sim_best_tiles[rack->candidates[sim_best].tiles]++;
    agree += (sim_best == sb);
    stat_push(gap_best[rack->candidates[sb].tiles],
              rack->candidates[sb].sim_equity -
                  rack->candidates[sb].static_equity,
              1);
    for (int i = 0; i < rack->num_candidates; i++) {
      const OpeningCandidate *c = &rack->candidates[i];
      stat_push(gap_all[c->tiles], c->sim_equity - c->static_equity, 1);
      if (r % 2 == 0) {
        stat_push(gap_dev[c->tiles], c->sim_equity - c->static_equity, 1);
      }
    }
  }
  printf("\nopening sim diagnostic: %d racks, top %d static candidates, %d "
         "plies, %d iterations per candidate, %s, %s\n",
         num_racks, PAT_OPENING_SIM_NUM_PLAYS, PAT_OPENING_SIM_PLIES,
         PAT_OPENING_SIM_ITERATIONS_PER_PLAY, lexicon, pat_name);
  printf("sim - static by tiles played (all candidates; candidate-level SE, "
         "within-rack correlation ignored):\n");
  double table[RACK_SIZE + 1] = {0};
  for (int t = 0; t <= RACK_SIZE; t++) {
    const uint64_t n = stat_get_num_samples(gap_all[t]);
    if (n == 0) {
      continue;
    }
    const double mean = stat_get_mean(gap_all[t]);
    const double se =
        (n > 1) ? stat_get_stdev(gap_all[t]) / sqrt((double)n) : 0;
    const uint64_t nb = stat_get_num_samples(gap_best[t]);
    printf("  %d tiles: %+.3f +/- %.3f (%llu candidates); as the static "
           "best: %+.3f +/- %.3f (%llu racks, %.1f%%); sim-best %ld\n",
           t, mean, se, (unsigned long long)n,
           nb > 0 ? stat_get_mean(gap_best[t]) : 0.0,
           nb > 1 ? stat_get_stdev(gap_best[t]) / sqrt((double)nb) : 0.0,
           (unsigned long long)nb, 100.0 * (double)nb / num_racks,
           sim_best_tiles[t]);
    table[t] =
        stat_get_num_samples(gap_dev[t]) > 0 ? stat_get_mean(gap_dev[t]) : 0.0;
  }
  printf("static best agrees with sim best on %d / %d racks (%.1f%%)\n", agree,
         num_racks, 100.0 * agree / num_racks);
  // Score the dev table on the eval racks: paired improvement over the
  // static choice, and how often the choice changes.
  printf("dev table (sim - static by tiles, even racks):");
  for (int t = 0; t <= RACK_SIZE; t++) {
    printf(" %d:%+.2f", t, table[t]);
  }
  printf("\n");
  Stat *improvement = stat_create(true);
  int changed = 0;
  int eval_racks = 0;
  Stat *improvement_changed = stat_create(true);
  for (int r = 1; r < num_racks; r += 2) {
    const OpeningRack *rack = &racks[r];
    const int sb = opening_best_index(rack, NULL);
    const int tb = opening_best_index(rack, table);
    const double delta =
        rack->candidates[tb].sim_equity - rack->candidates[sb].sim_equity;
    stat_push(improvement, delta, 1);
    if (tb != sb) {
      changed++;
      stat_push(improvement_changed, delta, 1);
    }
    eval_racks++;
  }
  const uint64_t ne = stat_get_num_samples(improvement);
  printf("eval racks (odd): %d; table changes the choice on %d (%.1f%%); "
         "paired sim improvement over the static choice %+.3f +/- %.3f per "
         "rack, %+.3f +/- %.3f per changed rack\n",
         eval_racks, changed, 100.0 * changed / eval_racks,
         stat_get_mean(improvement),
         ne > 1 ? stat_get_stdev(improvement) / sqrt((double)ne) : 0.0,
         changed > 0 ? stat_get_mean(improvement_changed) : 0.0,
         changed > 1
             ? stat_get_stdev(improvement_changed) / sqrt((double)changed)
             : 0.0);
  // The rows a file would carry: every bin's mean over all racks,
  // relative to the best bin so the best is 0 and the rest are
  // penalties (<= 0, as the format requires), in milli-equity. Bin 0 is
  // the exchange; a bin without samples gets no row (reads as 0).
  double best_mean = 0.0;
  bool have_best = false;
  for (int t = 0; t <= RACK_SIZE; t++) {
    if (stat_get_num_samples(gap_all[t]) > 0 &&
        (!have_best || stat_get_mean(gap_all[t]) > best_mean)) {
      best_mean = stat_get_mean(gap_all[t]);
      have_best = true;
    }
  }
  printf("rows for the file (all racks, relative to the best bin):\n");
  for (int t = 2; t <= RACK_SIZE; t++) {
    if (stat_get_num_samples(gap_all[t]) > 0) {
      printf("opening_tiles_%d,%ld\n", t,
             lround((stat_get_mean(gap_all[t]) - best_mean) * 1000.0));
    }
  }
  if (stat_get_num_samples(gap_all[0]) > 0) {
    printf("opening_exchange,%ld\n",
           lround((stat_get_mean(gap_all[0]) - best_mean) * 1000.0));
  }
  stat_destroy(improvement);
  stat_destroy(improvement_changed);
  for (int t = 0; t <= RACK_SIZE; t++) {
    stat_destroy(gap_all[t]);
    stat_destroy(gap_dev[t]);
    stat_destroy(gap_best[t]);
  }
  free(racks);
  config_destroy(config);
}

void test_pat_opening_sim(void) {
  pat_opening_sim_run(PAT_OPENING_SIM_LEXICON, PAT_OPENING_SIM_PAT,
                      PAT_OPENING_SIM_NUM_RACKS);
}

// "<lexicon>:<pat>[:<racks>]"
void pat_opening_sim_run_spec(const char *spec) {
  StringSplitter *fields = split_string(spec, ':', true);
  const int num_fields = string_splitter_get_number_of_items(fields);
  if (num_fields < 2 || num_fields > 3) {
    log_fatal("patopeningsim spec must be <lexicon>:<pat>[:<racks>], got '%s'",
              spec);
  }
  const int racks = (num_fields == 3)
                        ? atoi(string_splitter_get_item(fields, 2))
                        : PAT_OPENING_SIM_NUM_RACKS;
  if (racks < 2) {
    log_fatal("patopeningsim needs at least 2 racks, got %d", racks);
  }
  pat_opening_sim_run(string_splitter_get_item(fields, 0),
                      string_splitter_get_item(fields, 1), racks);
  string_splitter_destroy(fields);
}
