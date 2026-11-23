// What changes to a sim's horizon are worth: LAB, NOT FOR MERGE.
//
// A sim's rollout ends with the spread plus each player's last leave value
// and its pool shift (-poolshift). The variants value the leaves plainly,
// with both shifts (the sim as it is) or with the simming player's shift
// alone, and add none, half or all of the last play's PAT term (see
// src/impl/sim_lab.h). None changes which plays a rollout makes, so one set
// of rollouts values every variant, and the variants differ only in how
// each rollout's end is scored.
//
// Positions come from static self-play: every position whose mover holds a
// full rack with at least a rack in the bag. For each:
//   screen  every candidate gets the same rollouts (round robin). Candidates
//           that no variant puts near its best are dropped.
//   main    the rest get many more. With <mid>, they first get <mid> each,
//           and only those some variant still puts near its best get the
//           rest. Each variant's pick is the candidate with the best mean
//           utility over all of these. One rollout seed in
//           <cont> is played on to the end of the game, and the rollouts
//           are split in parts by seed, the same way for every candidate
//           (for the analysis across candidates).
//   oracle  where the variants' picks differ, every pick gets <oracle>
//           fresh rollouts, all played to the end of the game, which value
//           the picks. 0 leaves this stage out.
//
// Spec: first=<game>,games=<n>,plies=<p>,screen=<rollouts per play>,
//       main=<rollouts per play>,cont=<one seed in this many plays on>,
//       oracle=<rollouts per pick>[,mid=<rollouts per play>]
//
// Lines printed:
//   SIMH pos ...     one per position
//   SIMH screen ...  a candidate's screen values
//   SIMH mid ...     a kept candidate's utilities after <mid> rollouts
//   SIMH main ...    a kept candidate's values in one part
//   SIMH picks ...   the main play each variant picks, by one measure
//   SIMH oracle ...  a pick's finished games in one part
//   SIMH checks ...  the lab's checks of its own arithmetic

#include "sim_horizon_test.h"

#include "../src/compat/ctime.h"
#include "../src/compat/memory_info.h"
#include "../src/def/rack_defs.h"
#include "../src/ent/game.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/rack.h"
#include "../src/ent/sim_results.h"
#include "../src/impl/cgp.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/sim_lab.h"
#include "../src/str/move_string.h"
#include "../src/util/io_util.h"
#include "../src/util/string_util.h"
#include "test_util.h"
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  SIMH_NUM_PLAYS = 15,
  SIMH_MAX_KEPT = 6,
  SIMH_MAX_MOVE_STRING = 64,
  SIMH_SPEC_FIELDS = 7,
  SIMH_SPEC_FIELDS_WITH_MID = 8,
  SIMH_NUM_MEASURES = 3,
  SIMH_MEASURE_UTILITY = 2,
  SIMH_FIRST_GAME_SEED = 5000,
  SIMH_POSITIONS_PER_GAME = 100,
  SIMH_SCREEN_SEED_STEP = 7,
  SIMH_SCREEN_SEED_OFFSET = 17,
  SIMH_MAIN_SEED = 1000003,
  SIMH_ORACLE_SEED = 2000003,
  SIMH_REST_SEED = 3000017,
};

// A candidate is kept when some variant has it within any of these of that
// variant's best candidate: points of equity, win fraction, utility.
static const double SIMH_KEEP_EQUITY = 1.25;
static const double SIMH_KEEP_WIN = 0.008;
static const double SIMH_KEEP_UTILITY = 0.006;
// After <mid> rollouts a candidate stays when some variant has its utility
// within this of that variant's best.
static const double SIMH_KEEP_MID_UTILITY = 0.003;
// The sim's own means against the lab's, which sum the same rollouts.
static const double SIMH_EQUITY_TOLERANCE = 0.002;
static const double SIMH_WIN_TOLERANCE = 1e-9;

typedef struct SimhPlay {
  char move[SIMH_MAX_MOVE_STRING];
  SimLabCell halves[SIM_LAB_NUM_HALVES];
  // The candidate's smallest utility gap to a variant's best, in the screen.
  double closeness;
  bool is_kept;
} SimhPlay;

