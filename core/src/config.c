#include <assert.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "constants.h"
#include "klv.h"
#include "kwg.h"
#include "letter_distribution.h"
#include "log.h"
#include "string_util.h"
#include "util.h"

#define MAX_ARG_LENGTH 300

#define LOAD_CGP_COMMAND_PREFIX "position cgp "
#define GO_COMMAND_PREFIX "go "

#define GO_COMMAND_SIM_NAME "sim"
#define GO_COMMAND_INFER_NAME "infer"
#define GO_COMMAND_AUTOPLAY_NAME "autoplay"

#define CGP_OPCODE_BINGO_BONUS "bb"
#define CGP_OPCODE_BOARD_NAME "bdn"
#define CGP_OPCODE_GAME_VARIANT "var"
#define CGP_OPCODE_LETTER_DISTRIBUTION_NAME "ld"
#define CGP_OPCODE_LEXICON_NAME "lex"

// Sim go arg name

#define GO_ARG_WIN_PERCENTAGE_FILENAME "winpct"
#define GO_ARG_MOVE_LIST_CAPACITY "cap"

typedef struct CGPOperations {
  int bingo_bonus;
  board_layout_t board_layout;
  game_variant_t game_variant;
  char *letter_distribution_name;
  char *lexicon_name;
} CGPOperations;

CGPOperations *get_default_cgp_operations() {
  CGPOperations *cgp_operations = malloc_or_die(sizeof(CGPOperations));
  cgp_operations->bingo_bonus = BINGO_BONUS;
  cgp_operations->board_layout = BOARD_LAYOUT_CROSSWORD_GAME;
  cgp_operations->game_variant = GAME_VARIANT_CLASSIC;
  cgp_operations->letter_distribution_name = NULL;
  cgp_operations->lexicon_name = NULL;
  return cgp_operations;
}

void destroy_cgp_operations(CGPOperations *cgp_operations) {
  if (cgp_operations->lexicon_name) {
    free(cgp_operations->lexicon_name);
  }
  if (cgp_operations->letter_distribution_name) {
    free(cgp_operations->letter_distribution_name);
  }
  free(cgp_operations);
}

cgp_parse_status_t load_cgp_operations(CGPOperations *cgp_operations,
                                       const char *cgp) {
  cgp_parse_status_t cgp_parse_status = CGP_PARSE_STATUS_SUCCESS;
  StringSplitter *split_cgp_string = split_string_by_whitespace(cgp, true);
  int number_of_items = string_splitter_get_number_of_items(split_cgp_string);
  for (int i = 0; i < number_of_items - 1; i++) {
    const char *opcode = string_splitter_get_item(split_cgp_string, i);
    char *string_value = string_splitter_get_item(split_cgp_string, i + 1);

    // For now all values can be derived from a single contiguous
    // string, so if any of them have a semicolon at the end,
    // remove it.
    // FIXME: move this 'remove last char' function to string util
    size_t string_value_length = string_length(string_value);
    if (string_value[string_value_length - 1] == ';') {
      string_value[string_value_length - 1] = '\0';
    }
    if (strings_equal(CGP_OPCODE_BINGO_BONUS, opcode)) {
      if (!is_all_digits_or_empty(string_value)) {
        cgp_parse_status = CGP_PARSE_STATUS_MALFORMED_CGP_OPCODE_BINGO_BONUS;
        break;
      }
      cgp_operations->bingo_bonus = string_to_int(string_value);
    } else if (strings_equal(CGP_OPCODE_BOARD_NAME, opcode)) {
      cgp_operations->board_layout =
          board_layout_string_to_board_layout(string_value);
      if (cgp_operations->board_layout == BOARD_LAYOUT_UNKNOWN) {
        cgp_parse_status = CGP_PARSE_STATUS_MALFORMED_CGP_OPCODE_BOARD_NAME;
      }
    } else if (strings_equal(CGP_OPCODE_GAME_VARIANT, opcode)) {
      cgp_operations->game_variant =
          get_game_variant_type_from_name(string_value);
      if (cgp_operations->game_variant == GAME_VARIANT_UNKNOWN) {
        cgp_parse_status = CGP_PARSE_STATUS_MALFORMED_CGP_OPCODE_GAME_VARIANT;
      }
    } else if (strings_equal(CGP_OPCODE_LETTER_DISTRIBUTION_NAME, opcode)) {
      if (cgp_operations->letter_distribution_name) {
        free(cgp_operations->letter_distribution_name);
      }
      cgp_operations->letter_distribution_name =
          get_formatted_string("%s", string_value);
    } else if (strings_equal(CGP_OPCODE_LEXICON_NAME, opcode)) {
      if (cgp_operations->lexicon_name) {
        free(cgp_operations->lexicon_name);
      }
      cgp_operations->lexicon_name = get_formatted_string("%s", string_value);
    }
  }
  destroy_string_splitter(split_cgp_string);
  return cgp_parse_status;
}

