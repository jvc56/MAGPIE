#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <getopt.h>

#include "config.h"
#include "constants.h"
#include "gaddag.h"
#include "letter_distribution.h"
#include "leaves.h"

static int game_pair_flag;

Config * create_config(const char * gaddag_filename, const char * alphabet_filename, const char * letter_distribution_filename, const char * cgp,
                       const char * laddag_filename_1, int move_sorting_1, int play_recorder_type_1,
                       const char * laddag_filename_2, int move_sorting_2, int play_recorder_type_2, int game_pair_flag, int number_of_games_or_pairs) {

    Config * config = malloc(sizeof(Config));
    config->letter_distribution = create_letter_distribution(letter_distribution_filename);
    config->gaddag = create_gaddag(gaddag_filename, alphabet_filename);
    config->cgp = strdup(cgp);
    config->game_pairs = game_pair_flag;
    config->number_of_games_or_pairs = number_of_games_or_pairs;

    StrategyParams * player_1_strategy_params = malloc(sizeof(StrategyParams));

	  player_1_strategy_params->laddag = create_laddag(laddag_filename_1);
    player_1_strategy_params->move_sorting = move_sorting_1;
    player_1_strategy_params->play_recorder_type = play_recorder_type_1;

    config->player_1_strategy_params = player_1_strategy_params;

    StrategyParams * player_2_strategy_params = malloc(sizeof(StrategyParams));

    if (!strcmp(laddag_filename_2, "") || !strcmp(laddag_filename_2, laddag_filename_1) ) {
      player_2_strategy_params->laddag = player_1_strategy_params->laddag;
      config->laddag_is_shared = 1;
    } else {
      player_2_strategy_params->laddag = create_laddag(laddag_filename_2);
      config->laddag_is_shared = 0;
    }

    if (move_sorting_2 < 0) {
      player_2_strategy_params->move_sorting = player_1_strategy_params->move_sorting;
    } else {
      player_2_strategy_params->move_sorting = move_sorting_2;
    }

    if (play_recorder_type_2 < 0) {
      player_2_strategy_params->play_recorder_type = player_1_strategy_params->play_recorder_type;
    } else {
      player_2_strategy_params->play_recorder_type = play_recorder_type_2;
    }

    config->player_2_strategy_params = player_2_strategy_params;

    return config;
}

void check_arg_length(const char * arg) {
  if (strlen(arg) > (MAX_ARG_LENGTH) - 1) {
    printf("argument exceeded maximum size: %s\n", arg);
    exit(EXIT_FAILURE);
  }
}

Config * create_config_from_args(int argc, char *argv[]) {
  char alphabet_filename[(MAX_ARG_LENGTH)] = "";
  char gaddag_filename[(MAX_ARG_LENGTH)] = "";
  char letter_distribution_filename[(MAX_ARG_LENGTH)] = "";
  char cgp[(MAX_ARG_LENGTH)] = "";

  char laddag_filename_1[(MAX_ARG_LENGTH)] = "";
  int play_recorder_type_1 = PLAY_RECORDER_TYPE_ALL;
  int move_sorting_1 = SORT_BY_EQUITY;

  char laddag_filename_2[(MAX_ARG_LENGTH)] = "";
  int play_recorder_type_2 = -1;
  int move_sorting_2 = -1;
  int number_of_games_or_pairs = 10000;

  int c;

  while (1) {
    static struct option long_options[] =
      {
        {"a",  required_argument, 0, 1000},
        {"c",  required_argument, 0, 1001},
        {"d",  required_argument, 0, 1002},
        {"g",  required_argument, 0, 1003},
        {"l1", required_argument, 0, 1004},
        {"r1", required_argument, 0, 1005},
        {"s1", required_argument, 0, 1006},
        {"l2", required_argument, 0, 1007},
        {"r2", required_argument, 0, 1008},
        {"s2", required_argument, 0, 1009},
        {"n",  required_argument, 0, 1010},
        {"p",  no_argument, &game_pair_flag, 1},
        {0, 0, 0, 0}
      };
    int option_index = 0;
    c = getopt_long_only (argc, argv, "",
                      long_options, &option_index);

    /* Detect the end of the options. */
    if (c == -1) {
      break;
    }

    switch (c) {
      case 1000:
        check_arg_length(optarg);
        strcpy(alphabet_filename, optarg);
        break;

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
        strcpy(gaddag_filename, optarg);
        break;

      case 1004:
        check_arg_length(optarg);
        strcpy(laddag_filename_1, optarg);
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
        strcpy(laddag_filename_2, optarg);
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
        long n = strtol(optarg, NULL, 10);
        number_of_games_or_pairs = (int)n;
        break;

      case '?':
        /* getopt_long already printed an error message. */
        break;

      default:
        abort ();
      }
  }

  return create_config(gaddag_filename, alphabet_filename, letter_distribution_filename, cgp,
  laddag_filename_1, move_sorting_1, play_recorder_type_1,
  laddag_filename_2, move_sorting_2, play_recorder_type_2, game_pair_flag, number_of_games_or_pairs);
}

void destroy_config(Config * config) {
	destroy_laddag(config->player_1_strategy_params->laddag);
  free(config->player_1_strategy_params);

  if (!config->laddag_is_shared) {
    destroy_laddag(config->player_2_strategy_params->laddag);
  }

  free(config->player_2_strategy_params);

	destroy_gaddag(config->gaddag);
	destroy_letter_distribution(config->letter_distribution);
  free(config->cgp);
  free(config);
}