static void simh_move_string(const Config *config, const Move *move,
                             char *out) {
  StringBuilder *sb = string_builder_create();
  const Game *game = config_get_game(config);
  string_builder_add_ucgi_move(sb, move, game_get_board(game),
                               game_get_ld(game));
  (void)snprintf(out, SIMH_MAX_MOVE_STRING, "%s", string_builder_peek(sb));
  string_builder_destroy(sb);
}

// The position as a cgp command without the opponent's rack, which a sim
// that chooses a play does not know.
static char *simh_cgp_command(const Game *game) {
  char *cgp = game_get_cgp(game, true);
  StringSplitter *fields = split_string_by_whitespace(cgp, true);
  StringBuilder *sb = string_builder_create();
  string_builder_add_string(sb, "cgp ");
  for (int idx = 0; idx < string_splitter_get_number_of_items(fields); idx++) {
    const char *field = string_splitter_get_item(fields, idx);
    if (idx > 0) {
      string_builder_add_char(sb, ' ');
    }
    if (idx == 1) {
      const char *slash = strchr(field, '/');
      const size_t length =
          slash == NULL ? strlen(field) : (size_t)(slash - field);
      for (size_t char_idx = 0; char_idx < length; char_idx++) {
        string_builder_add_char(sb, field[char_idx]);
      }
      string_builder_add_char(sb, '/');
    } else {
      string_builder_add_string(sb, field);
    }
  }
  string_splitter_destroy(fields);
  free(cgp);
  return string_builder_dump_and_destroy(sb, NULL);
}

static void simh_cell_add(SimLabCell *total, const SimLabCell *cell) {
  total->rollouts += cell->rollouts;
  for (int variant = 0; variant < SIM_LAB_NUM_VARIANTS; variant++) {
    total->equity_sum[variant] += cell->equity_sum[variant];
    total->win_sum[variant] += cell->win_sum[variant];
    total->utility_sum[variant] += cell->utility_sum[variant];
  }
  total->shift_sum += cell->shift_sum;
  total->pat_sum += cell->pat_sum;
  for (int split = 0; split < SIM_LAB_NUM_SPLITS; split++) {
    total->split_sum[split] += cell->split_sum[split];
  }
  total->continued += cell->continued;
  total->final_spread_sum += cell->final_spread_sum;
  total->final_win_sum += cell->final_win_sum;
  total->final_utility_sum += cell->final_utility_sum;
  total->deep_plain_sum += cell->deep_plain_sum;
  total->deep_full_sum += cell->deep_full_sum;
  total->pat_term_sum += cell->pat_term_sum;
  total->shift_term_sum += cell->shift_term_sum;
  total->pat_square_sum += cell->pat_square_sum;
  total->shift_square_sum += cell->shift_square_sum;
  total->pat_shift_sum += cell->pat_shift_sum;
  for (int target = 0; target < SIM_LAB_NUM_TARGETS; target++) {
    total->target_sum[target] += cell->target_sum[target];
    total->target_square_sum[target] += cell->target_square_sum[target];
    total->target_pat_sum[target] += cell->target_pat_sum[target];
    total->target_shift_sum[target] += cell->target_shift_sum[target];
  }
}

// Sums the lab's cells over its workers into plays, and names each play.
// Returns how many plays the sim had, and checks the lab's unchanged
// variant against the sim's own results.
static int simh_collect(const Config *config, const SimLab *lab,
                        SimhPlay *plays, int *mean_mismatches) {
  const SimResults *sim_results = config_get_sim_results(config);
  const int number_of_plays = sim_results_get_number_of_plays(sim_results);
  if (number_of_plays > SIM_LAB_MAX_PLAYS) {
    log_fatal("sim horizon: %d plays", number_of_plays);
  }
  for (int play_idx = 0; play_idx < number_of_plays; play_idx++) {
    SimhPlay *play = &plays[play_idx];
    memset(play, 0, sizeof(*play));
    const SimmedPlay *simmed_play =
        sim_results_get_simmed_play(sim_results, play_idx);
    simh_move_string(config, simmed_play_get_move(simmed_play), play->move);
    SimLabCell both;
    memset(&both, 0, sizeof(both));
    for (int half = 0; half < SIM_LAB_NUM_HALVES; half++) {
      for (int worker = 0; worker < SIM_LAB_MAX_WORKERS; worker++) {
        simh_cell_add(&play->halves[half], &lab->cells[worker][play_idx][half]);
      }
      simh_cell_add(&both, &play->halves[half]);
    }
    if (both.rollouts == 0) {
      continue;
    }
    const double rollouts = (double)both.rollouts;
    const double equity = both.equity_sum[SIM_LAB_SIM_VARIANT] / rollouts;
    const double win = both.win_sum[SIM_LAB_SIM_VARIANT] / rollouts;
    if (fabs(equity - simmed_play_get_exact_equity_mean(simmed_play)) >
            SIMH_EQUITY_TOLERANCE ||
        fabs(win - simmed_play_get_exact_win_pct_mean(simmed_play)) >
            SIMH_WIN_TOLERANCE) {
      (*mean_mismatches)++;
      printf("SIMH mean_mismatch play=%d move=%s lab_eq=%.6f sim_eq=%.6f "
             "lab_win=%.9f sim_win=%.9f\n",
             play_idx, play->move, equity,
             simmed_play_get_exact_equity_mean(simmed_play), win,
             simmed_play_get_exact_win_pct_mean(simmed_play));
    }
  }
  return number_of_plays;
}

