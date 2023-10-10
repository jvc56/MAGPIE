#include <assert.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "cgp.h"
#include "config.h"
#include "constants.h"
#include "klv.h"
#include "kwg.h"
#include "letter_distribution.h"
#include "log.h"
#include "string_util.h"
#include "util.h"

#define DEFAULT_BINGO_BONUS 50
#define DEFAULT_BOARD_LAYOUT BOARD_LAYOUT_CROSSWORD_GAME
#define DEFAULT_GAME_VARIANT GAME_VARIANT_CLASSIC
#define DEFAULT_MOVE_LIST_CAPACITY 20
#define DEFAULT_MOVE_SORT_TYPE MOVE_SORT_EQUITY
#define DEFAULT_MOVE_RECORD_TYPE MOVE_RECORDER_BEST

// Parsed Args
typedef enum {
  // Commands
  ARG_TOKEN_POSITION,
  ARG_TOKEN_CGP,
  ARG_TOKEN_GO,
  ARG_TOKEN_SIM,
  ARG_TOKEN_INFER,
  ARG_TOKEN_AUTOPLAY,
  // Game
  // shared between players
  ARG_TOKEN_CGP_BINGO_BONUS,
  ARG_TOKEN_CGP_BOARD_LAYOUT,
  ARG_TOKEN_CGP_GAME_VARIANT,
  ARG_TOKEN_CGP_LETTER_DISTRIBUTION,
  ARG_TOKEN_CGP_LEXICON,
  // possibly unique for each player
  ARG_TOKEN_P1_LEXICON,
  ARG_TOKEN_P1_LEAVES,
  ARG_TOKEN_P1_MOVE_SORT_TYPE,
  ARG_TOKEN_P1_MOVE_RECORD_TYPE,
  ARG_TOKEN_P2_LEXICON,
  ARG_TOKEN_P2_LEAVES,
  ARG_TOKEN_P2_MOVE_SORT_TYPE,
  ARG_TOKEN_P2_MOVE_RECORD_TYPE,
  // Sim
  ARG_TOKEN_KNOWN_OPP_RACK,
  ARG_TOKEN_WIN_PCT_FILENAME,
  ARG_TOKEN_MOVE_LIST_CAPACITY,
  ARG_TOKEN_PLIES,
  ARG_TOKEN_NUMBER_OF_PLAYS,
  ARG_TOKEN_MAX_ITERATIONS,
  ARG_TOKEN_STOPPING_CONDITION,
  ARG_TOKEN_STATIC_SEARCH_ONLY,
  // Infer
  // Rack is shared with sim
  ARG_TOKEN_PLAYER_TO_INFER_INDEX,
  ARG_TOKEN_SCORE,
  ARG_TOKEN_EQUITY_MARGIN,
  ARG_TOKEN_NUMBER_OF_TILES_EXCHANGED,
  // Autoplay
  ARG_TOKEN_USE_GAME_PAIRS,
  ARG_TOKEN_SEED,
  // number of iterations shared with sim
  // Thread Control
  // This must always be the last
  // token for the count to be accurate
  NUMBER_OF_ARG_TOKENS
} arg_token_t;

// Valid command sequences

const struct {
  struct {
    // The command type for this sequence
    command_t ct;
    // The sequence of tokens associated with this command type
    arg_token_t ats[3];
  } cs[5];
} VALID_COMMAND_SEQUENCES = {{
    // The valid sequences
    {COMMAND_TYPE_LOAD_CGP,
     // The NUMBER_OF_ARG_TOKENS token denotes the end of the arg token
     // sequence
     {ARG_TOKEN_POSITION, ARG_TOKEN_CGP, NUMBER_OF_ARG_TOKENS}},
    {COMMAND_TYPE_SIM, {ARG_TOKEN_GO, ARG_TOKEN_SIM, NUMBER_OF_ARG_TOKENS}},
    {COMMAND_TYPE_INFER, {ARG_TOKEN_GO, ARG_TOKEN_INFER, NUMBER_OF_ARG_TOKENS}},
    {COMMAND_TYPE_AUTOPLAY,
     {ARG_TOKEN_GO, ARG_TOKEN_AUTOPLAY, NUMBER_OF_ARG_TOKENS}},
    // The unknown command type denotes the end of the sequences
    {COMMAND_TYPE_UNKNOWN, {}},
}};

