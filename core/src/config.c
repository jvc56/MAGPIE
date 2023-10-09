#include <assert.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cgp.h"
#include "config.h"
#include "constants.h"
#include "klv.h"
#include "kwg.h"
#include "letter_distribution.h"
#include "log.h"
#include "string_util.h"
#include "util.h"

#define MAX_NUMBER_OF_ARGS 100
#define MAX_COMMAND_SEQUENCE_LENGTH 10
#define MAX_NUMBER_OF_COMMANDS 10

// Strategy params

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

// Parsed Args
typedef enum {
  ARG_TOKEN_UNKNOWN,
  ARG_TOKEN_CGP,
  ARG_TOKEN_POSITION,
  ARG_TOKEN_GO,
  ARG_TOKEN_SIM,
  ARG_TOKEN_INFER,
  ARG_TOKEN_AUTOPLAY,
  ARG_TOKEN_BINGO_BONUS,
  ARG_TOKEN_BOARD_LAYOUT,
  ARG_TOKEN_GAME_VARIANT,
  ARG_TOKEN_LETTER_DISTRIBUTION,
  ARG_TOKEN_LEXICON,
  ARG_TOKEN_WIN_PCT_FILENAME,
  ARG_TOKEN_MOVE_LIST_CAPACITY,
  ARG_TOKEN_TILES_PLAYED,
  ARG_TOKEN_PLAYER_TO_INFER_INDEX,
  ARG_TOKEN_SCORE,
  ARG_TOKEN_EQUITY_MARGIN,
} arg_token_t;

typedef struct ParsedArgs {
  int arg_index;
  int number_of_args;
  arg_token_t *tokens[MAX_NUMBER_OF_ARGS];
  char **names[MAX_NUMBER_OF_ARGS];
  char *values[MAX_NUMBER_OF_ARGS];
} ParsedArgs;

void set_parsed_arg_token_and_name(ParsedArgs *parsed_args,
                                   arg_token_t arg_token,
                                   const char *arg_name) {
  parsed_args->tokens[parsed_args->number_of_args] = arg_token;
  parsed_args->names[parsed_args->number_of_args] =
      get_formatted_string("%s", arg_name);
  parsed_args->values[parsed_args->number_of_args] = NULL;
  parsed_args->number_of_args++;
}

ParsedArgs *create_parsed_args(StringSplitter *cmd) {
  ParsedArgs *parsed_args = malloc_or_die(sizeof(ParsedArgs));
  parsed_args->number_of_args = 0;
  parsed_args->arg_index = 0;

  set_parsed_arg_token_and_name(parsed_args, ARG_TOKEN_POSITION, "position");
  set_parsed_arg_token_and_name(parsed_args, ARG_TOKEN_CGP, "cgp");
  set_parsed_arg_token_and_name(parsed_args, ARG_TOKEN_GO, "go");
  set_parsed_arg_token_and_name(parsed_args, ARG_TOKEN_SIM, "sim");
  set_parsed_arg_token_and_name(parsed_args, ARG_TOKEN_INFER, "infer");
  set_parsed_arg_token_and_name(parsed_args, ARG_TOKEN_AUTOPLAY, "autoplay");
  // *** These are CGP operations and must match the CGP spec: ***
  set_parsed_arg_token_and_name(parsed_args, ARG_TOKEN_BINGO_BONUS, "bb");
  set_parsed_arg_token_and_name(parsed_args, ARG_TOKEN_BOARD_LAYOUT, "bdn");
  set_parsed_arg_token_and_name(parsed_args, ARG_TOKEN_GAME_VARIANT, "var");
  set_parsed_arg_token_and_name(parsed_args, ARG_TOKEN_LETTER_DISTRIBUTION,
                                "ld");
  set_parsed_arg_token_and_name(parsed_args, ARG_TOKEN_LEXICON, "lex");
  // ***
  set_parsed_arg_token_and_name(parsed_args, ARG_TOKEN_WIN_PCT_FILENAME,
                                "winpct");
  set_parsed_arg_token_and_name(parsed_args, ARG_TOKEN_MOVE_LIST_CAPACITY,
                                "cap");
  set_parsed_arg_token_and_name(parsed_args, ARG_TOKEN_TILES_PLAYED,
                                "tilesplayed");
  set_parsed_arg_token_and_name(parsed_args, ARG_TOKEN_PLAYER_TO_INFER_INDEX,
                                "pindex");
  set_parsed_arg_token_and_name(parsed_args, ARG_TOKEN_SCORE, "score");
  set_parsed_arg_token_and_name(parsed_args, ARG_TOKEN_EQUITY_MARGIN, "eq");

  return parsed_args;
}