static void simh_print_variants(const char *name, const double *sums,
                                double count) {
  printf(" %s=", name);
  for (int variant = 0; variant < SIM_LAB_NUM_VARIANTS; variant++) {
    printf("%s%.6f", variant == 0 ? "" : ",", sums[variant] / count);
  }
}

static void simh_print_screen(int pos_idx, int play_idx, const SimhPlay *play) {
  SimLabCell both;
  memset(&both, 0, sizeof(both));
  for (int half = 0; half < SIM_LAB_NUM_HALVES; half++) {
    simh_cell_add(&both, &play->halves[half]);
  }
  const double rollouts = both.rollouts > 0 ? (double)both.rollouts : 1.0;
  printf("SIMH screen pos=%d play=%d move=%s kept=%d n=%llu", pos_idx, play_idx,
         play->move, (int)play->is_kept, (unsigned long long)both.rollouts);
  simh_print_variants("eq", both.equity_sum, rollouts);
  simh_print_variants("win", both.win_sum, rollouts);
  simh_print_variants("util", both.utility_sum, rollouts);
  printf(" shift=%.6f pat=%.6f\n", both.shift_sum / rollouts,
         both.pat_sum / rollouts);
}

static void simh_print_main(int pos_idx, int play_idx, const SimhPlay *play) {
  for (int half = 0; half < SIM_LAB_NUM_HALVES; half++) {
    const SimLabCell *cell = &play->halves[half];
    const double rollouts = cell->rollouts > 0 ? (double)cell->rollouts : 1.0;
    const double continued =
        cell->continued > 0 ? (double)cell->continued : 1.0;
    printf("SIMH main pos=%d play=%d move=%s half=%d n=%llu", pos_idx, play_idx,
           play->move, half, (unsigned long long)cell->rollouts);
    simh_print_variants("eq", cell->equity_sum, rollouts);
    simh_print_variants("win", cell->win_sum, rollouts);
    simh_print_variants("util", cell->utility_sum, rollouts);
    printf(" shift=%.6f pat=%.6f cont=%llu fspread=%.6f fwin=%.6f "
           "futil=%.6f deep=%.6f,%.6f",
           cell->shift_sum / rollouts, cell->pat_sum / rollouts,
           (unsigned long long)cell->continued,
           cell->final_spread_sum / continued, cell->final_win_sum / continued,
           cell->final_utility_sum / continued,
           cell->deep_plain_sum / continued, cell->deep_full_sum / continued);
    // Raw moments of the rollouts played on, for regressions pooled over
    // candidates.
    printf(" mom=%.9g,%.9g,%.9g,%.9g,%.9g", cell->pat_term_sum,
           cell->shift_term_sum, cell->pat_square_sum, cell->shift_square_sum,
           cell->pat_shift_sum);
    for (int target = 0; target < SIM_LAB_NUM_TARGETS; target++) {
      printf(" tgt%d=%.9g,%.9g,%.9g,%.9g", target, cell->target_sum[target],
             cell->target_square_sum[target], cell->target_pat_sum[target],
             cell->target_shift_sum[target]);
    }
    printf(" split=");
    for (int split = 0; split < SIM_LAB_NUM_SPLITS; split++) {
      printf("%s%.6f", split == 0 ? "" : ",",
             cell->split_sum[split] / rollouts);
    }
    printf("\n");
  }
}