typedef struct SingleArg {
  arg_token_t token;
  char *name;
  command_t command_type;
  bool has_value;
  int number_of_values;
  char **values;
  int position;
} SingleArg;

typedef struct ParsedArgs {
  SingleArg *args[NUMBER_OF_ARG_TOKENS];
} ParsedArgs;

SingleArg *create_single_arg() {
  SingleArg *single_arg = malloc_or_die(sizeof(SingleArg));
  return single_arg;
}

void destory_single_arg(SingleArg *single_arg) {
  if (single_arg->name) {
    free(single_arg->name);
  }
  if (single_arg->number_of_values > 0) {
    for (int i = 0; i < single_arg->number_of_values; i++) {
      if (single_arg->values[i]) {
        free(single_arg->values[i]);
      }
      free(single_arg->values);
    }
  }
  free(single_arg);
}

void set_single_arg(ParsedArgs *parsed_args, int index, command_t command_type,
                    arg_token_t arg_token, const char *arg_name,
                    int number_of_values) {
  SingleArg *single_arg = create_single_arg();
  single_arg->command_type = command_type;
  single_arg->token = arg_token;
  single_arg->name = get_formatted_string("%s", arg_name);
  single_arg->number_of_values = number_of_values;
  single_arg->values = NULL;
  if (number_of_values > 0) {
    single_arg->values = malloc_or_die(sizeof(char *) * number_of_values);
  }
  single_arg->has_value = false;
  single_arg->position = NUMBER_OF_ARG_TOKENS + 1;
  parsed_args->args[index] = single_arg;
}

ParsedArgs *create_parsed_args(StringSplitter *cmd) {
  ParsedArgs *parsed_args = malloc_or_die(sizeof(ParsedArgs));

  int index = 0;
  // Command args
  set_single_arg(parsed_args, index++, COMMAND_TYPE_UNKNOWN, ARG_TOKEN_POSITION,
                 "position", 0);
  set_single_arg(parsed_args, index++, COMMAND_TYPE_UNKNOWN, ARG_TOKEN_CGP,
                 "cgp", 4);
  set_single_arg(parsed_args, index++, COMMAND_TYPE_UNKNOWN, ARG_TOKEN_GO, "go",
                 0);
  set_single_arg(parsed_args, index++, COMMAND_TYPE_UNKNOWN, ARG_TOKEN_SIM,
                 "sim", 0);
  set_single_arg(parsed_args, index++, COMMAND_TYPE_UNKNOWN, ARG_TOKEN_INFER,
                 "infer", 0);
  set_single_arg(parsed_args, index++, COMMAND_TYPE_UNKNOWN, ARG_TOKEN_AUTOPLAY,
                 "autoplay", 0);

  // CGP args
  set_single_arg(parsed_args, index++, COMMAND_TYPE_LOAD_CGP,
                 ARG_TOKEN_CGP_BINGO_BONUS, "bb", 1);
  set_single_arg(parsed_args, index++, COMMAND_TYPE_LOAD_CGP,
                 ARG_TOKEN_CGP_BOARD_LAYOUT, "bdn", 1);
  set_single_arg(parsed_args, index++, COMMAND_TYPE_LOAD_CGP,
                 ARG_TOKEN_CGP_GAME_VARIANT, "var", 1);
  set_single_arg(parsed_args, index++, COMMAND_TYPE_LOAD_CGP,
                 ARG_TOKEN_CGP_LETTER_DISTRIBUTION, "ld", 1);
  set_single_arg(parsed_args, index++, COMMAND_TYPE_LOAD_CGP,
                 ARG_TOKEN_CGP_LEXICON, "lex", 1);

  // Game args
  // Player 1
  set_single_arg(parsed_args, index++, COMMAND_TYPE_UNKNOWN,
                 ARG_TOKEN_P1_LEXICON, "l1", 1);
  set_single_arg(parsed_args, index++, COMMAND_TYPE_UNKNOWN,
                 ARG_TOKEN_P1_LEAVES, "k1", 1);
  set_single_arg(parsed_args, index++, COMMAND_TYPE_UNKNOWN,
                 ARG_TOKEN_P1_MOVE_SORT_TYPE, "s1", 1);
  set_single_arg(parsed_args, index++, COMMAND_TYPE_UNKNOWN,
                 ARG_TOKEN_P1_MOVE_RECORD_TYPE, "r1", 1);

  // Player 2
  set_single_arg(parsed_args, index++, COMMAND_TYPE_UNKNOWN,
                 ARG_TOKEN_P2_LEXICON, "l2", 1);
  set_single_arg(parsed_args, index++, COMMAND_TYPE_UNKNOWN,
                 ARG_TOKEN_P2_LEAVES, "k2", 1);
  set_single_arg(parsed_args, index++, COMMAND_TYPE_UNKNOWN,
                 ARG_TOKEN_P2_MOVE_SORT_TYPE, "s2", 1);
  set_single_arg(parsed_args, index++, COMMAND_TYPE_UNKNOWN,
                 ARG_TOKEN_P2_MOVE_RECORD_TYPE, "r2", 1);

  // Sim args
  set_single_arg(parsed_args, index++, COMMAND_TYPE_SIM,
                 ARG_TOKEN_WIN_PCT_FILENAME, "winpct", 1);
  set_single_arg(parsed_args, index++, COMMAND_TYPE_SIM,
                 ARG_TOKEN_MOVE_LIST_CAPACITY, "cap", 1);

  // Inference args
  set_single_arg(parsed_args, index++, COMMAND_TYPE_INFER,
                 ARG_TOKEN_KNOWN_OPP_RACK, "tilesplayed", 1);
  set_single_arg(parsed_args, index++, COMMAND_TYPE_INFER,
                 ARG_TOKEN_PLAYER_TO_INFER_INDEX, "pindex", 1);
  set_single_arg(parsed_args, index++, COMMAND_TYPE_INFER, ARG_TOKEN_SCORE,
                 "score", 1);
  set_single_arg(parsed_args, index++, COMMAND_TYPE_INFER,
                 ARG_TOKEN_EQUITY_MARGIN, "eq", 1);

  return parsed_args;
}

