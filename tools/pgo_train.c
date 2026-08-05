#include "../src/def/config_defs.h"
#include "../src/def/peg_defs.h"
#include "../src/def/thread_control_defs.h"
#include "../src/ent/bag.h"
#include "../src/ent/game.h"
#include "../src/ent/move.h"
#include "../src/ent/thread_control.h"
#include "../src/ent/win_pct.h"
#include "../src/impl/config.h"
#include "../src/impl/gameplay.h"
#include "../src/impl/play_chooser.h"
#include "../src/util/io_util.h"
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
  PGO_MODE_GENERAL,
  PGO_MODE_STATIC,
  PGO_MODE_SIM,
  PGO_MODE_PEG,
  PGO_MODE_EG,
  PGO_MODE_LEAVEGEN,
} pgo_mode_t;

typedef struct {
  uint64_t static_moves;
  uint64_t sim_moves;
  uint64_t peg_moves;
  uint64_t endgame_moves;
} TrainingCounts;

static void exit_on_error(ErrorStack *error_stack, const char *operation) {
  if (error_stack_is_empty(error_stack)) {
    return;
  }
  (void)fprintf(stderr, "PGO training failed while %s:\n", operation);
  error_stack_print_and_reset(error_stack);
  exit(EXIT_FAILURE);
}

static int parse_positive_int(const char *value, const char *name) {
  errno = 0;
  char *end = NULL;
  const long parsed = strtol(value, &end, 10);
  if (errno != 0 || end == value || *end != '\0' || parsed < 1 ||
      parsed > INT_MAX) {
    (void)fprintf(stderr, "%s must be a positive integer, got '%s'\n", name,
                  value);
    exit(EXIT_FAILURE);
  }
  return (int)parsed;
}

static double parse_positive_double(const char *value, const char *name) {
  errno = 0;
  char *end = NULL;
  const double parsed = strtod(value, &end);
  if (errno != 0 || end == value || *end != '\0' || !(parsed > 0.0)) {
    (void)fprintf(stderr, "%s must be positive, got '%s'\n", name, value);
    exit(EXIT_FAILURE);
  }
  return parsed;
}

static pgo_mode_t parse_mode(const char *mode) {
  if (strcmp(mode, "general") == 0) {
    return PGO_MODE_GENERAL;
  }
  if (strcmp(mode, "static") == 0) {
    return PGO_MODE_STATIC;
  }
  if (strcmp(mode, "sim") == 0) {
    return PGO_MODE_SIM;
  }
  if (strcmp(mode, "peg") == 0) {
    return PGO_MODE_PEG;
  }
  if (strcmp(mode, "eg") == 0) {
    return PGO_MODE_EG;
  }
  if (strcmp(mode, "leavegen") == 0) {
    return PGO_MODE_LEAVEGEN;
  }
  (void)fprintf(stderr, "unknown PGO workload '%s'\n", mode);
  exit(EXIT_FAILURE);
}

static void execute_config_command(Config *config, const char *command,
                                   ErrorStack *error_stack) {
  thread_control_set_status(config_get_thread_control(config),
                            THREAD_CONTROL_STATUS_STARTED);
  config_load_command(config, command, error_stack);
  exit_on_error(error_stack, command);
  config_execute_command(config, error_stack);
  exit_on_error(error_stack, command);
  thread_control_set_status(config_get_thread_control(config),
                            THREAD_CONTROL_STATUS_FINISHED);
}

static Config *create_training_config(const char *data_paths, int num_threads,
                                      ErrorStack *error_stack) {
  const ConfigArgs args = {
      .data_paths = data_paths,
      .settings_filename = DEFAULT_SETTINGS_FILENAME,
      .use_wmp = true,
  };
  Config *config = config_create(&args, error_stack);
  exit_on_error(error_stack, "creating the engine configuration");

  char command[512];
  (void)snprintf(command, sizeof(command),
                 "set -lex CSW24 -wmp true -rit true -ritmmap true -wit true "
                 "-threads %d -s1 equity -s2 equity -r1 best -r2 best "
                 "-numplays 1 -pfrequency 0 -hr false -savesettings false "
                 "-autosavegcg false -fgrequired false",
                 num_threads);
  execute_config_command(config, command, error_stack);
  return config;
}

static void count_move_for_mode(pgo_mode_t mode, const Game *game,
                                TrainingCounts *counts) {
  const int bag_letters = bag_get_letters(game_get_bag(game));
  if (bag_letters == 0) {
    if (mode == PGO_MODE_EG) {
      counts->endgame_moves++;
    } else {
      counts->static_moves++;
    }
  } else if (mode == PGO_MODE_PEG && bag_letters <= PEG_MAX_BAG) {
    counts->peg_moves++;
  } else if (mode == PGO_MODE_SIM) {
    counts->sim_moves++;
  } else {
    counts->static_moves++;
  }
}

static void validate_training_counts(pgo_mode_t mode,
                                     const TrainingCounts *counts) {
  bool valid = true;
  switch (mode) {
  case PGO_MODE_GENERAL:
    break;
  case PGO_MODE_STATIC:
    valid = counts->static_moves > 0;
    break;
  case PGO_MODE_SIM:
    valid = counts->sim_moves > 0;
    break;
  case PGO_MODE_PEG:
    valid = counts->peg_moves > 0;
    break;
  case PGO_MODE_EG:
    valid = counts->endgame_moves > 0;
    break;
  case PGO_MODE_LEAVEGEN:
    break;
  }
  if (!valid) {
    (void)fprintf(stderr,
                  "workload did not reach every requested phase: static=%llu, "
                  "sim=%llu, peg=%llu, endgame=%llu\n",
                  (unsigned long long)counts->static_moves,
                  (unsigned long long)counts->sim_moves,
                  (unsigned long long)counts->peg_moves,
                  (unsigned long long)counts->endgame_moves);
    exit(EXIT_FAILURE);
  }
}