ThreadControl *create_thread_control_from_config(Config *config) {
  ThreadControl *thread_control = create_thread_control(NULL);
  set_print_info_interval(thread_control, config->print_info);
  set_check_stopping_condition_interval(thread_control, config->checkstop);
  return thread_control;
}

void load_game_config_with_cgp_operations(GameConfig *game_config,
                                          CGPOperations *cgp_operations) {}

go_command_t go_command_from_string(const char *str) {
  go_command_t go_command = GO_COMMAND_UNKNOWN;
  if (strings_equal(GO_COMMAND_SIM_NAME, str)) {
    go_command = GO_COMMAND_SIM;
  } else if (strings_equal(GO_COMMAND_INFER_NAME, str)) {
    go_command = GO_COMMAND_INFER;
  } else if (strings_equal(GO_COMMAND_AUTOPLAY_NAME, str)) {
    go_command = GO_COMMAND_AUTOPLAY;
  }
  return go_command;
}

void load_winpct_for_sim_config(SimConfig *sim_config,
                                const char *win_pct_filename) {
  if (sim_config->win_pct_filename &&
      strings_equal(sim_config->win_pct_filename, win_pct_filename)) {
    return;
  }
  WinPct *new_win_pcts = create_winpct(win_pct_filename);
  destroy_winpct(sim_config->win_pcts);
  sim_config->win_pcts = new_win_pcts;

  free(sim_config->win_pct_filename);
  sim_config->win_pct_filename = get_formatted_string("%s", win_pct_filename);
}

config_load_status_t load_config_for_sim(Config *config,
                                         StringSplitter *go_args) {
  int number_of_args = string_splitter_get_number_of_items(go_args);
  for (int i = 0; i < number_of_args - 1; i++) {
    const char *arg_name = string_splitter_get_item(go_args, i);
    const char *arg_value = string_splitter_get_item(go_args, i + 1);
    if (strings_equal(GO_ARG_WIN_PERCENTAGE_FILENAME, arg_name)) {
      load_winpct_for_sim_config(config->sim_config, arg_value);
    } else if (strings_equal(GO_ARG_MOVE_LIST_CAPACITY, arg_name)) {
      config->sim_config->move_list_capacity = string_to_int(arg_value);
    } else {
      return CONFIG_LOAD_STATUS_UNRECOGNIZED_ARG;
    }
  }
  return CONFIG_LOAD_STATUS_SUCCESS;
}

config_load_status_t load_config_with_go_command(Config *config,
                                                 StringSplitter *go_args) {
  config->go_command = GO_COMMAND_UNKNOWN;
  int number_of_args = string_splitter_get_number_of_items(go_args);
  for (int i = 0; i < number_of_args; i++) {
    go_command_t possible_go_command =
        go_command_from_string(string_splitter_get_item(go_args, i));
    if (possible_go_command != GO_COMMAND_UNKNOWN) {
      if (config->go_command != GO_COMMAND_UNKNOWN) {
        return CONFIG_LOAD_STATUS_MULTIPLE_COMMANDS;
      }
      config->go_command = possible_go_command;
    }
  }
  if (config->go_command == GO_COMMAND_UNKNOWN) {
    return CONFIG_LOAD_STATUS_NO_COMMAND_SPECIFIED;
  }

  config_load_status_t config_load_status = CONFIG_LOAD_STATUS_FAILURE;
  switch (config->go_command) {
  case GO_COMMAND_SIM:
    config_load_status = load_config_for_sim(config, go_args);
    break;
  case GO_COMMAND_INFER:
    config_load_status = load_config_for_infer(config, go_args);
    break;
  case GO_COMMAND_AUTOPLAY:
    config_load_status = load_config_for_autoplay(config, go_args);
    break;
  default:
    log_fatal("unhandled go command: %d\n", config->go_command);
  }
  return config_load_status;
}