void destroy_parsed_args(ParsedArgs *parsed_args) {
  for (int i = 0; i < parsed_args->number_of_args; i++) {
    if (parsed_args->values[i]) {
      free(parsed_args->values[i]);
    }
    if (parsed_args->names[i]) {
      free(parsed_args->names[i]);
    }
  }
  free(parsed_args);
}

config_load_status_t init_parsed_args(ParsedArgs *parsed_args,
                                      StringSplitter *cmd) {
  int number_of_cmd_args = string_splitter_get_number_of_items(cmd);
  for (int i = 0; i < number_of_cmd_args; i++) {
    char *cmd_arg = string_splitter_get_item(cmd, i);
    bool is_valid_arg = false;
    for (int j = 0; j < parsed_args->number_of_args; j++) {
      if (strings_equal(cmd_arg, parsed_args->names[j])) {
        if (i < number_of_cmd_args - 1) {
          parsed_args->values[j] =
              get_formatted_string("%s", string_splitter_get_item(cmd, i + 1));
        } else {
          // This could be a standalone flag argument, so set it to
          // the empty string so it isn't NULL.
          parsed_args->values[j] = get_formatted_string("%s", "");
        }
        is_valid_arg = true;
        break;
      }
    }
    if (!is_valid_arg) {
      return CONFIG_LOAD_STATUS_UNRECOGNIZED_ARG;
    }
  }
}

// Command Sequences

typedef struct CommandSequences {
  int number_of_sequences;
  command_t command_types[MAX_NUMBER_OF_COMMANDS];
  arg_token_t sequences[MAX_NUMBER_OF_COMMANDS]
                       [MAX_COMMAND_SEQUENCE_LENGTH + 1];
} CommandSequences;

void add_command_sequence(CommandSequences *command_sequences,
                          command_t command_type, arg_token_t *sequence) {
  int index = command_sequences->number_of_sequences;
  command_sequences->command_type[index] = command_type;
  // Unknown arg token denotes the end of the sequence.
  for (i = 0; i < MAX_COMMAND_SEQUENCE_LENGTH; i++) {
    command_sequences->sequences[index]->[i] = sequence[i];
    if (sequence[i] == ARG_TOKEN_UNKNOWN) {
      break;
    }
  }
  command_sequences->number_of_sequences++;
}

CommandSequences *create_command_sequences() {
  CommandSequences *command_sequences = malloc_or_die(sizeof(CommandSequences));
  command_sequences->number_of_sequences = 0;

  add_command_sequence(command_sequences, COMMAND_TYPE_LOAD_CGP,
                       (const arg_token_t[]){ARG_TOKEN_CGP, ARG_TOKEN_POSITION,
                                             ARG_TOKEN_UNKNOWN});
  add_command_sequence(
      command_sequences, COMMAND_TYPE_SIM,
      (const arg_token_t[]){ARG_TOKEN_GO, ARG_TOKEN_SIM, ARG_TOKEN_UNKNOWN});

  add_command_sequence(
      command_sequences, COMMAND_TYPE_INFER,
      (const arg_token_t[]){ARG_TOKEN_GO, ARG_TOKEN_INFER, ARG_TOKEN_UNKNOWN});

  add_command_sequence(command_sequences, COMMAND_TYPE_AUTOPLAY,
                       (const arg_token_t[]){ARG_TOKEN_GO, ARG_TOKEN_AUTOPLAY,
                                             ARG_TOKEN_UNKNOWN});
  return command_sequences;
}

void destroy_command_sequences(CommandSequences *command_sequences) {
  free(command_sequences);
}

config_load_status_t load_winpct_for_config(Config *config,
                                            const char *win_pct_filename) {
  if (config->win_pct_filename &&
      strings_equal(config->win_pct_filename, win_pct_filename)) {
    return;
  }
  WinPct *new_win_pcts = create_winpct(win_pct_filename);
  if (config->win_pcts) {
    destroy_winpct(config->win_pcts);
  }
  config->win_pcts = new_win_pcts;

  if (config->win_pct_filename) {
    free(config->win_pct_filename);
  }
  config->win_pct_filename = get_formatted_string("%s", win_pct_filename);
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
  config->bingo_bonus = string_to_int(bingo_bonus);
  return CONFIG_LOAD_STATUS_SUCCESS;
}

