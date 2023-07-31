#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "constants.h"
#include "klv.h"
#include "kwg.h"
#include "letter_distribution.h"

static int game_pair_flag;

Config *create_config(const char *kwg_filename,
                      const char *letter_distribution_filename, const char *cgp,
                      const char *klv_filename_1, int move_sorting_1,
                      int play_recorder_type_1, const char *klv_filename_2,
                      int move_sorting_2, int play_recorder_type_2,
                      int game_pair_flag, int number_of_games_or_pairs,
                      const char *actual_tiles_played,
                      int player_to_infer_index, int actual_score,
                      int number_of_tiles_exchanged, double equity_margin,
                      int number_of_threads, const char *winpct_filename,
                      int move_list_capacity) {

  Config *config = malloc(sizeof(Config));
  config->letter_distribution =
      create_letter_distribution(letter_distribution_filename);
  config->kwg = create_kwg(kwg_filename);
  config->cgp = strdup(cgp);
  config->actual_tiles_played = create_rack(config->letter_distribution->size);
  if (actual_tiles_played != NULL) {
    set_rack_to_string(config->actual_tiles_played, actual_tiles_played,
                       config->letter_distribution);
  }
  config->game_pairs = game_pair_flag;
  config->number_of_games_or_pairs = number_of_games_or_pairs;
  config->player_to_infer_index = player_to_infer_index;
  config->actual_score = actual_score;
  config->number_of_tiles_exchanged = number_of_tiles_exchanged;
  config->equity_margin = equity_margin;
  config->number_of_threads = number_of_threads;

  StrategyParams *player_1_strategy_params = malloc(sizeof(StrategyParams));
  if (strcmp(klv_filename_1, "") != 0) {
    player_1_strategy_params->klv = create_klv(klv_filename_1);
  } else {
    player_1_strategy_params->klv = NULL;
  }
  player_1_strategy_params->move_sorting = move_sorting_1;
  player_1_strategy_params->play_recorder_type = play_recorder_type_1;

  config->player_1_strategy_params = player_1_strategy_params;

  StrategyParams *player_2_strategy_params = malloc(sizeof(StrategyParams));

  if (!strcmp(klv_filename_2, "") || !strcmp(klv_filename_2, klv_filename_1)) {
    player_2_strategy_params->klv = player_1_strategy_params->klv;
    config->klv_is_shared = 1;
  } else {
    player_2_strategy_params->klv = create_klv(klv_filename_2);
    config->klv_is_shared = 0;
  }

  if (move_sorting_2 < 0) {
    player_2_strategy_params->move_sorting =
        player_1_strategy_params->move_sorting;
  } else {
    player_2_strategy_params->move_sorting = move_sorting_2;
  }

  if (play_recorder_type_2 < 0) {
    player_2_strategy_params->play_recorder_type =
        player_1_strategy_params->play_recorder_type;
  } else {
    player_2_strategy_params->play_recorder_type = play_recorder_type_2;
  }

  config->player_2_strategy_params = player_2_strategy_params;

  // XXX: do we want to do it this way? not consistent with rest of config.
  if (strcmp(winpct_filename, "")) {
    config->win_pcts = create_winpct(winpct_filename);
  } else {
    config->win_pcts = NULL;
  }
  config->move_list_capacity = move_list_capacity;
  return config;
}

void check_arg_length(const char *arg) {
  if (strlen(arg) > (MAX_ARG_LENGTH)-1) {
    printf("argument exceeded maximum size: %s\n", arg);
    exit(EXIT_FAILURE);
  }
}