void destroy_parsed_args(ParsedArgs *parsed_args) {
  for (int i = 0; i < NUMBER_OF_ARG_TOKENS; i++) {
    destroy_single_arg(parsed_args->args[i]);
  }
  free(parsed_args);
}

config_load_status_t init_parsed_args(ParsedArgs *parsed_args,
                                      StringSplitter *cmd) {
  int number_of_input_args = string_splitter_get_number_of_items(cmd);
  config_load_status_t config_load_status = CONFIG_LOAD_STATUS_SUCCESS;
  for (int i = 0; i < number_of_input_args; i++) {
    char *input_arg = string_splitter_get_item(cmd, i);
    bool is_recognized_arg = false;
    for (int j = 0; j < NUMBER_OF_ARG_TOKENS; j++) {
      SingleArg *single_arg = parsed_args->args[j];
      if (strings_equal(input_arg, single_arg->name)) {
        if (single_arg->has_value) {
          return CONFIG_LOAD_STATUS_DUPLICATE_ARG;
        } else if (i + single_arg->number_of_values < number_of_input_args) {
          for (int k = 0; k < single_arg->number_of_values; k++) {
            single_arg->values[k] = get_formatted_string(
                "%s", string_splitter_get_item(cmd, i + k));
          }
          single_arg->has_value = true;
          single_arg->position = i;
        } else {
          return CONFIG_LOAD_STATUS_INSUFFICIENT_NUMBER_OF_VALUES;
        }
        is_recognized_arg = true;
        break;
      }
    }
    if (!is_recognized_arg) {
      return CONFIG_LOAD_STATUS_UNRECOGNIZED_ARG;
    }
  }

  // Do a simple insertion sort
  // to order the args in the user
  // input order to more easily
  // validate the command and subcommand

  SingleArg *temp_single_arg;
  int j;

  for (int i = 1; i < NUMBER_OF_ARG_TOKENS; i++) {
    temp_single_arg = parsed_args->args[i];
    j = i - 1;
    while (j >= 0 &&
           parsed_args->args[j]->position > temp_single_arg->position) {
      parsed_args->args[j + 1] = parsed_args->args[j];
      j = j - 1;
    }
    parsed_args->args[j + 1] = temp_single_arg;
  }

  return CONFIG_LOAD_STATUS_SUCCESS;
}