// Each variant's pick among the plays, by equity, win fraction and utility:
// the play with the best mean over all of its rollouts.
static void simh_get_picks(const SimhPlay *plays, int number_of_plays,
                           int picks[SIMH_NUM_MEASURES][SIM_LAB_NUM_VARIANTS]) {
  for (int measure = 0; measure < SIMH_NUM_MEASURES; measure++) {
    for (int variant = 0; variant < SIM_LAB_NUM_VARIANTS; variant++) {
      double best_mean = -INFINITY;
      picks[measure][variant] = 0;
      for (int play_idx = 0; play_idx < number_of_plays; play_idx++) {
        SimLabCell both;
        memset(&both, 0, sizeof(both));
        for (int half = 0; half < SIM_LAB_NUM_HALVES; half++) {
          simh_cell_add(&both, &plays[play_idx].halves[half]);
        }
        if (both.rollouts == 0) {
          continue;
        }
        const double *sums = both.equity_sum;
        if (measure == 1) {
          sums = both.win_sum;
        } else if (measure == SIMH_MEASURE_UTILITY) {
          sums = both.utility_sum;
        }
        const double mean = sums[variant] / (double)both.rollouts;
        if (mean > best_mean) {
          best_mean = mean;
          picks[measure][variant] = play_idx;
        }
      }
    }
  }
}

static void
simh_print_picks(int pos_idx,
                 int picks[SIMH_NUM_MEASURES][SIM_LAB_NUM_VARIANTS]) {
  const char *measure_names[SIMH_NUM_MEASURES] = {"eq", "win", "util"};
  for (int measure = 0; measure < SIMH_NUM_MEASURES; measure++) {
    printf("SIMH picks pos=%d measure=%s picks=", pos_idx,
           measure_names[measure]);
    for (int variant = 0; variant < SIM_LAB_NUM_VARIANTS; variant++) {
      printf("%s%d", variant == 0 ? "" : ",", picks[measure][variant]);
    }
    printf("\n");
  }
}

static void simh_print_oracle(int pos_idx, int play_idx, const SimhPlay *play) {
  for (int half = 0; half < SIM_LAB_NUM_HALVES; half++) {
    const SimLabCell *cell = &play->halves[half];
    const double continued =
        cell->continued > 0 ? (double)cell->continued : 1.0;
    printf(
        "SIMH oracle pos=%d play=%d move=%s half=%d n=%llu cont=%llu "
        "fspread=%.6f fwin=%.6f futil=%.6f\n",
        pos_idx, play_idx, play->move, half, (unsigned long long)cell->rollouts,
        (unsigned long long)cell->continued, cell->final_spread_sum / continued,
        cell->final_win_sum / continued, cell->final_utility_sum / continued);
  }
}