Config *create_config_from_args(int argc, char *argv[]) {
  char kwg_filename[(MAX_ARG_LENGTH)] = "";
  char letter_distribution_filename[(MAX_ARG_LENGTH)] = "";
  char cgp[(MAX_ARG_LENGTH)] = "";

  char klv_filename_1[(MAX_ARG_LENGTH)] = "";
  int play_recorder_type_1 = PLAY_RECORDER_TYPE_ALL;
  int move_sorting_1 = SORT_BY_EQUITY;

  char klv_filename_2[(MAX_ARG_LENGTH)] = "";
  int play_recorder_type_2 = -1;
  int move_sorting_2 = -1;
  int number_of_games_or_pairs = 10000;

  char actual_tiles_played[(MAX_ARG_LENGTH)] = "";
  int player_to_infer_index = -1;
  int actual_score = -1;
  int number_of_tiles_exchanged = 0;
  double equity_margin = -1.0;
  int number_of_threads = 1;

  char winpct_filename[(MAX_ARG_LENGTH)] = "";

  int c;
  long n;

  while (1) {
    static struct option long_options[] = {
        {"c", required_argument, 0, 1001},
        {"d", required_argument, 0, 1002},
        {"g", required_argument, 0, 1003},
        {"l1", required_argument, 0, 1004},
        {"r1", required_argument, 0, 1005},
        {"s1", required_argument, 0, 1006},
        {"l2", required_argument, 0, 1007},
        {"r2", required_argument, 0, 1008},
        {"s2", required_argument, 0, 1009},
        {"n", required_argument, 0, 1010},
        {"t", required_argument, 0, 1011},
        {"i", required_argument, 0, 1012},
        {"a", required_argument, 0, 1013},
        {"e", required_argument, 0, 1014},
        {"q", required_argument, 0, 1015},
        {"h", required_argument, 0, 1016},
        {"w", required_argument, 0, 1017},
        {"p", no_argument, &game_pair_flag, 1},
        {0, 0, 0, 0}};
    int option_index = 0;
    c = getopt_long_only(argc, argv, "", long_options, &option_index);

    /* Detect the end of the options. */
    if (c == -1) {
      break;
    }

    switch (c) {
    case 1001:
      check_arg_length(optarg);
      strcpy(cgp, optarg);
      break;

    case 1002:
      check_arg_length(optarg);
      strcpy(letter_distribution_filename, optarg);
      break;

    case 1003:
      check_arg_length(optarg);
      strcpy(kwg_filename, optarg);
      break;

    case 1004:
      check_arg_length(optarg);
      strcpy(klv_filename_1, optarg);
      break;

    case 1005:
      check_arg_length(optarg);
      if (!strcmp("all", optarg)) {
        play_recorder_type_1 = PLAY_RECORDER_TYPE_ALL;
      } else if (!strcmp("top", optarg)) {
        // Not strictly necessary since this
        // is the default.
        play_recorder_type_1 = PLAY_RECORDER_TYPE_TOP_EQUITY;
      } else {
        printf("invalid play recorder option: %s\n", optarg);
        exit(EXIT_FAILURE);
      }
      break;

    case 1006:
      check_arg_length(optarg);
      if (!strcmp("score", optarg)) {
        move_sorting_1 = SORT_BY_SCORE;
      } else if (!strcmp("equity", optarg)) {
        // Not strictly necessary since this
        // is the default.
        move_sorting_1 = SORT_BY_EQUITY;
      } else {
        printf("invalid sort option: %s\n", optarg);
        exit(EXIT_FAILURE);
      }
      break;

    case 1007:
      check_arg_length(optarg);
      strcpy(klv_filename_2, optarg);
      break;

    case 1008:
      check_arg_length(optarg);
      if (!strcmp("all", optarg)) {
        play_recorder_type_2 = PLAY_RECORDER_TYPE_ALL;
      } else if (!strcmp("top", optarg)) {
        // Not strictly necessary since this
        // is the default.
        play_recorder_type_2 = PLAY_RECORDER_TYPE_TOP_EQUITY;
      } else {
        printf("invalid play recorder option: %s\n", optarg);
        exit(EXIT_FAILURE);
      }
      break;

    case 1009:
      check_arg_length(optarg);
      if (!strcmp("score", optarg)) {
        move_sorting_2 = SORT_BY_SCORE;
      } else if (!strcmp("equity", optarg)) {
        // Not strictly necessary since this
        // is the default.
        move_sorting_2 = SORT_BY_EQUITY;
      } else {
        printf("invalid sort option: %s\n", optarg);
        exit(EXIT_FAILURE);
      }
      break;

    case 1010:
      check_arg_length(optarg);
      n = strtol(optarg, NULL, 10);
      number_of_games_or_pairs = (int)n;
      break;

    case 1011:
      check_arg_length(optarg);
      strcpy(actual_tiles_played, optarg);
      break;

    case 1012:
      check_arg_length(optarg);
      n = strtol(optarg, NULL, 10);
      player_to_infer_index = (int)n;
      break;

    case 1013:
      check_arg_length(optarg);
      n = strtol(optarg, NULL, 10);
      actual_score = (int)n;
      break;

    case 1014:
      check_arg_length(optarg);
      n = strtol(optarg, NULL, 10);
      number_of_tiles_exchanged = (int)n;
      break;

    case 1015:
      check_arg_length(optarg);
      equity_margin = atof(optarg);
      break;

    case 1016:
      check_arg_length(optarg);
      n = strtol(optarg, NULL, 10);
      number_of_threads = (int)n;
      break;

    case 1017:
      check_arg_length(optarg);
      strcpy(winpct_filename, optarg);
      break;

    case '?':
      /* getopt_long already printed an error message. */
      break;

    default:
      abort();
    }
  }

  return create_config(kwg_filename, letter_distribution_filename, cgp,
                       klv_filename_1, move_sorting_1, play_recorder_type_1,
                       klv_filename_2, move_sorting_2, play_recorder_type_2,
                       game_pair_flag, number_of_games_or_pairs,
                       actual_tiles_played, player_to_infer_index, actual_score,
                       number_of_tiles_exchanged, equity_margin,
                       number_of_threads, winpct_filename, MOVE_LIST_CAPACITY);
}

void destroy_config(Config *config) {
  if (config->player_1_strategy_params->klv != NULL) {
    destroy_klv(config->player_1_strategy_params->klv);
  }
  free(config->player_1_strategy_params);

  if (!config->klv_is_shared) {
    destroy_klv(config->player_2_strategy_params->klv);
  }

  free(config->player_2_strategy_params);

  destroy_kwg(config->kwg);
  destroy_letter_distribution(config->letter_distribution);
  destroy_rack(config->actual_tiles_played);
  free(config->cgp);

  destroy_winpct(config->win_pcts);

  free(config);
}