config_load_status_t load_winpct_for_config(Config *config,
                                            const char *win_pct_name) {
  if (strings_equal(config->win_pct_name, win_pct_name)) {
    return CONFIG_LOAD_STATUS_SUCCESS;
  }
  WinPct *new_win_pcts = create_winpct(win_pct_name);
  if (config->win_pcts) {
    destroy_winpct(config->win_pcts);
  }
  config->win_pcts = new_win_pcts;

  if (config->win_pct_name) {
    free(config->win_pct_name);
  }
  config->win_pct_name = get_formatted_string("%s", win_pct_name);
  return CONFIG_LOAD_STATUS_SUCCESS;
}

config_load_status_t
load_move_list_capacity_for_config(Config *config,
                                   const char *move_list_capacity) {
  config->move_list_capacity = string_to_int(move_list_capacity);
  return CONFIG_LOAD_STATUS_SUCCESS;
}

config_load_status_t load_bingo_bonus_for_config(Config *config,
                                                 const char *bingo_bonus) {
  if (!is_all_digits_or_empty(bingo_bonus)) {
    return CONFIG_LOAD_STATUS_MALFORMED_BINGO_BONUS;
  }
  config->bingo_bonus = string_to_int(bingo_bonus);
  return CONFIG_LOAD_STATUS_SUCCESS;
}

config_load_status_t load_board_layout_for_config(Config *config,
                                                  const char *board_layout) {
  config->board_layout = board_layout_string_to_board_layout(board_layout);
  if (config->board_layout == BOARD_LAYOUT_UNKNOWN) {
    return CONFIG_LOAD_STATUS_UNKNOWN_BOARD_LAYOUT;
  }
  return CONFIG_LOAD_STATUS_SUCCESS;
}

config_load_status_t load_game_variant_for_config(Config *config,
                                                  const char *game_variant) {
  config->game_variant = get_game_variant_type_from_name(game_variant);
  if (config->game_variant == GAME_VARIANT_UNKNOWN) {
    return CONFIG_LOAD_STATUS_UNKNOWN_GAME_VARIANT;
  }
  return CONFIG_LOAD_STATUS_SUCCESS;
}

config_load_status_t
load_letter_distribution_for_config(Config *config,
                                    const char *letter_distribution_name) {
  if (strings_equal(config->ld_name, letter_distribution_name)) {
    return CONFIG_LOAD_STATUS_SUCCESS;
  }
  LetterDistribution *new_letter_distribution =
      create_letter_distribution(letter_distribution_name);
  if (config->letter_distribution) {
    destroy_letter_distribution(config->letter_distribution);
  }
  config->letter_distribution = new_letter_distribution;

  if (config->letter_distribution_name) {
    free(config->letter_distribution_name);
  }
  config->letter_distribution_name =
      get_formatted_string("%s", letter_distribution_name);
  return CONFIG_LOAD_STATUS_SUCCESS;
}

config_load_status_t load_lexicon_for_config(Config *config,
                                             const char *lexicon_name,
                                             int player_index) {
  // Check if kwg is already defined
  if (string_equals(config->player_strategy_params[player_index]->kwg_name,
                    lexicon_name)) {
    return;
  }
  // Check if the other player has already loaded the same
  // lexicon
  if (string_equals(config->player_strategy_params[1 - player_index]->kwg_name,
                    lexicon_name)) {
    config->player_strategy_params[player_index]->kwg_name =
        get_formatted_string(
            "%s", config->player_strategy_params[1 - player_index]->kwg_name);
    config->player_strategy_params[player_index]->kwg =
        config->player_strategy_params[1 - player_index]->kwg;
    return;
  }
  bool kwg_was_shared =
      config->player_strategy_params[player_index]->kwg &&
      config->player_strategy_params[1 - player_index]->kwg &&
      config->player_strategy_params[player_index]->kwg ==
          config->player_strategy_params[1 - player_index]->kwg;

  destroy_kwg(config->player_strategy_params[player_index]->kwg);
  free(config->player_strategy_params[player_index]->kwg_name);
  if (kwg_was_shared) {
    free(config->player_strategy_params[1 - player_index]->kwg_name);
  }

  config->player_strategy_params[player_index]->kwg = create_kwg(lexicon_name);
  config->player_strategy_params[player_index]->kwg_name =
      get_formatted_string("%s", lexicon_name);
}

