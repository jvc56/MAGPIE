// What the exact leave values at the root of a sim are worth (see
// pool_shift.h): LOCAL MEASUREMENT HARNESS.
//
// Positions come from static self-play: every position whose mover holds a
// full rack with at least a rack in the bag. Each is simmed with the pool
// shift (-poolshift 1.2) and with the exact leave values (-poolshift exact:
// exact candidates, and rollouts on the leaves of the position's pool), at
// each of SIMX_NUM_TIERS iteration budgets with the same seed. Where the two
// sims of a budget choose different plays, two oracles give every chosen play
// the same large number of deeper rollouts: one rolls out as the first kind
// of sim does, the other as the second.
//
// Lines printed:
//   SIMX pos ...     one per position
//   SIMX pick ...    the play a sim chose at a budget
//   SIMX oracle ...  an oracle's value of a chosen play

#include "sim_exact_test.h"

#include "../src/compat/ctime.h"
#include "../src/compat/memory_info.h"
#include "../src/def/rack_defs.h"
#include "../src/ent/game.h"
#include "../src/ent/move.h"
#include "../src/ent/player.h"
#include "../src/ent/pool_shift.h"
#include "../src/ent/rack.h"
#include "../src/ent/sim_results.h"
#include "../src/ent/stats.h"
#include "../src/impl/cgp.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
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
  SIMX_NUM_PLAYS = 15,
  SIMX_MAX_UNION = 2 * SIMX_NUM_PLAYS,
  SIMX_MAX_MOVE_STRING = 64,
  SIMX_NUM_TIERS = 1,
  SIMX_ORACLE_ITERS_PER_PLAY = 24000,
  SIMX_PLIES = 2,
  SIMX_ORACLE_PLIES = 4,
  SIMX_MAX_PICKS = 2 * SIMX_NUM_TIERS,
  SIMX_ORACLE_SEED = 1000003,
  SIMX_POSITIONS_PER_GAME = 100,
  SIMX_DEFAULT_GAMES = 42,
  SIMX_SPEC_FIELDS = 3,
  SIMX_FIRST_GAME_SEED = 5000,
};

static const int simx_tiers[SIMX_NUM_TIERS] = {128000};

typedef struct SimxList {
  int count;
  char moves[SIMX_MAX_UNION][SIMX_MAX_MOVE_STRING];
} SimxList;

static void simx_move_string(const Config *config, const Move *move,
                             char *out) {
  StringBuilder *sb = string_builder_create();
  const Game *game = config_get_game(config);
  string_builder_add_ucgi_move(sb, move, game_get_board(game),
                               game_get_ld(game));
  (void)snprintf(out, SIMX_MAX_MOVE_STRING, "%s", string_builder_peek(sb));
  string_builder_destroy(sb);
}

static bool simx_list_has(const SimxList *list, const char *move) {
  for (int idx = 0; idx < list->count; idx++) {
    if (strings_equal(list->moves[idx], move)) {
      return true;
    }
  }
  return false;
}

static Config *simx_config(const char *pool_shift, int plies,
                           const char *extra) {
  char *settings = get_formatted_string(
      "set -lex CSW24 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays %d -plies %d -threads %d -scond none -tlim 0 "
      "-savesettings false -poolshift %s %s",
      SIMX_NUM_PLAYS, plies, get_num_cores(), pool_shift, extra);
  Config *config = config_create_or_die(settings);
  free(settings);
  return config;
}