// Marks the candidates some variant puts near its best, at most
// SIMH_MAX_KEPT of them, and returns how many.
static int simh_choose_kept(SimhPlay *plays, int number_of_plays) {
  double means[SIM_LAB_MAX_PLAYS][3][SIM_LAB_NUM_VARIANTS];
  bool has_rollouts[SIM_LAB_MAX_PLAYS];
  double best[3][SIM_LAB_NUM_VARIANTS];
  for (int measure = 0; measure < 3; measure++) {
    for (int variant = 0; variant < SIM_LAB_NUM_VARIANTS; variant++) {
      best[measure][variant] = -INFINITY;
    }
  }
  for (int play_idx = 0; play_idx < number_of_plays; play_idx++) {
    SimLabCell both;
    memset(&both, 0, sizeof(both));
    for (int half = 0; half < SIM_LAB_NUM_HALVES; half++) {
      simh_cell_add(&both, &plays[play_idx].halves[half]);
    }
    has_rollouts[play_idx] = both.rollouts > 0;
    if (!has_rollouts[play_idx]) {
      continue;
    }
    const double rollouts = (double)both.rollouts;
    for (int variant = 0; variant < SIM_LAB_NUM_VARIANTS; variant++) {
      means[play_idx][0][variant] = both.equity_sum[variant] / rollouts;
      means[play_idx][1][variant] = both.win_sum[variant] / rollouts;
      means[play_idx][2][variant] = both.utility_sum[variant] / rollouts;
      for (int measure = 0; measure < 3; measure++) {
        if (means[play_idx][measure][variant] > best[measure][variant]) {
          best[measure][variant] = means[play_idx][measure][variant];
        }
      }
    }
  }
  const double keep_within[3] = {SIMH_KEEP_EQUITY, SIMH_KEEP_WIN,
                                 SIMH_KEEP_UTILITY};
  int number_near = 0;
  for (int play_idx = 0; play_idx < number_of_plays; play_idx++) {
    SimhPlay *play = &plays[play_idx];
    play->is_kept = false;
    play->closeness = INFINITY;
    if (!has_rollouts[play_idx]) {
      continue;
    }
    for (int variant = 0; variant < SIM_LAB_NUM_VARIANTS; variant++) {
      for (int measure = 0; measure < 3; measure++) {
        const double gap =
            best[measure][variant] - means[play_idx][measure][variant];
        if (gap <= keep_within[measure]) {
          play->is_kept = true;
        }
      }
      const double utility_gap = best[2][variant] - means[play_idx][2][variant];
      if (utility_gap < play->closeness) {
        play->closeness = utility_gap;
      }
    }
    if (play->is_kept) {
      number_near++;
    }
  }
  // Too many: drop the ones furthest from every variant's best.
  while (number_near > SIMH_MAX_KEPT) {
    int furthest = -1;
    for (int play_idx = 0; play_idx < number_of_plays; play_idx++) {
      if (plays[play_idx].is_kept &&
          (furthest < 0 ||
           plays[play_idx].closeness > plays[furthest].closeness)) {
        furthest = play_idx;
      }
    }
    plays[furthest].is_kept = false;
    number_near--;
  }
  return number_near;
}

// Marks the candidates whose utility some variant has within
// SIMH_KEEP_MID_UTILITY of its best, and returns how many.
static int simh_choose_survivors(SimhPlay *plays, int number_of_plays) {
  double means[SIM_LAB_MAX_PLAYS][SIM_LAB_NUM_VARIANTS];
  bool has_rollouts[SIM_LAB_MAX_PLAYS];
  double best[SIM_LAB_NUM_VARIANTS];
  for (int variant = 0; variant < SIM_LAB_NUM_VARIANTS; variant++) {
    best[variant] = -INFINITY;
  }
  for (int play_idx = 0; play_idx < number_of_plays; play_idx++) {
    SimLabCell both;
    memset(&both, 0, sizeof(both));
    for (int half = 0; half < SIM_LAB_NUM_HALVES; half++) {
      simh_cell_add(&both, &plays[play_idx].halves[half]);
    }
    has_rollouts[play_idx] = both.rollouts > 0;
    if (!has_rollouts[play_idx]) {
      continue;
    }
    for (int variant = 0; variant < SIM_LAB_NUM_VARIANTS; variant++) {
      means[play_idx][variant] =
          both.utility_sum[variant] / (double)both.rollouts;
      if (means[play_idx][variant] > best[variant]) {
        best[variant] = means[play_idx][variant];
      }
    }
  }
  int number_of_survivors = 0;
  for (int play_idx = 0; play_idx < number_of_plays; play_idx++) {
    SimhPlay *play = &plays[play_idx];
    play->is_kept = false;
    if (!has_rollouts[play_idx]) {
      continue;
    }
    for (int variant = 0; variant < SIM_LAB_NUM_VARIANTS; variant++) {
      if (best[variant] - means[play_idx][variant] <= SIMH_KEEP_MID_UTILITY) {
        play->is_kept = true;
      }
    }
    if (play->is_kept) {
      number_of_survivors++;
    }
  }
  return number_of_survivors;
}

static void simh_print_mid(int pos_idx, int play_idx, const SimhPlay *play) {
  SimLabCell both;
  memset(&both, 0, sizeof(both));
  for (int half = 0; half < SIM_LAB_NUM_HALVES; half++) {
    simh_cell_add(&both, &play->halves[half]);
  }
  const double rollouts = both.rollouts > 0 ? (double)both.rollouts : 1.0;
  printf("SIMH mid pos=%d play=%d move=%s kept=%d n=%llu", pos_idx, play_idx,
         play->move, (int)play->is_kept, (unsigned long long)both.rollouts);
  simh_print_variants("util", both.utility_sum, rollouts);
  printf("\n");
}