config_load_status_t load_leaves_for_config(Config *config,
                                            const char *leaves_name,
                                            int player_index) {
  // Check if kwg is already defined
  if (string_equals(config->player_strategy_params[player_index]->klv_name,
                    leaves_name)) {
    return;
  }
  // Check if the other player has already loaded the same
  // leaves
  if (string_equals(config->player_strategy_params[1 - player_index]->klv_name,
                    leaves_name)) {
    config->player_strategy_params[player_index]->klv_name =
        get_formatted_string(
            "%s", config->player_strategy_params[1 - player_index]->klv_name);
    config->player_strategy_params[player_index]->klv =
        config->player_strategy_params[1 - player_index]->klv;
    return;
  }
  bool klv_was_shared =
      config->player_strategy_params[player_index]->klv &&
      config->player_strategy_params[1 - player_index]->klv &&
      config->player_strategy_params[player_index]->klv ==
          config->player_strategy_params[1 - player_index]->klv;

  destroy_klv(config->player_strategy_params[player_index]->klv);
  free(config->player_strategy_params[player_index]->klv_name);
  if (klv_was_shared) {
    free(config->player_strategy_params[1 - player_index]->klv_name);
  }
  config->player_strategy_params[player_index]->klv = create_klv(leaves_name);
  config->player_strategy_params[player_index]->klv_name =
      get_formatted_string("%s", leaves_name);
}

config_load_status_t load_move_sort_type_for_config(
    Config *config, const char *move_sort_type_string, int player_index) {
  config_load_status_t config_load_status = CONFIG_LOAD_STATUS_SUCCESS;
  if (strings_equal(move_sort_type_string, "equity")) {
    config->player_strategy_params[player_index]->move_sort_type =
        MOVE_SORT_EQUITY;
  } else if (strings_equal(move_sort_type_string, "score")) {
    config->player_strategy_params[player_index]->move_sort_type =
        MOVE_SORT_SCORE;
  } else {
    config_load_status = CONFIG_LOAD_STATUS_MALFORMED_MOVE_SORT_TYPE;
  }
  return config_load_status;
}

config_load_status_t load_move_record_type_for_config(
    Config *config, const char *move_record_type_string, int player_index) {
  config_load_status_t config_load_status = CONFIG_LOAD_STATUS_SUCCESS;
  if (strings_equal(move_record_type_string, "best") ||
      strings_equal(move_record_type_string, "top")) {
    config->player_strategy_params[player_index]->move_record_type =
        MOVE_RECORD_BEST;
  } else if (strings_equal(move_record_type_string, "all")) {
    config->player_strategy_params[player_index]->move_record_type =
        MOVE_RECORD_ALL;
  } else {
    config_load_status = CONFIG_LOAD_STATUS_MALFORMED_MOVE_RECORD_TYPE;
  }
  return config_load_status;
}

config_load_status_t load_rack_for_config(Config *config, const char *rack) {
  if (!config->letter_distribution) {
    return CONFIG_LOAD_STATUS_MISSING_LETTER_DISTRIBUTION;
  }
  if (!config->rack) {
    config->rack = create_rack();
  }
  set_rack_to_string(config->rack, rack, config->letter_distribution);
  return CONFIG_LOAD_STATUS_SUCCESS;
}

int set_command_type_for_config(Config *config, ParsedArgs *parsed_args) {
  command_t cmd_type = COMMAND_TYPE_UNKNOWN;
  int number_of_tokens_parsed = 0;
  for (int i = 0; VALID_COMMAND_SEQUENCES.cs[i].ct != COMMAND_TYPE_UNKNOWN;
       i++) {
    bool sequence_matches = true;
    number_of_tokens_parsed = 0;
    for (int j = 0;
         VALID_COMMAND_SEQUENCES.cs[i].ats[j] != NUMBER_OF_ARG_TOKENS; j++) {
      arg_token_t arg_token = VALID_COMMAND_SEQUENCES.cs[i].ats[j];
      if (!parsed_args->args[j]->has_value ||
          parsed_args->args[j]->token != VALID_COMMAND_SEQUENCES.cs[i].ats[j]) {
        sequence_matches = false;
        break;
      }
      number_of_tokens_parsed++;
    }
    if (sequence_matches) {
      config->command_type = VALID_COMMAND_SEQUENCES.cs[i].ct;
      break;
    }
  }
  return number_of_tokens_parsed;
}