config_load_status_t load_config(Config *config, const char *cmd) {
  config_load_status_t config_load_status =
      CONFIG_LOAD_STATUS_UNRECOGNIZED_COMMAND;
  if (has_prefix(LOAD_CGP_COMMAND_PREFIX, cmd)) {
    CGPOperations *cgp_operations = get_default_cgp_operations();
    load_cgp_operations(cgp_operations,
                        cmd + string_length(LOAD_CGP_COMMAND_PREFIX));
    load_game_config_with_cgp_operations(config->game_config, cgp_operations);
    destroy_cgp_operations(cgp_operations);
  } else if (has_prefix(GO_COMMAND_PREFIX, cmd)) {
    StringSplitter *go_args = split_string_by_whitespace(
        cmd + string_length(GO_COMMAND_PREFIX), true);
    config_load_status = load_config_with_go_command(config, go_args);
    destroy_string_splitter(go_args);
  }
}

Config *create_config(const char *letter_distribution_filename, const char *cgp,
                      const char *kwg_filename_1, const char *klv_filename_1,
                      int move_sorting_1, int play_recorder_type_1,
                      const char *kwg_filename_2, const char *klv_filename_2,
                      int move_sorting_2, int play_recorder_type_2,
                      int game_pair_flag, int number_of_games_or_pairs,
                      int print_info, int checkstop,
                      const char *actual_tiles_played,
                      int player_to_infer_index, int actual_score,
                      int number_of_tiles_exchanged, double equity_margin,
                      int number_of_threads, const char *winpct_filename,
                      int move_list_capacity) {

  Config *config = malloc_or_die(sizeof(Config));
  config->letter_distribution =
      create_letter_distribution(letter_distribution_filename);
  string_copy(config->ld_filename, letter_distribution_filename);
  config->cgp = strdup(cgp);
  config->actual_tiles_played = create_rack(config->letter_distribution->size);
  if (actual_tiles_played) {
    set_rack_to_string(config->actual_tiles_played, actual_tiles_played,
                       config->letter_distribution);
  }
  config->use_game_pairs = game_pair_flag;
  config->number_of_games_or_pairs = number_of_games_or_pairs;
  config->print_info = print_info;
  config->checkstop = checkstop;
  config->player_to_infer_index = player_to_infer_index;
  config->actual_score = actual_score;
  config->number_of_tiles_exchanged = number_of_tiles_exchanged;
  config->equity_margin = equity_margin;
  config->number_of_threads = number_of_threads;

  StrategyParams *player_1_strategy_params =
      malloc_or_die(sizeof(StrategyParams));
  if (!strings_equal(kwg_filename_1, "")) {
    player_1_strategy_params->kwg = create_kwg(kwg_filename_1);
    string_copy(player_1_strategy_params->kwg_filename, kwg_filename_1);
  } else {
    player_1_strategy_params->kwg = NULL;
  }

  if (!strings_equal(klv_filename_1, "")) {
    player_1_strategy_params->klv = create_klv(klv_filename_1);
    string_copy(player_1_strategy_params->klv_filename, klv_filename_1);
  } else {
    player_1_strategy_params->klv = NULL;
  }
  player_1_strategy_params->move_sorting = move_sorting_1;
  player_1_strategy_params->play_recorder_type = play_recorder_type_1;

  config->player_1_strategy_params = player_1_strategy_params;

  StrategyParams *player_2_strategy_params =
      malloc_or_die(sizeof(StrategyParams));
  if (strings_equal(kwg_filename_2, "") ||
      strings_equal(kwg_filename_2, kwg_filename_1)) {
    player_2_strategy_params->kwg = player_1_strategy_params->kwg;
    string_copy(player_2_strategy_params->kwg_filename, kwg_filename_1);
    config->kwg_is_shared = 1;
  } else {
    string_copy(player_2_strategy_params->kwg_filename, kwg_filename_2);
    player_2_strategy_params->kwg = create_kwg(kwg_filename_2);
    config->kwg_is_shared = 0;
  }

  if (strings_equal(klv_filename_2, "") ||
      strings_equal(klv_filename_2, klv_filename_1)) {
    player_2_strategy_params->klv = player_1_strategy_params->klv;
    string_copy(player_2_strategy_params->klv_filename, klv_filename_1);
    config->klv_is_shared = 1;
  } else {
    string_copy(player_2_strategy_params->klv_filename, klv_filename_2);
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
  if (!strings_equal(winpct_filename, "")) {
    config->win_pcts = create_winpct(winpct_filename);
    string_copy(config->win_pct_filename, winpct_filename);
  } else {
    config->win_pcts = NULL;
  }
  config->move_list_capacity = move_list_capacity;
  return config;
}

void check_arg_length(const char *arg) {
  if (string_length(arg) > (MAX_ARG_LENGTH)-1) {
    printf("argument exceeded maximum size: %s\n", arg);
    exit(EXIT_FAILURE);
  }
}

Config *create_config_from_args(int argc, char *argv[]) {
  char letter_distribution_filename[(MAX_ARG_LENGTH)] = "";
  char cgp[(MAX_ARG_LENGTH)] = "";

  char kwg_filename_1[(MAX_ARG_LENGTH)] = "";
  char klv_filename_1[(MAX_ARG_LENGTH)] = "";
  int play_recorder_type_1 = MOVE_RECORDER_ALL;
  int move_sorting_1 = MOVE_SORT_EQUITY;

  char kwg_filename_2[(MAX_ARG_LENGTH)] = "";
  char klv_filename_2[(MAX_ARG_LENGTH)] = "";
  int play_recorder_type_2 = -1;
  int move_sorting_2 = -1;

  int number_of_games_or_pairs = 0;
  int print_info = 0;
  int checkstop = 0;

  char actual_tiles_played[(MAX_ARG_LENGTH)] = "";
  int player_to_infer_index = -1;
  int actual_score = -1;
  int number_of_tiles_exchanged = 0;
  double equity_margin = -1.0;
  int number_of_threads = 1;

  char winpct_filename[(MAX_ARG_LENGTH)] = "";
  int use_game_pairs = 1;

  int c;
  long n;

  while (1) {
    static struct option long_options[] = {
        {"c", required_argument, 0, 1001},  {"d", required_argument, 0, 1002},
        {"g1", required_argument, 0, 1003}, {"l1", required_argument, 0, 1004},
        {"r1", required_argument, 0, 1005}, {"s1", required_argument, 0, 1006},
        {"g2", required_argument, 0, 1007}, {"l2", required_argument, 0, 1008},
        {"r2", required_argument, 0, 1009}, {"s2", required_argument, 0, 1010},
        {"n", required_argument, 0, 1011},  {"t", required_argument, 0, 1012},
        {"i", required_argument, 0, 1013},  {"a", required_argument, 0, 1014},
        {"e", required_argument, 0, 1015},  {"q", required_argument, 0, 1016},
        {"h", required_argument, 0, 1017},  {"w", required_argument, 0, 1018},
        {"f", required_argument, 0, 1019},  {"k", required_argument, 0, 1020},
        {"p", required_argument, 0, 1021},  {0, 0, 0, 0}};
    int option_index = 0;
    c = getopt_long_only(argc, argv, "", long_options, &option_index);

    /* Detect the end of the options. */
    if (c == -1) {
      break;
    }

    switch (c) {
    case 1001:
      check_arg_length(optarg);
      string_copy(cgp, optarg);
      break;

    case 1002:
      check_arg_length(optarg);
      string_copy(letter_distribution_filename, optarg);
      break;

    case 1003:
      check_arg_length(optarg);
      string_copy(kwg_filename_1, optarg);
      break;

    case 1004:
      check_arg_length(optarg);
      string_copy(klv_filename_1, optarg);
      break;

    case 1005:
      check_arg_length(optarg);
      if (strings_equal("all", optarg)) {
        play_recorder_type_1 = MOVE_RECORDER_ALL;
      } else if (strings_equal("top", optarg)) {
        // Not strictly necessary since this
        // is the default.
        play_recorder_type_1 = MOVE_RECORDER_BEST;
      } else {
        printf("invalid play recorder option: %s\n", optarg);
        exit(EXIT_FAILURE);
      }
      break;

    case 1006:
      check_arg_length(optarg);
      if (strings_equal("score", optarg)) {
        move_sorting_1 = MOVE_SORT_SCORE;
      } else if (strings_equal("equity", optarg)) {
        // Not strictly necessary since this
        // is the default.
        move_sorting_1 = MOVE_SORT_EQUITY;
      } else {
        printf("invalid sort option: %s\n", optarg);
        exit(EXIT_FAILURE);
      }
      break;

    case 1007:
      check_arg_length(optarg);
      string_copy(kwg_filename_2, optarg);
      break;

    case 1008:
      check_arg_length(optarg);
      string_copy(klv_filename_2, optarg);
      break;

    case 1009:
      check_arg_length(optarg);
      if (strings_equal("all", optarg)) {
        play_recorder_type_2 = MOVE_RECORDER_ALL;
      } else if (strings_equal("top", optarg)) {
        // Not strictly necessary since this
        // is the default.
        play_recorder_type_2 = MOVE_RECORDER_BEST;
      } else {
        printf("invalid play recorder option: %s\n", optarg);
        exit(EXIT_FAILURE);
      }
      break;

    case 1010:
      check_arg_length(optarg);
      if (strings_equal("score", optarg)) {
        move_sorting_2 = MOVE_SORT_SCORE;
      } else if (strings_equal("equity", optarg)) {
        // Not strictly necessary since this
        // is the default.
        move_sorting_2 = MOVE_SORT_EQUITY;
      } else {
        printf("invalid sort option: %s\n", optarg);
        exit(EXIT_FAILURE);
      }
      break;

    case 1011:
      check_arg_length(optarg);
      n = strtol(optarg, NULL, 10);
      number_of_games_or_pairs = (int)n;
      break;

    case 1012:
      check_arg_length(optarg);
      string_copy(actual_tiles_played, optarg);
      break;

    case 1013:
      check_arg_length(optarg);
      n = strtol(optarg, NULL, 10);
      player_to_infer_index = (int)n;
      break;

    case 1014:
      check_arg_length(optarg);
      n = strtol(optarg, NULL, 10);
      actual_score = (int)n;
      break;

    case 1015:
      check_arg_length(optarg);
      n = strtol(optarg, NULL, 10);
      number_of_tiles_exchanged = (int)n;
      break;

    case 1016:
      check_arg_length(optarg);
      equity_margin = atof(optarg);
      break;

    case 1017:
      check_arg_length(optarg);
      n = strtol(optarg, NULL, 10);
      number_of_threads = (int)n;
      break;

    case 1018:
      check_arg_length(optarg);
      string_copy(winpct_filename, optarg);
      break;

    case 1019:
      check_arg_length(optarg);
      n = strtol(optarg, NULL, 10);
      print_info = (int)n;
      break;

    case 1020:
      check_arg_length(optarg);
      n = strtol(optarg, NULL, 10);
      checkstop = (int)n;
      break;

    case 1021:
      check_arg_length(optarg);
      n = strtol(optarg, NULL, 10);
      use_game_pairs = (int)n;
      break;

    case '?':
      /* getopt_long already printed an error message. */
      break;

    default:
      log_fatal("arg not handled: %d\n", c);
    }
  }

  return create_config(
      letter_distribution_filename, cgp, kwg_filename_1, klv_filename_1,
      move_sorting_1, play_recorder_type_1, kwg_filename_2, klv_filename_2,
      move_sorting_2, play_recorder_type_2, use_game_pairs,
      number_of_games_or_pairs, print_info, checkstop, actual_tiles_played,
      player_to_infer_index, actual_score, number_of_tiles_exchanged,
      equity_margin, number_of_threads, winpct_filename,
      DEFAULT_MOVE_LIST_CAPACITY);
}

void destroy_config(Config *config) {
  if (config->player_1_strategy_params->klv) {
    destroy_klv(config->player_1_strategy_params->klv);
  }
  if (config->player_1_strategy_params->kwg) {
    destroy_kwg(config->player_1_strategy_params->kwg);
  }
  free(config->player_1_strategy_params);

  if (!config->klv_is_shared) {
    destroy_klv(config->player_2_strategy_params->klv);
  }
  if (!config->kwg_is_shared) {
    destroy_kwg(config->player_2_strategy_params->kwg);
  }
  free(config->player_2_strategy_params);

  destroy_letter_distribution(config->letter_distribution);
  destroy_rack(config->actual_tiles_played);
  free(config->cgp);

  destroy_winpct(config->win_pcts);

  free(config);
}

// potentially edit an existing config, or create a brand new one
void load_config_from_lexargs(Config **config, const char *cgp,
                              char *lexicon_name, char *ldname) {

  char *dist = get_formatted_string("data/letterdistributions/%s.csv", ldname);
  char leaves[50] = "data/lexica/english.klv2";
  char winpct[50] = "data/strategy/default_english/winpct.csv";
  char *lexicon_file = get_formatted_string("data/lexica/%s.kwg", lexicon_name);
  if (strings_equal(lexicon_name, "CSW21")) {
    string_copy(leaves, "data/lexica/CSW21.klv2");
  } else if (has_prefix("NSF", lexicon_name)) {
    string_copy(leaves, "data/lexica/norwegian.klv2");
  } else if (has_prefix("RD", lexicon_name)) {
    string_copy(leaves, "data/lexica/german.klv2");
  } else if (has_prefix("DISC", lexicon_name)) {
    string_copy(leaves, "data/lexica/catalan.klv2");
  } else if (has_prefix("FRA", lexicon_name)) {
    string_copy(leaves, "data/lexica/french.klv2");
  }

  if (!*config) {
    *config = create_config(dist, cgp, lexicon_file, leaves, MOVE_SORT_EQUITY,
                            MOVE_RECORDER_ALL, "", "", MOVE_SORT_EQUITY,
                            MOVE_RECORDER_ALL, 0, 0, 9, 0, "", 0, 0, 0, 0, 0,
                            winpct, 100);
  } else {
    Config *c = (*config);
    // check each filename
    if (!strings_equal(c->ld_filename, dist)) {
      // They're different; reload.
      log_debug("reloading letter distribution; was %s, new %s", c->ld_filename,
                dist);
      destroy_letter_distribution(c->letter_distribution);
      c->letter_distribution = create_letter_distribution(dist);
    }
    if (!strings_equal(c->player_1_strategy_params->kwg_filename,
                       lexicon_file)) {
      log_debug("reloading kwg #1");
      destroy_kwg(c->player_1_strategy_params->kwg);
      c->player_1_strategy_params->kwg = create_kwg(lexicon_file);
      // assume the kwg applies to both players if we're using this function
      assert(c->kwg_is_shared);
      c->player_2_strategy_params->kwg = c->player_1_strategy_params->kwg;
      string_copy(c->player_2_strategy_params->kwg_filename, lexicon_file);
    }
    if (!strings_equal(c->player_1_strategy_params->klv_filename, leaves)) {
      log_debug("reloading klv #1");
      destroy_klv(c->player_1_strategy_params->klv);
      c->player_1_strategy_params->klv = create_klv(leaves);
      // assume the klv applies to both players if we're using this function
      assert(c->klv_is_shared);
      c->player_2_strategy_params->klv = c->player_1_strategy_params->klv;
      string_copy(c->player_2_strategy_params->klv_filename, leaves);
    }

    free(c->cgp);
    c->cgp = strdup(cgp);
  }
  free(dist);
  free(lexicon_file);
}

StrategyParams *copy_strategy_params(StrategyParams *orig) {
  StrategyParams *sp = malloc_or_die(sizeof(StrategyParams));
  // No need to copy the klv itself.
  sp->klv = orig->klv;
  sp->kwg = orig->kwg;
  string_copy(sp->klv_filename, orig->klv_filename);
  sp->move_sorting = orig->move_sorting;
  sp->play_recorder_type = orig->play_recorder_type;

  return sp;
}

void destroy_strategy_params(StrategyParams *sp) { free(sp); }