static void simh_lab_reset(SimLab *lab, int continue_one_in) {
  memset(lab->cells, 0, sizeof(lab->cells));
  lab->continue_one_in = continue_one_in;
}

// Sims the comma-separated moves from the position, the same number of
// rollouts each, and collects the lab's cells into plays. Returns how many
// plays the sim had.
static int simh_sim_moves(Config *config, SimLab *lab, const char *cgp_command,
                          const char *moves, int number_of_moves, int rollouts,
                          int seed, int continue_one_in, SimhPlay *plays,
                          int *mean_mismatches) {
  simh_lab_reset(lab, continue_one_in);
  char *settings =
      get_formatted_string("set -iterations %d -minplayiterations %d -seed %d",
                           number_of_moves * rollouts, rollouts, seed);
  load_and_exec_config_or_die(config, settings);
  free(settings);
  load_and_exec_config_or_die(config, cgp_command);
  char *add_command = get_formatted_string("addmoves %s", moves);
  load_and_exec_config_or_die(config, add_command);
  free(add_command);
  load_and_exec_config_or_die(config, "sim");
  return simh_collect(config, lab, plays, mean_mismatches);
}

static void simh_run(int first_game, int num_games, int plies,
                     int screen_rollouts, int main_rollouts,
                     int continue_one_in, int oracle_rollouts,
                     int mid_rollouts) {
  char *settings = get_formatted_string(
      "set -lex CSW24 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays %d -plies %d -threads %d -scond none -sr rr -tlim 0 "
      "-savesettings false -poolshift 1.2 -pat auto -scensus false",
      SIMH_NUM_PLAYS, plies, get_num_cores());
  Config *config = config_create_or_die(settings);
  free(settings);
  SimLab *lab = calloc_or_die(1, sizeof(SimLab));
  rv_sim_lab_set(lab);
  Game *game = config_game_create(config);
  SimhPlay *plays = malloc_or_die(sizeof(SimhPlay) * SIM_LAB_MAX_PLAYS);
  Timer total_timer;
  ctimer_start(&total_timer);
  int positions = 0;
  int positions_with_main = 0;
  int positions_with_oracle = 0;
  int mean_mismatches = 0;
  SimhPlay *oracle_plays = malloc_or_die(sizeof(SimhPlay) * SIM_LAB_MAX_PLAYS);
  SimhPlay *rest_plays = malloc_or_die(sizeof(SimhPlay) * SIM_LAB_MAX_PLAYS);
  for (int game_idx = first_game; game_idx < first_game + num_games;
       game_idx++) {
    game_reset(game);
    game_seed(game, (uint64_t)(SIMH_FIRST_GAME_SEED + game_idx));
    draw_starting_racks(game);
    int pos_idx = game_idx * SIMH_POSITIONS_PER_GAME;
    while (!game_over(game)) {
      const Rack *rack = player_get_rack(
          game_get_player(game, game_get_player_on_turn_index(game)));
      if (rack_get_total_letters(rack) == RACK_SIZE &&
          game_get_real_bag_size(game) >= RACK_SIZE) {
        char *cgp_command = simh_cgp_command(game);
        Timer position_timer;
        ctimer_start(&position_timer);
        // Screen.
        simh_lab_reset(lab, 0);
        char *screen_settings = get_formatted_string(
            "set -iterations %d -minplayiterations %d -seed %d",
            SIMH_NUM_PLAYS * screen_rollouts, screen_rollouts,
            pos_idx * SIMH_SCREEN_SEED_STEP + SIMH_SCREEN_SEED_OFFSET);
        load_and_exec_config_or_die(config, screen_settings);
        free(screen_settings);
        load_and_exec_config_or_die(config, cgp_command);
        load_and_exec_config_or_die(config, "gen");
        load_and_exec_config_or_die(config, "sim");
        const int number_of_plays =
            simh_collect(config, lab, plays, &mean_mismatches);
        const int number_kept = simh_choose_kept(plays, number_of_plays);
        const double screen_seconds = ctimer_elapsed_seconds(&position_timer);
        printf("SIMH pos idx=%d game=%d bag=%d plays=%d kept=%d %s\n", pos_idx,
               game_idx, game_get_real_bag_size(game), number_of_plays,
               number_kept, cgp_command);
        StringBuilder *kept_moves = string_builder_create();
        for (int play_idx = 0; play_idx < number_of_plays; play_idx++) {
          simh_print_screen(pos_idx, play_idx, &plays[play_idx]);
          if (plays[play_idx].is_kept) {
            if (string_builder_length(kept_moves) > 0) {
              string_builder_add_char(kept_moves, ',');
            }
            string_builder_add_string(kept_moves, plays[play_idx].move);
          }
        }
        // Main.
        double main_seconds = 0.0;
        double oracle_seconds = 0.0;
        int number_of_main_plays = 0;
        if (number_kept > 1) {
          ctimer_start(&position_timer);
          const int first_rollouts =
              mid_rollouts > 0 && mid_rollouts < main_rollouts ? mid_rollouts
                                                               : main_rollouts;
          number_of_main_plays = simh_sim_moves(
              config, lab, cgp_command, string_builder_peek(kept_moves),
              number_kept, first_rollouts, SIMH_MAIN_SEED + pos_idx,
              continue_one_in, plays, &mean_mismatches);
          if (first_rollouts < main_rollouts) {
            const int number_of_survivors =
                simh_choose_survivors(plays, number_of_main_plays);
            StringBuilder *survivor_moves = string_builder_create();
            for (int play_idx = 0; play_idx < number_of_main_plays;
                 play_idx++) {
              simh_print_mid(pos_idx, play_idx, &plays[play_idx]);
              if (plays[play_idx].is_kept) {
                if (string_builder_length(survivor_moves) > 0) {
                  string_builder_add_char(survivor_moves, ',');
                }
                string_builder_add_string(survivor_moves, plays[play_idx].move);
              }
            }
            if (number_of_survivors > 1) {
              const int number_of_rest_plays = simh_sim_moves(
                  config, lab, cgp_command, string_builder_peek(survivor_moves),
                  number_of_survivors, main_rollouts - first_rollouts,
                  SIMH_REST_SEED + pos_idx, continue_one_in, rest_plays,
                  &mean_mismatches);
              // Each survivor's rollouts of both steps together.
              for (int rest_idx = 0; rest_idx < number_of_rest_plays;
                   rest_idx++) {
                for (int play_idx = 0; play_idx < number_of_main_plays;
                     play_idx++) {
                  if (!strings_equal(rest_plays[rest_idx].move,
                                     plays[play_idx].move)) {
                    continue;
                  }
                  for (int half = 0; half < SIM_LAB_NUM_HALVES; half++) {
                    simh_cell_add(&rest_plays[rest_idx].halves[half],
                                  &plays[play_idx].halves[half]);
                  }
                }
              }
              memcpy(plays, rest_plays,
                     sizeof(SimhPlay) * (size_t)number_of_rest_plays);
              number_of_main_plays = number_of_rest_plays;
            } else {
              number_of_main_plays = 1;
            }
            string_builder_destroy(survivor_moves);
          }
          main_seconds = ctimer_elapsed_seconds(&position_timer);
        }
        if (number_of_main_plays > 1) {
          for (int play_idx = 0; play_idx < number_of_main_plays; play_idx++) {
            simh_print_main(pos_idx, play_idx, &plays[play_idx]);
          }
          positions_with_main++;
          // Oracle: the picks' finished games, where the variants' picks by
          // utility differ.
          int picks[SIMH_NUM_MEASURES][SIM_LAB_NUM_VARIANTS];
          simh_get_picks(plays, number_of_main_plays, picks);
          simh_print_picks(pos_idx, picks);
          bool is_picked[SIM_LAB_MAX_PLAYS] = {false};
          int number_picked = 0;
          for (int variant = 0; variant < SIM_LAB_NUM_VARIANTS; variant++) {
            const int pick = picks[SIMH_MEASURE_UTILITY][variant];
            if (!is_picked[pick]) {
              is_picked[pick] = true;
              number_picked++;
            }
          }
          if (number_picked > 1 && oracle_rollouts > 0) {
            ctimer_start(&position_timer);
            StringBuilder *picked_moves = string_builder_create();
            for (int play_idx = 0; play_idx < number_of_main_plays;
                 play_idx++) {
              if (!is_picked[play_idx]) {
                continue;
              }
              if (string_builder_length(picked_moves) > 0) {
                string_builder_add_char(picked_moves, ',');
              }
              string_builder_add_string(picked_moves, plays[play_idx].move);
            }
            simh_lab_reset(lab, 1);
            char *oracle_settings = get_formatted_string(
                "set -iterations %d -minplayiterations %d -seed %d",
                number_picked * oracle_rollouts, oracle_rollouts,
                SIMH_ORACLE_SEED + pos_idx);
            load_and_exec_config_or_die(config, oracle_settings);
            free(oracle_settings);
            load_and_exec_config_or_die(config, cgp_command);
            char *add_picks_command = get_formatted_string(
                "addmoves %s", string_builder_peek(picked_moves));
            load_and_exec_config_or_die(config, add_picks_command);
            free(add_picks_command);
            string_builder_destroy(picked_moves);
            load_and_exec_config_or_die(config, "sim");
            const int number_of_oracle_plays =
                simh_collect(config, lab, oracle_plays, &mean_mismatches);
            for (int play_idx = 0; play_idx < number_of_oracle_plays;
                 play_idx++) {
              simh_print_oracle(pos_idx, play_idx, &oracle_plays[play_idx]);
            }
            oracle_seconds = ctimer_elapsed_seconds(&position_timer);
            positions_with_oracle++;
          }
        }
        string_builder_destroy(kept_moves);
        printf("SIMH time pos=%d screen_secs=%.3f main_secs=%.3f "
               "oracle_secs=%.3f\n",
               pos_idx, screen_seconds, main_seconds, oracle_seconds);
        (void)fflush(stdout);
        free(cgp_command);
        pos_idx++;
        positions++;
      }
      play_top_n_equity_move(game, 0);
    }
  }
  SimLabChecks checks;
  memset(&checks, 0, sizeof(checks));
  for (int worker = 0; worker < SIM_LAB_MAX_WORKERS; worker++) {
    const SimLabChecks *worker_checks = &lab->checks[worker];
    checks.placements += worker_checks->placements;
    checks.placement_mismatches += worker_checks->placement_mismatches;
    checks.other_plays += worker_checks->other_plays;
    checks.other_play_mismatches += worker_checks->other_play_mismatches;
    checks.horizons += worker_checks->horizons;
    checks.horizon_mismatches += worker_checks->horizon_mismatches;
    checks.positive_pat_terms += worker_checks->positive_pat_terms;
  }
  printf("SIMH checks placements=%llu placement_mismatches=%llu "
         "other_plays=%llu other_play_mismatches=%llu horizons=%llu "
         "horizon_mismatches=%llu positive_pat_terms=%llu "
         "mean_mismatches=%d\n",
         (unsigned long long)checks.placements,
         (unsigned long long)checks.placement_mismatches,
         (unsigned long long)checks.other_plays,
         (unsigned long long)checks.other_play_mismatches,
         (unsigned long long)checks.horizons,
         (unsigned long long)checks.horizon_mismatches,
         (unsigned long long)checks.positive_pat_terms, mean_mismatches);
  printf("SIMH done positions=%d with_main=%d with_oracle=%d seconds=%.0f\n",
         positions, positions_with_main, positions_with_oracle,
         ctimer_elapsed_seconds(&total_timer));
  rv_sim_lab_set(NULL);
  free(rest_plays);
  free(oracle_plays);
  free(plays);
  free(lab);
  game_destroy(game);
  config_destroy(config);
}

void sim_horizon_run_spec(const char *spec) {
  int first_game = 0;
  int num_games = 0;
  int plies = 0;
  int screen_rollouts = 0;
  int main_rollouts = 0;
  int continue_one_in = 0;
  int oracle_rollouts = 0;
  int mid_rollouts = 0;
  const int fields_read =
      sscanf(spec,
             "first=%d,games=%d,plies=%d,screen=%d,main=%d,"
             "cont=%d,oracle=%d,mid=%d",
             &first_game, &num_games, &plies, &screen_rollouts, &main_rollouts,
             &continue_one_in, &oracle_rollouts, &mid_rollouts);
  if (fields_read != SIMH_SPEC_FIELDS &&
      fields_read != SIMH_SPEC_FIELDS_WITH_MID) {
    log_fatal("sim horizon: bad spec: %s", spec);
  }
  simh_run(first_game, num_games, plies, screen_rollouts, main_rollouts,
           continue_one_in, oracle_rollouts, mid_rollouts);
}