config_load_status_t is_valid_command_type_for_arg(Config *config,
                                                   ParsedArgs *parsed_args,
                                                   int index) {
  return
      // Args with an unknown command type are always allowed
      parsed_args->args[index]->command_type == COMMAND_TYPE_UNKNOWN ||
      // When setting options (and not running any command), all arg types are
      // valid
      config->command_type == COMMAND_TYPE_SET_OPTIONS ||
      // Otherwise, the arg token must be valid for the command
      parsed_args->args[index]->command_type == config->command_type;
}

config_load_status_t load_config_with_parsed_args(Config *config,
                                                  ParsedArgs *parsed_args) {

  int start_token_index = set_command_type_for_config(config, parsed_args);

  config_load_status_t config_load_status = CONFIG_LOAD_STATUS_SUCCESS;
  for (int i = start_token_index; i < NUMBER_OF_ARG_TOKENS; i++) {
    if (!parsed_args->has_value[i]) {
      continue;
    }
    if (!is_valid_command_type_for_arg(config, parsed_args, i)) {
      config_load_status = CONFIG_LOAD_STATUS_INVALID_ARG_FOR_COMMAND;
      break;
    }
    arg_token_t arg_token = parsed_args->tokens[i];
    int arg_position = parsed_args->arg_positions[i];
    char **arg_values = parsed_args->values[i];
    switch (arg_token) {
    case ARG_TOKEN_POSITION:
    case ARG_TOKEN_GO:
    case ARG_TOKEN_CGP:
    case ARG_TOKEN_SIM:
    case ARG_TOKEN_INFER:
    case ARG_TOKEN_AUTOPLAY:
      config_load_status = CONFIG_LOAD_STATUS_MISPLACED_COMMAND;
      break;
    case ARG_TOKEN_CGP_BINGO_BONUS:
      config_load_status = load_bingo_bonus_for_config(config, arg_values[0]);
      break;
    case ARG_TOKEN_CGP_BOARD_LAYOUT:
      config_load_status = load_board_layout_for_config(config, arg_values[0]);
      break;
    case ARG_TOKEN_CGP_GAME_VARIANT:
      config_load_status = load_game_variant_for_config(config, arg_values[0]);
      break;
    case ARG_TOKEN_CGP_LETTER_DISTRIBUTION:
      config_load_status =
          load_letter_distribution_for_config(config, arg_values[0]);
      break;
    case ARG_TOKEN_CGP_LEXICON:
      config_load_status = load_lexicon_for_config(config, arg_values[0], 0);
      break;
    case ARG_TOKEN_P1_LEXICON:
      config_load_status = load_lexicon_for_config(config, arg_values[0], 0);
      break;
    case ARG_TOKEN_P1_LEAVES:
      config_load_status = load_lexicon_for_config(config, arg_values[0], 0);
      break;
    case ARG_TOKEN_P1_MOVE_SORT_TYPE:
      config_load_status =
          load_move_sort_type_for_config(config, arg_values[0], 0);
      break;
    case ARG_TOKEN_P1_MOVE_RECORD_TYPE:
      config_load_status =
          load_move_record_type_for_config(config, arg_values[0], 0);
      break;
    case ARG_TOKEN_P2_LEXICON:
      config_load_status = load_lexicon_for_config(config, arg_values[0], 1);
      break;
    case ARG_TOKEN_P2_LEAVES:
      config_load_status = load_lexicon_for_config(config, arg_values[0], 1);
      break;
    case ARG_TOKEN_P2_MOVE_SORT_TYPE:
      config_load_status =
          load_move_sort_type_for_config(config, arg_values[0], 1);
      break;
    case ARG_TOKEN_P2_MOVE_RECORD_TYPE:
      config_load_status =
          load_move_record_type_for_config(config, arg_values[0], 1);
      break;
    case ARG_TOKEN_WIN_PCT_FILENAME:
      config_load_status = load_winpct_for_config(config, arg_values[0]);
      break;
    case ARG_TOKEN_MOVE_LIST_CAPACITY:
      config_load_status =
          load_move_list_capacity_for_config(config, arg_values[0]);
      break;
    case ARG_TOKEN_KNOWN_OPP_RACK:
      config_load_status = load_rack_for_config(config, arg_values[0]);
      break;
    case NUMBER_OF_ARG_TOKENS:
      log_fatal("invalid token found in args\n");
      break;
    }
    if (config_load_status != CONFIG_LOAD_STATUS_SUCCESS) {
      break;
    }
  }
  return config_load_status;
}

