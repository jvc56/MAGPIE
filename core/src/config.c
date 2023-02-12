#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <getopt.h>

#include "config.h"
#include "constants.h"
#include "gaddag.h"
#include "letter_distribution.h"
#include "leaves.h"

Config * create_config(const char * gaddag_filename, const char * alphabet_filename, const char * letter_distribution_filename, const char * laddag_filename, const char * cgp, int move_sorting, int play_recorder_type) {
    Config * config = malloc(sizeof(Config));
    config->gaddag = create_gaddag(gaddag_filename, alphabet_filename);
    config->letter_distribution = create_letter_distribution(letter_distribution_filename);
    config->cgp = strdup(cgp);
	  config->laddag = create_laddag(laddag_filename);
    config->move_sorting = move_sorting;
    config->play_recorder_type = play_recorder_type;
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
  char letter_distribution_filename[(MAX_ARG_LENGTH)] = "";
  char gaddag_filename[(MAX_ARG_LENGTH)] = "";
  char laddag_filename[(MAX_ARG_LENGTH)] = "";
  char cgp[(MAX_ARG_LENGTH)] = "";
  int play_recorder_type = PLAY_RECORDER_TYPE_ALL;
  int move_sorting = SORT_BY_EQUITY;

  int c;

  while (1) {
    static struct option long_options[] =
      {
        {"alph",     required_argument, 0, 'a'},
        {"cgp",      required_argument, 0, 'c'},
        {"dist",     required_argument, 0, 'd'},
        {"gaddag",   required_argument, 0, 'g'},
        {"laddag",   required_argument, 0, 'l'},
        {"recorder", required_argument, 0, 'r'},
        {"sort",     required_argument, 0, 's'},
        {0, 0, 0, 0}
      };
    int option_index = 0;
    c = getopt_long_only (argc, argv, "a:c:d:g:l:r:s:",
                      long_options, &option_index);

    /* Detect the end of the options. */
    if (c == -1) {
      break;
    }

    switch (c) {
      case 'a':
        check_arg_length(optarg);
        strcpy(alphabet_filename, optarg);
        break;

      case 'c':
        check_arg_length(optarg);
        strcpy(cgp, optarg);
        break;

      case 'd':
        check_arg_length(optarg);
        strcpy(letter_distribution_filename, optarg);
        break;

      case 'g':
        check_arg_length(optarg);
        strcpy(gaddag_filename, optarg);
        break;

      case 'l':
        check_arg_length(optarg);
        strcpy(laddag_filename, optarg);
        break;

      case 'r':
        check_arg_length(optarg);
        if (!strcmp("all", optarg)) {
          play_recorder_type = PLAY_RECORDER_TYPE_ALL;
        } else if (!strcmp("top", optarg)) {
          // Not strictly necessary since this
          // is the default.
          play_recorder_type = PLAY_RECORDER_TYPE_TOP_EQUITY;
        } else {
          printf("invalid play recorder option: %s\n", optarg);
          exit(EXIT_FAILURE);
        }
        break;

      case 's':
        check_arg_length(optarg);
        if (!strcmp("score", optarg)) {
          move_sorting = SORT_BY_SCORE;
        } else if (!strcmp("equity", optarg)) {
          // Not strictly necessary since this
          // is the default.
          move_sorting = SORT_BY_EQUITY;
        } else {
          printf("invalid sort option: %s\n", optarg);
          exit(EXIT_FAILURE);
        }
        break;

      case '?':
        /* getopt_long already printed an error message. */
        break;

      default:
        abort ();
      }
  }
  return create_config(gaddag_filename, alphabet_filename, letter_distribution_filename, laddag_filename, cgp, move_sorting, play_recorder_type);
}

void destroy_config(Config * config) {
	destroy_laddag(config->laddag);
	destroy_letter_distribution(config->letter_distribution);
	destroy_gaddag(config->gaddag);
  free(config->cgp);
  free(config);
}