config_load_status_t load_tiles_played_for_config(Config *config,
                                                  const char *tiles_played) {
  if (config->letter_distribution) {
    return CONFIG_LOAD_STATUS_MISSING_LETTER_DISTRIBUTION;
  }
  if (!config->actual_tiles_played) {
    config->actual_tiles_played = create_rack();
  }
  set_rack_to_string(config->actual_tiles_played, tiles_played,
                     config->letter_distribution);
  return CONFIG_LOAD_STATUS_SUCCESS;
}

config_load_status_t load_config_with_parsed_args(Config *config,
                                                  ParsedArgs *parsed_args) {
  config_load_status_t config_load_status = CONFIG_LOAD_STATUS_SUCCESS;
  for (int i = parsed_args->arg_index; i < parsed_args->number_of_args; i++) {
    arg_token_t arg_token = parsed_args->tokens[i];
    char *arg_value = parsed_args->values[i];
    switch (arg_token) {
    case ARG_TOKEN_UNKNOWN:
    case ARG_TOKEN_CGP:
    case ARG_TOKEN_POSITION:
    case ARG_TOKEN_GO:
    case ARG_TOKEN_SIM:
    case ARG_TOKEN_INFER:
    case ARG_TOKEN_AUTOPLAY:
      config_load_status = CONFIG_LOAD_STATUS_UNRECOGNIZED_ARG;
      break;
    case ARG_TOKEN_BINGO_BONUS:
      config_load_status = load_bingo_bonus_for_config(config, arg_value);
      break;
    case ARG_TOKEN_WIN_PCT_FILENAME:
      config_load_status = load_winpct_for_config(config, arg_value);
      break;
    case ARG_TOKEN_MOVE_LIST_CAPACITY:
      config_load_status =
          load_move_list_capacity_for_config(config, arg_value);
      break;
    case ARG_TOKEN_TILES_PLAYED:
      config_load_status = load_tiles_played_for_config(config, arg_value);
      break;
    }
    if (config_load_status != CONFIG_LOAD_STATUS_SUCCESS) {
      break;
    }
  }
  return config_load_status;
}

command_t get_command_type_from_parsed_args(ParsedArgs *parsed_args) {
  CommandSequences *command_sequences = create_command_sequences();
  command_t command_type = COMMAND_TYPE_UNKNOWN;
  for (int i = 0; i < command_sequences->number_of_sequences; i++) {
    bool command_sequence_matches = true;
    int j = 0;
    for (; command_sequences->sequences[i][j] != ARG_TOKEN_UNKNOWN &&
           j < parsed_args->number_of_args;
         j++) {
      if (command_sequences[i]->sequences[j] != parsed_args->tokens[j]) {
        command_sequence_matches = false;
        break;
      }
    }
    if (command_sequence_matches && parsed_args->number_of_args == j) {
      command_type = command_sequences->command_types[i];
      parsed_args->arg_index = j;
      break;
    }
  }
  return command_type;
}

config_load_status_t finalize_config(Config *config) {
  // Set leaves from lexicon
  // Set ld from lexicon
  // Set default move sort
  // set default move recorder
  return;
}

config_load_status_t load_config(Config *config, const char *cmd) {
  StringSplitter *cmd_split_string = split_string_by_whitespace(cmd, true);
  ParsedArgs *parsed_args = create_parsed_args(cmd_split_string);

  config_load_status_t config_load_status =
      init_parsed_args(parsed_args, cmd_split_string);

  if (config_load_status == CONFIG_LOAD_STATUS_SUCCESS) {
    config->command_type = get_command_type_from_parsed_args(parsed_args);
    if (config->command_type != COMMAND_TYPE_UNKNOWN) {
      config_load_status = load_config_with_parsed_args(config, parsed_args);
    } else {
      config_load_status = CONFIG_LOAD_STATUS_UNRECOGNIZED_COMMAND;
    }
  }
  destroy_parsed_args(parsed_args);
  destroy_string_splitter(cmd_split_string);

  if (config->command_type == COMMAND_TYPE_LOAD_CGP) {
    finalize_config_for_load_cgp(config);
  }

  return config_load_status;
}

Config *create_config() { return malloc_or_die(sizeof(Config)); }

void destroy_config(Config *config) { abort(); }