config_load_status_t load_config(Config *config, const char *cmd) {
  StringSplitter *cmd_split_string = split_string_by_whitespace(cmd, true);
  ParsedArgs *parsed_args = create_parsed_args(cmd_split_string);

  config_load_status_t config_load_status =
      init_parsed_args(parsed_args, cmd_split_string);

  if (config_load_status == CONFIG_LOAD_STATUS_SUCCESS) {
    config_load_status = load_config_with_parsed_args(config, parsed_args);
  }

  destroy_parsed_args(parsed_args);
  destroy_string_splitter(cmd_split_string);

  if (config->command_type == COMMAND_TYPE_LOAD_CGP) {
    finalize_config_for_load_cgp(config);
  }

  return config_load_status;
}

// Strategy params

StrategyParams *create_strategy_params() {
  StrategyParams *strategy_params = malloc_or_die(sizeof(StrategyParams));
  strategy_params->klv = NULL;
  strategy_params->klv_name = NULL;
  strategy_params->kwg = NULL;
  strategy_params->kwg_name;
  strategy_params->move_sort_type = DEFAULT_MOVE_SORT_TYPE;
  strategy_params->move_record_type = DEFAULT_MOVE_RECORD_TYPE;
  return strategy_params;
}

void destroy_strategy_params(StrategyParams *strategy_params, bool destroy_kwg,
                             bool destroy_klv) {
  if (destroy_klv) {
    destroy_klv(strategy_params->klv);
  }
  if (destroy_kwg) {
    destroy_kwg(strategy_params->kwg);
  }
  if (strategy_params->klv_name) {
    free(strategy_params->klv_name);
  }
  if (strategy_params->kwg_name) {
    free(strategy_params->kwg_name);
  }
  free(strategy_params);
}

Config *create_config() {
  Config *config = malloc_or_die(sizeof(Config));
  config->command_type = COMMAND_TYPE_UNKNOWN;
  config->letter_distribution = NULL;
  config->ld_name = NULL;
  config->cgp = NULL;
  config->bingo_bonus = DEFAULT_BINGO_BONUS;
  config->board_layout = DEFAULT_BOARD_LAYOUT;
  config->game_variant = DEFAULT_GAME_VARIANT;
  config->kwg_is_shared = false;
  config->klv_is_shared = false;
  config->player_strategy_params[0] = create_strategy_params();
  config->player_strategy_params[1] = create_strategy_params();
  config->rack = NULL;
  config->player_to_infer_index = 0;
  config->actual_score = 0;
  config->number_of_tiles_exchanged = 0;
  config->equity_margin = 0;
  config->win_pcts = NULL;
  config->win_pct_name = NULL;
  config->move_list_capacity = DEFAULT_MOVE_LIST_CAPACITY;
  config->use_game_pairs = true;
  config->number_of_games_or_pairs = 0;
  config->thread_control = NULL;
  return config;
}

void destroy_config(Config *config) {
  if (config->letter_distribution) {
    destroy_letter_distribution(config->letter_distribution);
  }
  if (config->ld_name) {
    free(config->ld_name);
  }
  if (config->cgp) {
    free(config->cgp);
  }
  destroy_strategy_params(config->player_strategy_params[0], true, true);
  destroy_strategy_params(config->player_strategy_params[1],
                          !config->kwg_is_shared, !config->klv_is_shared);
  if (config->rack) {
    destroy_rack(config->rack);
  }
  if (config->win_pcts) {
    destroy_winpct(config->win_pcts);
  }
  if (config->win_pct_name) {
    free(config->win_pct_name);
  }
  if (config->thread_control) {
    destroy_thread_control(config->thread_control);
  }
}