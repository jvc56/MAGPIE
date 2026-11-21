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
#include "../src/def/sim_defs.h"
#include "../src/ent/bag.h"
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

// The position as a cgp command. The opponent's rack is not known to a sim
// that chooses a play, so it is left out unless both_racks is set.
static char *simx_cgp_command_with_racks(const Game *game, bool both_racks) {
  char *cgp = game_get_cgp(game, true);
  if (both_racks) {
    char *command = get_formatted_string("cgp %s", cgp);
    free(cgp);
    return command;
  }
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

static char *simx_cgp_command(const Game *game) {
  return simx_cgp_command_with_racks(game, false);
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
// with an optional ,endbag=<b>: a sim of a position with b tiles in the bag
// or fewer, choosing or valuing, then rolls out to the end of the game (the
// most plies a sim allows), so that no leave value stands in for tiles that
// can no longer be drawn or for a game that has already ended. With the bag
// empty both racks are known and every rollout is the same, so one is run.
// A task line is: pos_idx <tab> cgp command <tab> play <tab> play
// Lines printed:
//   NEST pos= pick= sample= value= wp= plies= util= vbag=
// vbag is the number of tiles in the bag where the value was taken, or -1
// when the game ended first.

// LOCAL MEASUREMENT HOOK in random_variable.c.
extern PoolLeaves *simmer_pool_leaves_override;

enum {
  SIMN_NESTED_PLIES = 2,
  SIMN_MAX_LINE = 2048,
  SIMN_MAX_PATH = 1024,
  SIMN_MAX_MODE = 16,
  SIMN_NUM_PICKS = 2,
  SIMN_SPEC_FIELDS = 6,
  SIMN_SPEC_FIELDS_WITH_END_BAG = 7,
  SIMN_VALUE_ITERATIONS = 1500,
  SIMN_VALUE_SEED_OFFSET = 500,
  SIMN_NO_END_BAG = -1,
};

// The endbag of the spec, or SIMN_NO_END_BAG.
static int simn_end_bag = SIMN_NO_END_BAG;

static bool simn_rolls_out_to_end(int bag_tiles) {
  return simn_end_bag != SIMN_NO_END_BAG && bag_tiles <= simn_end_bag;
}

static int simn_sim_plies(int bag_tiles) {
  return simn_rolls_out_to_end(bag_tiles) ? MAX_PLIES : SIMX_PLIES;
}

static int simn_sim_iterations(int bag_tiles, int iterations) {
  if (simn_rolls_out_to_end(bag_tiles) && bag_tiles == 0) {
    return 1;
  }
  return iterations;
}

// The win % weight and the spread weight of the blended utility, summed: a
// utility of u to one player is this less u to the other.
static const double SIMN_UTILITY_TOTAL_WEIGHT = 1.5;

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
  double utility = -1;
  bool valued = false;
  int value_bag_tiles = -1;
  int plies = 0;
  for (; plies <= SIMN_NESTED_PLIES && !game_over(truth); plies++) {
    const int bag_tiles = bag_get_letters(game_get_bag(truth));
    // With the bag empty, each player can deduce the other's rack.
    const bool bag_is_empty = bag_tiles == 0;
    char *cgp_command = simx_cgp_command_with_racks(truth, bag_is_empty);
    char *settings = get_formatted_string(
        "set -seed %llu -iterations %d -plies %d",
        (unsigned long long)(seed + (uint64_t)plies + 1),
        simn_sim_iterations(bag_tiles, iterations), simn_sim_plies(bag_tiles));
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
    // seed, values it, so that the value is not the best of noisy ones. The
    // valuing sim is given both racks, so that the tiles the other player
    // kept on their last play count for what they are.
    char best_name[SIMX_MAX_MOVE_STRING];
    simx_move_string(sim_config, sim_results_get_best_move(sim_results),
                     best_name);
    char *value_settings = get_formatted_string(
        "set -seed %llu -iterations %d -plies %d",
        (unsigned long long)(seed + SIMN_VALUE_SEED_OFFSET),
        simn_sim_iterations(bag_tiles, SIMN_VALUE_ITERATIONS),
        simn_sim_plies(bag_tiles));
    value_bag_tiles = bag_tiles;
    load_and_exec_config_or_die(sim_config, value_settings);
    free(value_settings);
    char *value_cgp = simx_cgp_command_with_racks(truth, true);
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
    const double on_turn_utility = simmed_play_get_exact_utility_mean(best);
    utility = on_turn_index == mover_index
                  ? on_turn_utility
                  : SIMN_UTILITY_TOTAL_WEIGHT - on_turn_utility;
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
  printf("NEST pos=%d pick=%d sample=%d value=%.4f wp=%.6f plies=%d "
         "util=%.6f vbag=%d\n",
         pos_idx, pick_idx, sample_idx, value, win_pct, plies, utility,
         value_bag_tiles);
}

void sim_exact_nested_run_spec(const char *spec) {
  char tasks_path[SIMN_MAX_PATH];
  char mode[SIMN_MAX_MODE];
  int slice = 0;
  int num_slices = 1;
  int samples = 0;
  int iterations = 0;
  int end_bag = SIMN_NO_END_BAG;
  const int num_fields = sscanf(
      spec,
      "tasks=%1023[^,],mode=%15[^,],slice=%d,of=%d,samples=%d,iters=%d,"
      "endbag=%d",
      tasks_path, mode, &slice, &num_slices, &samples, &iterations, &end_bag);
  if ((num_fields != SIMN_SPEC_FIELDS &&
       num_fields != SIMN_SPEC_FIELDS_WITH_END_BAG) ||
      (num_fields == SIMN_SPEC_FIELDS_WITH_END_BAG && end_bag < 0)) {
    log_fatal("nested oracle: bad spec: %s", spec);
  }
  simn_end_bag = end_bag;
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

// Leave accuracy: for the mover of each eligible position of static self-play,
// the error of each model's value of every leave of the rack against the
// exact value for the pool the mover sees, less the rack's mean error (a
// constant across a rack's leaves changes no choice).
//
// Spec: games=<n>
extern bool pool_leaves_fill_exhausted;

enum {
  LEAFDIAG_MODELS = 4,
  LEAFDIAG_SUBSETS = 1 << RACK_SIZE,
};

typedef struct LeafDiagStat {
  double sum_error[LEAFDIAG_MODELS];
  double sum_squared[LEAFDIAG_MODELS];
  double count;
} LeafDiagStat;

static void leafdiag_print(const char *label, const LeafDiagStat *stat) {
  if (stat->count == 0) {
    return;
  }
  printf("LEAF %s n=%.0f", label, stat->count);
  static const char *names[LEAFDIAG_MODELS] = {"pool", "poolfix", "poolnoshift",
                                               "stock1.2"};
  for (int model_idx = 0; model_idx < LEAFDIAG_MODELS; model_idx++) {
    printf(" %s bias=%+.3f rmse=%.3f", names[model_idx],
           stat->sum_error[model_idx] / stat->count,
           sqrt(stat->sum_squared[model_idx] / stat->count));
  }
  printf("\n");
}

void sim_exact_leaf_diag_run_spec(const char *spec) {
  int num_games = 0;
  if (sscanf(spec, "games=%d", &num_games) != 1) {
    log_fatal("leaf diag: bad spec: %s", spec);
  }
  Config *config = simx_config("exact", SIMX_PLIES, "");
  Game *game = config_game_create(config);
  const LetterDistribution *ld = game_get_ld(game);
  // By leave size, then leaves with no exhausted letter and with one.
  LeafDiagStat stats[RACK_SIZE][2];
  memset(stats, 0, sizeof(stats));
  LeafDiagStat low_bag[2];
  memset(low_bag, 0, sizeof(low_bag));
  for (int game_idx = 0; game_idx < num_games; game_idx++) {
    game_reset(game);
    game_seed(game, (uint64_t)(SIMX_FIRST_GAME_SEED + game_idx));
    draw_starting_racks(game);
    while (!game_over(game)) {
      const int mover_index = game_get_player_on_turn_index(game);
      const Player *mover = game_get_player(game, mover_index);
      const Rack *rack = player_get_rack(mover);
      const Rack *opp_rack =
          player_get_rack(game_get_player(game, 1 - mover_index));
      const int bag_size = game_get_real_bag_size(game);
      if (rack_get_total_letters(rack) == RACK_SIZE && bag_size >= RACK_SIZE) {
        const KLV *klv = player_get_klv(mover);
        const PoolShiftTable *table = player_get_pool_shift_table(mover);
        const Bag *bag = game_get_bag(game);
        pool_leaves_fill_exhausted = false;
        PoolLeaves *pool = pool_leaves_create(table, ld, bag, opp_rack, rack);
        pool_leaves_fill_exhausted = true;
        PoolLeaves *fixed = pool_leaves_create(table, ld, bag, opp_rack, rack);
        MachineLetter tiles[RACK_SIZE];
        int num_tiles = 0;
        for (int ml = 0; ml < ld_get_size(ld); ml++) {
          for (int copy = 0; copy < rack_get_letter(rack, (MachineLetter)ml);
               copy++) {
            tiles[num_tiles++] = (MachineLetter)ml;
          }
        }
        double errors[LEAFDIAG_SUBSETS][LEAFDIAG_MODELS];
        int sizes[LEAFDIAG_SUBSETS];
        int exhausted[LEAFDIAG_SUBSETS];
        double means[LEAFDIAG_MODELS] = {0};
        for (int mask = 0; mask < LEAFDIAG_SUBSETS - 1; mask++) {
          Rack leave;
          rack_set_dist_size_and_reset(&leave, ld_get_size(ld));
          for (int bit = 0; bit < RACK_SIZE; bit++) {
            if (mask & (1 << bit)) {
              rack_add_letter(&leave, tiles[bit]);
            }
          }
          sizes[mask] = rack_get_total_letters(&leave);
          exhausted[mask] = 0;
          for (int ml = 0; ml < ld_get_size(ld); ml++) {
            if (rack_get_letter(&leave, (MachineLetter)ml) >=
                ld_get_dist(ld, (MachineLetter)ml)) {
              exhausted[mask] = 1;
            }
          }
          const double exact = equity_to_double(
              klv_get_leave_value(klv, &leave) +
              pool_shift_get_leave_shift(
                  table, klv, POOL_SHIFT_EXACT_WEIGHT_MILLI, ld, bag, opp_rack,
                  &leave, RACK_SIZE, bag_size));
          const PoolLeaves *pools[2] = {pool, fixed};
          double values[LEAFDIAG_MODELS];
          for (int pool_idx = 0; pool_idx < 2; pool_idx++) {
            const PoolLeaves *leaves = pools[pool_idx];
            values[pool_idx] =
                equity_to_double(klv_get_leave_value(leaves->klv, &leave) +
                                 leaves->table->reference_exact_offset +
                                 pool_shift_get_leave_shift(
                                     leaves->table, leaves->klv, 1000, ld, bag,
                                     opp_rack, &leave, RACK_SIZE, bag_size));
          }
          values[2] = equity_to_double(klv_get_leave_value(fixed->klv, &leave) +
                                       fixed->table->reference_exact_offset);
          values[3] = equity_to_double(
              klv_get_leave_value(klv, &leave) +
              pool_shift_get_leave_shift(table, klv, 1200, ld, bag, opp_rack,
                                         &leave, RACK_SIZE, bag_size));
          for (int model_idx = 0; model_idx < LEAFDIAG_MODELS; model_idx++) {
            errors[mask][model_idx] = values[model_idx] - exact;
            means[model_idx] +=
                errors[mask][model_idx] / (double)(LEAFDIAG_SUBSETS - 1);
          }
        }
        for (int mask = 0; mask < LEAFDIAG_SUBSETS - 1; mask++) {
          LeafDiagStat *targets[2] = {&stats[sizes[mask]][exhausted[mask]],
                                      &low_bag[bag_size <= 20 ? 0 : 1]};
          for (int target_idx = 0; target_idx < 2; target_idx++) {
            LeafDiagStat *stat = targets[target_idx];
            stat->count += 1;
            for (int model_idx = 0; model_idx < LEAFDIAG_MODELS; model_idx++) {
              const double centred = errors[mask][model_idx] - means[model_idx];
              stat->sum_error[model_idx] += centred;
              stat->sum_squared[model_idx] += centred * centred;
            }
          }
        }
        pool_leaves_destroy(pool);
        pool_leaves_destroy(fixed);
      }
      play_top_n_equity_move(game, 0);
    }
  }
  for (int size = 0; size < RACK_SIZE; size++) {
    for (int has_exhausted = 0; has_exhausted < 2; has_exhausted++) {
      char label[SIMX_MAX_MOVE_STRING];
      (void)snprintf(label, sizeof(label), "size=%d exhausted=%d", size,
                     has_exhausted);
      leafdiag_print(label, &stats[size][has_exhausted]);
    }
  }
  leafdiag_print("bag<=20", &low_bag[0]);
  leafdiag_print("bag>20", &low_bag[1]);
  game_destroy(game);
  config_destroy(config);
}