// The mover's rack alone: the opponent's is not known to a sim.
static char *simx_cgp_command(const Game *game) {
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

// Sims the position's candidates and prints the play chosen, which it also
// copies to pick.
static void simx_sim_and_print_pick(Config *config, const char *mode,
                                    const char *cgp_command, int pos_idx,
                                    int iterations, char *pick) {
  char *settings = get_formatted_string("set -iterations %d -seed %d",
                                        iterations, pos_idx * 7 + 17);
  load_and_exec_config_or_die(config, settings);
  free(settings);
  load_and_exec_config_or_die(config, cgp_command);
  Timer timer;
  ctimer_start(&timer);
  load_and_exec_config_or_die(config, "gen");
  const double gen_seconds = ctimer_elapsed_seconds(&timer);
  ctimer_start(&timer);
  load_and_exec_config_or_die(config, "sim");
  const double seconds = ctimer_elapsed_seconds(&timer);
  const SimResults *sim_results = config_get_sim_results(config);
  simx_move_string(config, sim_results_get_best_move(sim_results), pick);
  printf("SIMX pick pos=%d n=%d mode=%s move=%s iters=%llu gen_secs=%.4f "
         "secs=%.4f\n",
         pos_idx, iterations, mode, pick,
         (unsigned long long)sim_results_get_iteration_count(sim_results),
         gen_seconds, seconds);
}

// Gives each of the plays the same number of rollouts and prints its values.
static void simx_oracle(Config *config, const char *oracle,
                        const char *cgp_command, int pos_idx,
                        const SimxList *plays) {
  StringBuilder *moves = string_builder_create();
  for (int idx = 0; idx < plays->count; idx++) {
    if (idx > 0) {
      string_builder_add_char(moves, ',');
    }
    string_builder_add_string(moves, plays->moves[idx]);
  }
  char *settings = get_formatted_string(
      "set -iterations %d -minplayiterations %d -seed %d",
      plays->count * SIMX_ORACLE_ITERS_PER_PLAY, SIMX_ORACLE_ITERS_PER_PLAY,
      SIMX_ORACLE_SEED + pos_idx);
  load_and_exec_config_or_die(config, settings);
  free(settings);
  load_and_exec_config_or_die(config, cgp_command);
  char *add_command =
      get_formatted_string("addmoves %s", string_builder_peek(moves));
  load_and_exec_config_or_die(config, add_command);
  free(add_command);
  string_builder_destroy(moves);
  load_and_exec_config_or_die(config, "sim");
  const SimResults *sim_results = config_get_sim_results(config);
  const int simmed = sim_results_get_number_of_plays(sim_results);
  for (int play_idx = 0; play_idx < simmed; play_idx++) {
    const SimmedPlay *simmed_play =
        sim_results_get_simmed_play(sim_results, play_idx);
    char move[SIMX_MAX_MOVE_STRING];
    simx_move_string(config, simmed_play_get_move(simmed_play), move);
    const Stat *equity_stat = simmed_play_get_equity_stat(simmed_play);
    const Stat *utility_stat = simmed_play_get_utility_stat(simmed_play);
    const double samples = (double)stat_get_num_samples(equity_stat);
    const double root = samples > 1 ? sqrt(samples) : 1.0;
    printf("SIMX oracle pos=%d oracle=%s move=%s eq=%.6f eq_se=%.6f wp=%.6f "
           "util=%.6f util_se=%.6f n=%.0f\n",
           pos_idx, oracle, move,
           simmed_play_get_exact_equity_mean(simmed_play),
           stat_get_stdev(equity_stat) / root,
           simmed_play_get_exact_win_pct_mean(simmed_play),
           simmed_play_get_exact_utility_mean(simmed_play),
           stat_get_stdev(utility_stat) / root, samples);
  }
}

// The play static equity alone would choose, as the pool shift values it: a
// control whose plays the sims' should beat.
static void simx_print_static_pick(Config *config, const char *cgp_command,
                                   int pos_idx) {
  load_and_exec_config_or_die(config, cgp_command);
  load_and_exec_config_or_die(config, "gen");
  const MoveList *move_list = config_get_move_list(config);
  const Move *best = NULL;
  for (int idx = 0; idx < move_list_get_count(move_list); idx++) {
    const Move *move = move_list_get_move(move_list, idx);
    if (best == NULL || move_get_equity(move) > move_get_equity(best)) {
      best = move;
    }
  }
  char pick[SIMX_MAX_MOVE_STRING];
  simx_move_string(config, best, pick);
  printf("SIMX pick pos=%d n=%d mode=static move=%s iters=0 gen_secs=0 "
         "secs=0\n",
         pos_idx, simx_tiers[0], pick);
}

static void simx_run(int first_game, int num_games, bool with_static) {
  Config *shift_config = simx_config("1.2", SIMX_PLIES, "");
  Config *exact_config = simx_config("exact", SIMX_PLIES, "");
  Config *shift_oracle = simx_config("1.2", SIMX_ORACLE_PLIES, "-sr rr");
  Config *exact_oracle = simx_config("exact", SIMX_ORACLE_PLIES, "-sr rr");
  Game *game = config_game_create(shift_config);
  Timer total_timer;
  ctimer_start(&total_timer);
  int positions = 0;
  int differing = 0;
  for (int game_idx = first_game; game_idx < first_game + num_games;
       game_idx++) {
    game_reset(game);
    game_seed(game, (uint64_t)(SIMX_FIRST_GAME_SEED + game_idx));
    draw_starting_racks(game);
    int pos_idx = game_idx * SIMX_POSITIONS_PER_GAME;
    while (!game_over(game)) {
      const Rack *rack = player_get_rack(
          game_get_player(game, game_get_player_on_turn_index(game)));
      if (rack_get_total_letters(rack) == RACK_SIZE &&
          game_get_real_bag_size(game) >= RACK_SIZE) {
        char *cgp_command = simx_cgp_command(game);
        printf("SIMX pos idx=%d game=%d bag=%d %s\n", pos_idx, game_idx,
               game_get_real_bag_size(game), cgp_command);
        SimxList picks = {.count = 0};
        for (int tier_idx = 0; tier_idx < SIMX_NUM_TIERS; tier_idx++) {
          char shift_pick[SIMX_MAX_MOVE_STRING];
          char exact_pick[SIMX_MAX_MOVE_STRING];
          simx_sim_and_print_pick(shift_config, "shift", cgp_command, pos_idx,
                                  simx_tiers[tier_idx], shift_pick);
          simx_sim_and_print_pick(exact_config, "exact", cgp_command, pos_idx,
                                  simx_tiers[tier_idx], exact_pick);
          if (!simx_list_has(&picks, shift_pick)) {
            (void)snprintf(picks.moves[picks.count++], SIMX_MAX_MOVE_STRING,
                           "%s", shift_pick);
          }
          if (!simx_list_has(&picks, exact_pick)) {
            (void)snprintf(picks.moves[picks.count++], SIMX_MAX_MOVE_STRING,
                           "%s", exact_pick);
          }
        }
        // Any two plays chosen: the budgets' own disagreements say what more
        // iterations are worth.
        if (picks.count > 1) {
          simx_oracle(shift_oracle, "shift", cgp_command, pos_idx, &picks);
          simx_oracle(exact_oracle, "exact", cgp_command, pos_idx, &picks);
          differing++;
        }
        if (with_static) {
          simx_print_static_pick(shift_config, cgp_command, pos_idx);
        }
        (void)fflush(stdout);
        free(cgp_command);
        pos_idx++;
        positions++;
      }
      play_top_n_equity_move(game, 0);
    }
  }
  printf("SIMX done positions=%d differing=%d seconds=%.0f\n", positions,
         differing, ctimer_elapsed_seconds(&total_timer));
  game_destroy(game);
  config_destroy(exact_oracle);
  config_destroy(shift_oracle);
  config_destroy(exact_config);
  config_destroy(shift_config);
}

void test_sim_exact(void) { simx_run(0, SIMX_DEFAULT_GAMES, false); }

// Spec: first=<game>,games=<n>,static=<0 or 1>
void sim_exact_run_spec(const char *spec) {
  int first_game = 0;
  int num_games = 0;
  int with_static = 0;
  if (sscanf(spec, "first=%d,games=%d,static=%d", &first_game, &num_games,
             &with_static) != SIMX_SPEC_FIELDS) {
    log_fatal("sim exact: bad spec: %s", spec);
  }
  simx_run(first_game, num_games, with_static != 0);
}

// The nested oracle: values a play by playing it and then letting sims, not
// static equity, choose the next SIMN_NESTED_PLIES plays, with a last sim
// valuing the position reached. Every sample draws the opponent's rack before
// the play, from a seed shared by the plays of a position.
//
// Spec: tasks=<file>,mode=<pool shift>,slice=<i>,of=<n>,samples=<m>,iters=<k>
// A task line is: pos_idx <tab> cgp command <tab> play <tab> play
// Lines printed:
//   NEST pos= pick= sample= value= wp= plies=

// LOCAL MEASUREMENT HOOK in random_variable.c.
extern PoolLeaves *simmer_pool_leaves_override;

enum {
  SIMN_NESTED_PLIES = 2,
  SIMN_MAX_LINE = 2048,
  SIMN_MAX_PATH = 1024,
  SIMN_MAX_MODE = 16,
  SIMN_NUM_PICKS = 2,
  SIMN_SPEC_FIELDS = 6,
  SIMN_VALUE_ITERATIONS = 1500,
  SIMN_VALUE_SEED_OFFSET = 500,
};

static double simn_spread(const Game *game, int player_index) {
  return equity_to_double(
      player_get_score(game_get_player(game, player_index)) -
      player_get_score(game_get_player(game, 1 - player_index)));
}

static const Move *simn_find_move(const Config *config, const char *wanted) {
  const MoveList *move_list = config_get_move_list(config);
  for (int idx = 0; idx < move_list_get_count(move_list); idx++) {
    const Move *move = move_list_get_move(move_list, idx);
    char name[SIMX_MAX_MOVE_STRING];
    simx_move_string(config, move, name);
    if (strings_equal(name, wanted)) {
      return move;
    }
  }
  log_fatal("nested oracle: play not found: %s", wanted);
  return NULL;
}

static void simn_sample(Config *sim_config, const Game *root, const Move *pick,
                        Game *truth, int pos_idx, int pick_idx, int sample_idx,
                        int iterations) {
  game_copy(truth, root);
  const int mover_index = game_get_player_on_turn_index(truth);
  const uint64_t seed =
      (uint64_t)pos_idx * 1000003 + (uint64_t)sample_idx * 7919 + 11;
  game_seed(truth, seed);
  draw_to_full_rack(truth, 1 - mover_index);
  play_move(pick, truth, NULL);
  double value = 0;
  double win_pct = -1;
  bool valued = false;
  int plies = 0;
  for (; plies <= SIMN_NESTED_PLIES && !game_over(truth); plies++) {
    char *cgp_command = simx_cgp_command(truth);
    char *settings = get_formatted_string(
        "set -seed %llu -iterations %d",
        (unsigned long long)(seed + (uint64_t)plies + 1), iterations);
    load_and_exec_config_or_die(sim_config, settings);
    free(settings);
    load_and_exec_config_or_die(sim_config, cgp_command);
    free(cgp_command);
    load_and_exec_config_or_die(sim_config, "gen");
    load_and_exec_config_or_die(sim_config, "sim");
    const SimResults *sim_results = config_get_sim_results(sim_config);
    if (plies < SIMN_NESTED_PLIES) {
      play_move(sim_results_get_best_move(sim_results), truth, NULL);
      continue;
    }
    // The last sim chooses the play; a sim of that play alone, from another
    // seed, values it, so that the value is not the best of noisy ones.
    char best_name[SIMX_MAX_MOVE_STRING];
    simx_move_string(sim_config, sim_results_get_best_move(sim_results),
                     best_name);
    char *value_settings = get_formatted_string(
        "set -seed %llu -iterations %d",
        (unsigned long long)(seed + SIMN_VALUE_SEED_OFFSET),
        SIMN_VALUE_ITERATIONS);
    load_and_exec_config_or_die(sim_config, value_settings);
    free(value_settings);
    char *value_cgp = simx_cgp_command(truth);
    load_and_exec_config_or_die(sim_config, value_cgp);
    free(value_cgp);
    char *add_command = get_formatted_string("addmoves %s", best_name);
    load_and_exec_config_or_die(sim_config, add_command);
    free(add_command);
    load_and_exec_config_or_die(sim_config, "sim");
    sim_results = config_get_sim_results(sim_config);
    const SimmedPlay *best = sim_results_get_simmed_play(sim_results, 0);
    const int on_turn_index = game_get_player_on_turn_index(truth);
    const double sign = on_turn_index == mover_index ? 1.0 : -1.0;
    value = simn_spread(truth, mover_index) +
            sign * simmed_play_get_exact_equity_mean(best);
    const double on_turn_win_pct = simmed_play_get_exact_win_pct_mean(best);
    win_pct =
        on_turn_index == mover_index ? on_turn_win_pct : 1.0 - on_turn_win_pct;
    valued = true;
  }
  if (!valued) {
    value = simn_spread(truth, mover_index);
    if (value > 0) {
      win_pct = 1.0;
    } else if (value < 0) {
      win_pct = 0.0;
    } else {
      win_pct = 0.5;
    }
  }
  printf("NEST pos=%d pick=%d sample=%d value=%.4f wp=%.6f plies=%d\n", pos_idx,
         pick_idx, sample_idx, value, win_pct, plies);
}

void sim_exact_nested_run_spec(const char *spec) {
  char tasks_path[SIMN_MAX_PATH];
  char mode[SIMN_MAX_MODE];
  int slice = 0;
  int num_slices = 1;
  int samples = 0;
  int iterations = 0;
  if (sscanf(spec,
             "tasks=%1023[^,],mode=%15[^,],slice=%d,of=%d,samples=%d,iters=%d",
             tasks_path, mode, &slice, &num_slices, &samples,
             &iterations) != SIMN_SPEC_FIELDS) {
    log_fatal("nested oracle: bad spec: %s", spec);
  }
  // mode exactroot: the inner sims roll out on the leaves of the root
  // position's pool, shifted from that pool, instead of building their own.
  const bool reuse_root_leaves = strings_equal(mode, "exactroot");
  char *settings = get_formatted_string(
      "set -lex CSW24 -wmp true -s1 equity -s2 equity -r1 all -r2 all "
      "-numplays %d -plies %d -threads 1 -scond none -tlim 0 "
      "-savesettings false -iterations %d -poolshift %s",
      SIMX_NUM_PLAYS, SIMX_PLIES, iterations,
      reuse_root_leaves ? "exact" : mode);
  Config *sim_config = config_create_or_die(settings);
  Config *root_config = config_create_or_die(settings);
  free(settings);
  FILE *tasks = fopen(tasks_path, "r");
  if (tasks == NULL) {
    log_fatal("nested oracle: cannot open %s", tasks_path);
  }
  char line[SIMN_MAX_LINE];
  Game *root = NULL;
  Game *truth = NULL;
  Timer timer;
  ctimer_start(&timer);
  for (int task_idx = 0; fgets(line, sizeof(line), tasks) != NULL; task_idx++) {
    if (task_idx % num_slices != slice) {
      continue;
    }
    line[strcspn(line, "\n")] = '\0';
    char *fields[2 + SIMN_NUM_PICKS];
    char *cursor = line;
    for (int field_idx = 0; field_idx < 2 + SIMN_NUM_PICKS; field_idx++) {
      fields[field_idx] = cursor;
      char *tab = strchr(cursor, '\t');
      if (tab != NULL) {
        *tab = '\0';
        cursor = tab + 1;
      }
    }
    const int pos_idx = atoi(fields[0]);
    load_and_exec_config_or_die(root_config, fields[1]);
    char *add_command =
        get_formatted_string("addmoves %s,%s", fields[2], fields[3]);
    load_and_exec_config_or_die(root_config, add_command);
    free(add_command);
    if (root == NULL) {
      root = game_duplicate(config_get_game(root_config));
      truth = game_duplicate(root);
    } else {
      game_copy(root, config_get_game(root_config));
    }
    if (reuse_root_leaves) {
      pool_leaves_destroy(simmer_pool_leaves_override);
      const int root_mover_index = game_get_player_on_turn_index(root);
      simmer_pool_leaves_override = pool_leaves_create(
          player_get_pool_shift_table(game_get_player(root, root_mover_index)),
          game_get_ld(root), game_get_bag(root),
          player_get_rack(game_get_player(root, 1 - root_mover_index)),
          player_get_rack(game_get_player(root, root_mover_index)));
    }
    for (int sample_idx = 0; sample_idx < samples; sample_idx++) {
      for (int pick_idx = 0; pick_idx < SIMN_NUM_PICKS; pick_idx++) {
        const Move *pick = simn_find_move(root_config, fields[2 + pick_idx]);
        simn_sample(sim_config, root, pick, truth, pos_idx, pick_idx,
                    sample_idx, iterations);
      }
      (void)fflush(stdout);
    }
  }
  printf("NEST done seconds=%.0f\n", ctimer_elapsed_seconds(&timer));
  (void)fclose(tasks);
  game_destroy(truth);
  game_destroy(root);
  config_destroy(root_config);
  config_destroy(sim_config);
  pool_leaves_destroy(simmer_pool_leaves_override);
  simmer_pool_leaves_override = NULL;
}