static void train_games(pgo_mode_t mode, const Config *config, int num_games,
                        double seconds_per_move, int num_threads,
                        const char *data_paths, ErrorStack *error_stack) {
  const bool needs_win_pcts = mode == PGO_MODE_SIM;
  WinPct *win_pcts = NULL;
  if (needs_win_pcts) {
    win_pcts = win_pct_create(data_paths, DEFAULT_WIN_PCT, error_stack);
    exit_on_error(error_stack, "loading win percentages");
  }

  PlayChooserStrategy strategy = {
      .pre_endgame_eval = PLAY_CHOOSER_EVAL_STATIC,
      .endgame_eval = PLAY_CHOOSER_EVAL_STATIC,
      .sim_plies = 2,
      .sim_max_candidates = 15,
      .fixed_seconds_per_move = seconds_per_move,
      .win_pcts = win_pcts,
      .num_threads = num_threads,
      .seed = 0x5eed,
  };
  switch (mode) {
  case PGO_MODE_GENERAL:
    strategy.pre_endgame_eval = PLAY_CHOOSER_EVAL_PEG;
    strategy.endgame_eval = PLAY_CHOOSER_EVAL_ENDGAME;
    break;
  case PGO_MODE_STATIC:
    break;
  case PGO_MODE_SIM:
    strategy.pre_endgame_eval = PLAY_CHOOSER_EVAL_SIM;
    break;
  case PGO_MODE_PEG:
    strategy.pre_endgame_eval = PLAY_CHOOSER_EVAL_PEG;
    break;
  case PGO_MODE_EG:
    strategy.endgame_eval = PLAY_CHOOSER_EVAL_ENDGAME;
    break;
  case PGO_MODE_LEAVEGEN:
    break;
  }

  Game *game = config_game_create(config);
  PlayChooser *chooser = play_chooser_create(&strategy);
  TrainingCounts counts = {0};
  for (int game_index = 0; game_index < num_games; game_index++) {
    game_reset(game);
    game_seed(game, 0x5eedU + ((uint64_t)game_index * 0x9e3779b9U));
    game_set_starting_player_index(game, game_index % 2);
    draw_starting_racks(game);
    while (!game_over(game)) {
      count_move_for_mode(mode, game, &counts);
      Move move;
      play_chooser_choose_move(chooser, game, &move, error_stack);
      exit_on_error(error_stack, "choosing a move");
      play_move(&move, game, NULL);
    }
  }

  validate_training_counts(mode, &counts);
  printf("trained %d games: static=%llu, sim=%llu, peg=%llu, endgame=%llu\n",
         num_games, (unsigned long long)counts.static_moves,
         (unsigned long long)counts.sim_moves,
         (unsigned long long)counts.peg_moves,
         (unsigned long long)counts.endgame_moves);

  play_chooser_destroy(chooser);
  game_destroy(game);
  win_pct_destroy(win_pcts);
}

static void train_general(Config *config, int num_games, double time_control_ms,
                          int num_threads, ErrorStack *error_stack) {
  char command[512];
  (void)snprintf(command, sizeof(command),
                 "autoplay games %d -pc1 %.17g -pc2 %.17g -threads %d "
                 "-mtmode igp -gp false -hr false -pfrequency 0 "
                 "-otpenalty 0 -otperiod 1 -seed 24301",
                 num_games, time_control_ms, time_control_ms, num_threads);
  execute_config_command(config, command, error_stack);
  printf("trained %d timed PlayChooser games at %.3f ms per player\n",
         num_games, time_control_ms);
}

static void train_leavegen(Config *config, int target_count,
                           ErrorStack *error_stack) {
  char command[512];
  (void)snprintf(command, sizeof(command),
                 "leavegen %d 0 tools/pgo_leavegen_racks.txt -seed 24301",
                 target_count);
  execute_config_command(config, command, error_stack);
  printf("trained leavegen until every corpus rack had %d observations\n",
         target_count);
}

int main(int argc, char *argv[]) {
  if (argc != 8) {
    (void)fprintf(stderr,
                  "usage: %s <general|static|sim|peg|eg|leavegen> <games> "
                  "<general-time-control-ms> <focused-seconds-per-move> "
                  "<threads> <data-paths> <leavegen-target>\n",
                  argv[0]);
    return EXIT_FAILURE;
  }

  const pgo_mode_t mode = parse_mode(argv[1]);
  const int num_games = parse_positive_int(argv[2], "games");
  const double time_control_ms =
      parse_positive_double(argv[3], "general-time-control-ms");
  const double seconds_per_move =
      parse_positive_double(argv[4], "focused-seconds-per-move");
  const int num_threads = parse_positive_int(argv[5], "threads");
  const char *data_paths = argv[6];
  const int leavegen_target = parse_positive_int(argv[7], "leavegen-target");

  ErrorStack *error_stack = error_stack_create();
  Config *config = create_training_config(data_paths, num_threads, error_stack);
  if (mode == PGO_MODE_GENERAL) {
    train_general(config, num_games, time_control_ms, num_threads, error_stack);
  } else if (mode == PGO_MODE_LEAVEGEN) {
    train_leavegen(config, leavegen_target, error_stack);
  } else {
    train_games(mode, config, num_games, seconds_per_move, num_threads,
                data_paths, error_stack);
  }
  config_destroy(config);
  error_stack_destroy(error_stack);
  return EXIT_SUCCESS;
}
