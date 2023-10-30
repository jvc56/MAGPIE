#include <assert.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "config.h"
#include "constants.h"
#include "game.h"
#include "klv.h"
#include "kwg.h"
#include "letter_distribution.h"
#include "log.h"
#include "players_data.h"
#include "string_util.h"
#include "util.h"

#define DEFAULT_BINGO_BONUS 50
#define DEFAULT_BOARD_LAYOUT BOARD_LAYOUT_CROSSWORD_GAME
#define DEFAULT_GAME_VARIANT GAME_VARIANT_CLASSIC
#define DEFAULT_MOVE_LIST_CAPACITY 1
#define DEFAULT_SIMMING_STOPPING_CONDITION SIM_STOPPING_CONDITION_NONE

#define ARG_POSITION "position"
#define ARG_CGP "cgp"
#define ARG_GO "go"
#define ARG_SIM "sim"
#define ARG_INFER "infer"
#define ARG_AUTOPLAY "autoplay"
#define ARG_SET_OPTIONS "setoptions"
#define ARG_BINGO_BONUS "bb"
#define ARG_BOARD_LAYOUT "bdn"
#define ARG_GAME_VARIANT "var"
#define ARG_LETTER_DISTRIBUTION "ld"
#define ARG_LEXICON "lex"
#define ARG_P1_NAME "p1"
#define ARG_P1_LEXICON "l1"
#define ARG_P1_LEAVES "k1"
#define ARG_P1_MOVE_SORT_TYPE "s1"
#define ARG_P1_MOVE_RECORD_TYPE "r1"
#define ARG_P2_NAME "p2"
#define ARG_P2_LEXICON "l2"
#define ARG_P2_LEAVES "k2"
#define ARG_P2_MOVE_SORT_TYPE "s2"
#define ARG_P2_MOVE_RECORD_TYPE "r2"
#define ARG_KNOWN_OPP_RACK "rack"
#define ARG_WIN_PCT "winpct"
#define ARG_PLIES "plies"
#define ARG_NUMBER_OF_PLAYS "numplays"
#define ARG_MAX_ITERATIONS "i"
#define ARG_STOPPING_CONDITION "stop"
#define ARG_STATIC_SEARCH_ON "static"
#define ARG_STATIC_SEARCH_OFF "nostatic"
#define ARG_PLAYER_TO_INFER_INDEX "pindex"
#define ARG_SCORE "score"
#define ARG_EQUITY_MARGIN "eq"
#define ARG_NUMBER_OF_TILES_EXCHANGED "exch"
#define ARG_GAME_PAIRS_ON "gp"
#define ARG_GAME_PAIRS_OFF "nogp"
#define ARG_RANDOM_SEED "rs"
#define ARG_NUMBER_OF_THREADS "threads"
#define ARG_PRINT_INFO_INTERVAL "info"
#define ARG_CHECK_STOP_INTERVAL "check"
#define ARG_INFILE "infile"
#define ARG_OUTFILE "outfile"
#define ARG_ERRORFILE "errorfile"
#define ARG_CONSOLE_MODE "console"
#define ARG_UCGI_MODE "ucgi"
#define ARG_COMMAND_FILE "file"

#define ARG_VAL_MOVE_SORT_EQUITY "equity"
#define ARG_VAL_MOVE_SORT_SCORE "score"

#define ARG_VAL_MOVE_RECORD_BEST "best"
#define ARG_VAL_MOVE_RECORD_ALL "all"

typedef enum {
  // Commands
  ARG_TOKEN_POSITION,
  ARG_TOKEN_CGP,
  ARG_TOKEN_GO,
  ARG_TOKEN_SIM,
  ARG_TOKEN_INFER,
  ARG_TOKEN_AUTOPLAY,
  ARG_TOKEN_SET_OPTIONS,
  // Game
  // shared between players
  ARG_TOKEN_BINGO_BONUS,
  ARG_TOKEN_BOARD_LAYOUT,
  ARG_TOKEN_GAME_VARIANT,
  ARG_TOKEN_LETTER_DISTRIBUTION,
  ARG_TOKEN_LEXICON,
  // possibly unique for each player
  ARG_TOKEN_P1_NAME,
  ARG_TOKEN_P1_LEXICON,
  ARG_TOKEN_P1_LEAVES,
  ARG_TOKEN_P1_MOVE_SORT_TYPE,
  ARG_TOKEN_P1_MOVE_RECORD_TYPE,
  ARG_TOKEN_P2_NAME,
  ARG_TOKEN_P2_LEXICON,
  ARG_TOKEN_P2_LEAVES,
  ARG_TOKEN_P2_MOVE_SORT_TYPE,
  ARG_TOKEN_P2_MOVE_RECORD_TYPE,
  // Sim
  ARG_TOKEN_KNOWN_OPP_RACK,
  ARG_TOKEN_WIN_PCT,
  ARG_TOKEN_PLIES,
  ARG_TOKEN_NUMBER_OF_PLAYS,
  ARG_TOKEN_MAX_ITERATIONS,
  ARG_TOKEN_STOPPING_CONDITION,
  ARG_TOKEN_STATIC_SEARCH_ON,
  ARG_TOKEN_STATIC_SEARCH_OFF,
  // Infer
  // Rack is shared with sim
  ARG_TOKEN_PLAYER_TO_INFER_INDEX,
  ARG_TOKEN_SCORE,
  ARG_TOKEN_EQUITY_MARGIN,
  ARG_TOKEN_NUMBER_OF_TILES_EXCHANGED,
  // Autoplay
  ARG_TOKEN_GAME_PAIRS_ON,
  ARG_TOKEN_GAME_PAIRS_OFF,
  ARG_TOKEN_RANDOM_SEED,
  // number of iterations shared with sim
  // Thread Control
  ARG_TOKEN_NUMBER_OF_THREADS,
  ARG_TOKEN_PRINT_INFO_INTERVAL,
  ARG_TOKEN_CHECK_STOP_INTERVAL,
  ARG_TOKEN_INFILE,
  ARG_TOKEN_OUTFILE,
  ARG_TOKEN_ERRORFILE,

  ARG_TOKEN_CONSOLE_MODE,
  ARG_TOKEN_UCGI_MODE,
  ARG_TOKEN_COMMAND_FILE,
  // This must always be the last
  // token for the count to be accurate
  NUMBER_OF_ARG_TOKENS
} arg_token_t;

// Valid command sequences

const struct {
  // The command type for this sequence
  command_t command_type;
  // The sequence of tokens associated with this command type
  arg_token_t arg_token_sequence[3];
} VALID_COMMAND_SEQUENCES[6] = {
    // The valid sequences
    // The NUMBER_OF_ARG_TOKENS denotes the end of the given sequence
    {COMMAND_TYPE_LOAD_CGP, {ARG_TOKEN_POSITION, NUMBER_OF_ARG_TOKENS}},
    {COMMAND_TYPE_SIM, {ARG_TOKEN_GO, ARG_TOKEN_SIM, NUMBER_OF_ARG_TOKENS}},
    {COMMAND_TYPE_INFER, {ARG_TOKEN_GO, ARG_TOKEN_INFER, NUMBER_OF_ARG_TOKENS}},
    {COMMAND_TYPE_AUTOPLAY,
     {ARG_TOKEN_GO, ARG_TOKEN_AUTOPLAY, NUMBER_OF_ARG_TOKENS}},
    {COMMAND_TYPE_SET_OPTIONS, {ARG_TOKEN_SET_OPTIONS, NUMBER_OF_ARG_TOKENS}},
    // The unknown command type denotes the end of the sequences
    {COMMAND_TYPE_UNKNOWN, {}},
};

typedef struct SingleArg {
  arg_token_t token;
  char *name;
  bool has_value;
  int number_of_values;
  char **values;
} SingleArg;

typedef struct ParsedArgs {
  SingleArg *args[NUMBER_OF_ARG_TOKENS];
} ParsedArgs;

SingleArg *create_single_arg() {
  SingleArg *single_arg = malloc_or_die(sizeof(SingleArg));
  return single_arg;
}

void destroy_single_arg(SingleArg *single_arg) {
  if (single_arg->name) {
    free(single_arg->name);
  }
  if (single_arg->has_value) {
    for (int i = 0; i < single_arg->number_of_values; i++) {
      if (single_arg->values[i]) {
        free(single_arg->values[i]);
      }
    }
  }
  free(single_arg->values);
  free(single_arg);
}

void set_single_arg(ParsedArgs *parsed_args, int index, arg_token_t arg_token,
                    const char *arg_name, int number_of_values) {
  SingleArg *single_arg = create_single_arg();
  single_arg->token = arg_token;
  single_arg->name = get_formatted_string("%s", arg_name);
  single_arg->number_of_values = number_of_values;
  single_arg->values = NULL;
  if (number_of_values > 0) {
    single_arg->values = malloc_or_die(sizeof(char *) * number_of_values);
  }
  single_arg->has_value = false;
  parsed_args->args[index] = single_arg;
}

ParsedArgs *create_parsed_args() {
  ParsedArgs *parsed_args = malloc_or_die(sizeof(ParsedArgs));

  int index = 0;
  // Command args
  set_single_arg(parsed_args, index++, ARG_TOKEN_POSITION, ARG_POSITION, 0);
  set_single_arg(parsed_args, index++, ARG_TOKEN_CGP, ARG_CGP, 4);
  set_single_arg(parsed_args, index++, ARG_TOKEN_GO, ARG_GO, 0);
  set_single_arg(parsed_args, index++, ARG_TOKEN_SIM, ARG_SIM, 0);
  set_single_arg(parsed_args, index++, ARG_TOKEN_INFER, ARG_INFER, 0);
  set_single_arg(parsed_args, index++, ARG_TOKEN_AUTOPLAY, ARG_AUTOPLAY, 0);
  set_single_arg(parsed_args, index++, ARG_TOKEN_SET_OPTIONS, ARG_SET_OPTIONS,
                 0);

  // CGP args
  set_single_arg(parsed_args, index++, ARG_TOKEN_BINGO_BONUS, ARG_BINGO_BONUS,
                 1);
  set_single_arg(parsed_args, index++, ARG_TOKEN_BOARD_LAYOUT, ARG_BOARD_LAYOUT,
                 1);
  set_single_arg(parsed_args, index++, ARG_TOKEN_GAME_VARIANT, ARG_GAME_VARIANT,
                 1);
  set_single_arg(parsed_args, index++, ARG_TOKEN_LETTER_DISTRIBUTION,
                 ARG_LETTER_DISTRIBUTION, 1);
  set_single_arg(parsed_args, index++, ARG_TOKEN_LEXICON, ARG_LEXICON, 1);

  // Game args
  // Player 1
  set_single_arg(parsed_args, index++, ARG_TOKEN_P1_NAME, ARG_P1_NAME, 1);
  set_single_arg(parsed_args, index++, ARG_TOKEN_P1_LEXICON, ARG_P1_LEXICON, 1);
  set_single_arg(parsed_args, index++, ARG_TOKEN_P1_LEAVES, ARG_P1_LEAVES, 1);
  set_single_arg(parsed_args, index++, ARG_TOKEN_P1_MOVE_SORT_TYPE,
                 ARG_P1_MOVE_SORT_TYPE, 1);
  set_single_arg(parsed_args, index++, ARG_TOKEN_P1_MOVE_RECORD_TYPE,
                 ARG_P1_MOVE_RECORD_TYPE, 1);

  // Player 2
  set_single_arg(parsed_args, index++, ARG_TOKEN_P2_NAME, ARG_P2_NAME, 1);
  set_single_arg(parsed_args, index++, ARG_TOKEN_P2_LEXICON, ARG_P2_LEXICON, 1);
  set_single_arg(parsed_args, index++, ARG_TOKEN_P2_LEAVES, ARG_P2_LEAVES, 1);
  set_single_arg(parsed_args, index++, ARG_TOKEN_P2_MOVE_SORT_TYPE,
                 ARG_P2_MOVE_SORT_TYPE, 1);
  set_single_arg(parsed_args, index++, ARG_TOKEN_P2_MOVE_RECORD_TYPE,
                 ARG_P2_MOVE_RECORD_TYPE, 1);

  // Sim args
  set_single_arg(parsed_args, index++, ARG_TOKEN_KNOWN_OPP_RACK,
                 ARG_KNOWN_OPP_RACK, 1);
  set_single_arg(parsed_args, index++, ARG_TOKEN_WIN_PCT, ARG_WIN_PCT, 1);
  set_single_arg(parsed_args, index++, ARG_TOKEN_PLIES, ARG_PLIES, 1);
  set_single_arg(parsed_args, index++, ARG_TOKEN_NUMBER_OF_PLAYS,
                 ARG_NUMBER_OF_PLAYS, 1);
  set_single_arg(parsed_args, index++, ARG_TOKEN_MAX_ITERATIONS,
                 ARG_MAX_ITERATIONS, 1);
  set_single_arg(parsed_args, index++, ARG_TOKEN_STOPPING_CONDITION,
                 ARG_STOPPING_CONDITION, 1);
  set_single_arg(parsed_args, index++, ARG_TOKEN_STATIC_SEARCH_ON,
                 ARG_STATIC_SEARCH_ON, 0);
  set_single_arg(parsed_args, index++, ARG_TOKEN_STATIC_SEARCH_OFF,
                 ARG_STATIC_SEARCH_OFF, 0);

  // Inference args
  // rack is KNOWN_OPP_RACK shared with sim
  set_single_arg(parsed_args, index++, ARG_TOKEN_PLAYER_TO_INFER_INDEX,
                 ARG_PLAYER_TO_INFER_INDEX, 1);
  set_single_arg(parsed_args, index++, ARG_TOKEN_SCORE, ARG_SCORE, 1);
  set_single_arg(parsed_args, index++, ARG_TOKEN_EQUITY_MARGIN,
                 ARG_EQUITY_MARGIN, 1);
  set_single_arg(parsed_args, index++, ARG_TOKEN_NUMBER_OF_TILES_EXCHANGED,
                 ARG_NUMBER_OF_TILES_EXCHANGED, 1);
  // Autoplay
  set_single_arg(parsed_args, index++, ARG_TOKEN_GAME_PAIRS_ON,
                 ARG_GAME_PAIRS_ON, 0);
  set_single_arg(parsed_args, index++, ARG_TOKEN_GAME_PAIRS_OFF,
                 ARG_GAME_PAIRS_OFF, 0);
  set_single_arg(parsed_args, index++, ARG_TOKEN_RANDOM_SEED, ARG_RANDOM_SEED,
                 1);

  // number of iterations shared with sim
  // Thread Control
  set_single_arg(parsed_args, index++, ARG_TOKEN_NUMBER_OF_THREADS,
                 ARG_NUMBER_OF_THREADS, 1);
  set_single_arg(parsed_args, index++, ARG_TOKEN_PRINT_INFO_INTERVAL,
                 ARG_PRINT_INFO_INTERVAL, 1);
  set_single_arg(parsed_args, index++, ARG_TOKEN_CHECK_STOP_INTERVAL,
                 ARG_CHECK_STOP_INTERVAL, 1);
  set_single_arg(parsed_args, index++, ARG_TOKEN_INFILE, ARG_INFILE, 1);
  set_single_arg(parsed_args, index++, ARG_TOKEN_OUTFILE, ARG_OUTFILE, 1);
  set_single_arg(parsed_args, index++, ARG_TOKEN_ERRORFILE, ARG_ERRORFILE, 1);

  set_single_arg(parsed_args, index++, ARG_TOKEN_CONSOLE_MODE, ARG_CONSOLE_MODE,
                 0);
  set_single_arg(parsed_args, index++, ARG_TOKEN_UCGI_MODE, ARG_UCGI_MODE, 0);
  set_single_arg(parsed_args, index++, ARG_TOKEN_COMMAND_FILE, ARG_COMMAND_FILE,
                 1);

  assert(index == NUMBER_OF_ARG_TOKENS);

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
  for (int i = 0; i < number_of_input_args;) {
    const char *input_arg = string_splitter_get_item(cmd, i);
    bool is_recognized_arg = false;
    for (int j = 0; j < NUMBER_OF_ARG_TOKENS; j++) {
      SingleArg *single_arg = parsed_args->args[j];
      if (strings_equal(input_arg, single_arg->name)) {
        if (single_arg->has_value) {
          return CONFIG_LOAD_STATUS_DUPLICATE_ARG;
        } else if (i + single_arg->number_of_values < number_of_input_args) {
          for (int k = 0; k < single_arg->number_of_values; k++) {
            single_arg->values[k] = get_formatted_string(
                "%s", string_splitter_get_item(cmd, i + k + 1));
          }
          single_arg->has_value = true;
          i += single_arg->number_of_values + 1;
        } else {
          log_warn("argument %s has an insufficient number of values\n",
                   single_arg->name);
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

config_load_status_t load_move_sort_type_for_config(
    Config *config, const char *move_sort_type_string, int player_index) {
  config_load_status_t config_load_status = CONFIG_LOAD_STATUS_SUCCESS;
  if (strings_equal(move_sort_type_string, ARG_VAL_MOVE_SORT_EQUITY)) {
    players_data_set_move_sort_type(config->players_data, player_index,
                                    MOVE_SORT_EQUITY);
  } else if (strings_equal(move_sort_type_string, ARG_VAL_MOVE_SORT_SCORE)) {
    players_data_set_move_sort_type(config->players_data, player_index,
                                    MOVE_SORT_SCORE);
  } else {
    config_load_status = CONFIG_LOAD_STATUS_MALFORMED_MOVE_SORT_TYPE;
  }
  return config_load_status;
}

config_load_status_t load_move_record_type_for_config(
    Config *config, const char *move_record_type_string, int player_index) {
  config_load_status_t config_load_status = CONFIG_LOAD_STATUS_SUCCESS;
  if (strings_equal(move_record_type_string, ARG_VAL_MOVE_RECORD_BEST)) {
    players_data_set_move_record_type(config->players_data, player_index,
                                      MOVE_RECORD_BEST);
  } else if (strings_equal(move_record_type_string, ARG_VAL_MOVE_RECORD_ALL)) {
    players_data_set_move_record_type(config->players_data, player_index,
                                      MOVE_RECORD_ALL);
  } else {
    config_load_status = CONFIG_LOAD_STATUS_MALFORMED_MOVE_RECORD_TYPE;
  }
  return config_load_status;
}

config_load_status_t load_rack_for_config(Config *config, const char *rack) {
  if (!rack) {
    return CONFIG_LOAD_STATUS_SUCCESS;
  }
  if (!strings_equal(EMPTY_RACK_STRING, rack)) {
    // If rack is specified on cold start without a
    // lexicon, the letter distribution could be missing.
    if (!config->letter_distribution) {
      return CONFIG_LOAD_STATUS_LEXICON_MISSING;
    }
    int number_of_letters_set =
        set_rack_to_string(config->rack, rack, config->letter_distribution);
    if (number_of_letters_set < 0) {
      return CONFIG_LOAD_STATUS_MALFORMED_RACK;
    }
  } else {
    reset_rack(config->rack);
  }

  return CONFIG_LOAD_STATUS_SUCCESS;
}

config_load_status_t load_winpct_for_config(Config *config,
                                            const char *input_win_pct_name) {

  const char *win_pct_name = NULL;
  if (is_string_empty_or_null(input_win_pct_name)) {
    win_pct_name = DEFAULT_WIN_PCT;
  } else {
    win_pct_name = input_win_pct_name;
  }

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

config_load_status_t load_num_plays_for_config(Config *config,
                                               const char *num_plays) {
  if (!is_all_digits_or_empty(num_plays)) {
    return CONFIG_LOAD_STATUS_MALFORMED_NUM_PLAYS;
  }
  config->num_plays = string_to_int(num_plays);
  return CONFIG_LOAD_STATUS_SUCCESS;
}

config_load_status_t load_plies_for_config(Config *config, const char *plies) {
  if (!is_all_digits_or_empty(plies)) {
    return CONFIG_LOAD_STATUS_MALFORMED_PLIES;
  }
  config->plies = string_to_int(plies);
  return CONFIG_LOAD_STATUS_SUCCESS;
}

config_load_status_t
load_max_iterations_for_config(Config *config, const char *max_iterations) {
  if (!is_all_digits_or_empty(max_iterations)) {
    return CONFIG_LOAD_STATUS_MALFORMED_MAX_ITERATIONS;
  }
  config->max_iterations = string_to_int(max_iterations);
  return CONFIG_LOAD_STATUS_SUCCESS;
}

config_load_status_t
load_stopping_condition_for_config(Config *config,
                                   const char *stopping_condition) {
  if (strings_equal(stopping_condition, "none")) {
    config->stopping_condition = SIM_STOPPING_CONDITION_NONE;
  } else if (strings_equal(stopping_condition, "95")) {
    config->stopping_condition = SIM_STOPPING_CONDITION_95PCT;
  } else if (strings_equal(stopping_condition, "98")) {
    config->stopping_condition = SIM_STOPPING_CONDITION_98PCT;
  } else if (strings_equal(stopping_condition, "99")) {
    config->stopping_condition = SIM_STOPPING_CONDITION_99PCT;
  } else {
    return CONFIG_LOAD_STATUS_MALFORMED_STOPPING_CONDITION;
  }
  return CONFIG_LOAD_STATUS_SUCCESS;
}

config_load_status_t
load_static_search_only_for_config(Config *config, bool static_search_only) {
  config->static_search_only = static_search_only;
  return CONFIG_LOAD_STATUS_SUCCESS;
}

config_load_status_t
load_player_to_infer_index_for_config(Config *config,
                                      const char *player_to_infer_index) {
  if (!is_all_digits_or_empty(player_to_infer_index)) {
    return CONFIG_LOAD_STATUS_MALFORMED_PLAYER_TO_INFER_INDEX;
  }
  config->player_to_infer_index = string_to_int(player_to_infer_index);
  if (config->player_to_infer_index != 0 &&
      config->player_to_infer_index != 1) {
    return CONFIG_LOAD_STATUS_MALFORMED_PLAYER_TO_INFER_INDEX;
  }
  return CONFIG_LOAD_STATUS_SUCCESS;
}

config_load_status_t load_score_for_config(Config *config, const char *score) {
  if (!is_all_digits_or_empty(score)) {
    return CONFIG_LOAD_STATUS_MALFORMED_SCORE;
  }
  config->actual_score = string_to_int(score);
  return CONFIG_LOAD_STATUS_SUCCESS;
}

config_load_status_t
load_equity_margin_for_config(Config *config,
                              const char *equity_margin_string) {
  if (!is_decimal_number(equity_margin_string)) {
    return CONFIG_LOAD_STATUS_MALFORMED_EQUITY_MARGIN;
  }
  double equity_margin = string_to_double(equity_margin_string);
  if (equity_margin < 0) {
    return CONFIG_LOAD_STATUS_MALFORMED_EQUITY_MARGIN;
  }
  config->equity_margin = equity_margin;
  return CONFIG_LOAD_STATUS_SUCCESS;
}

config_load_status_t load_number_of_tiles_exchanged_for_config(
    Config *config, const char *number_of_tiles_exchanged) {
  if (!is_all_digits_or_empty(number_of_tiles_exchanged)) {
    return CONFIG_LOAD_STATUS_MALFORMED_NUMBER_OF_TILES_EXCHANGED;
  }
  config->number_of_tiles_exchanged = string_to_int(number_of_tiles_exchanged);
  return CONFIG_LOAD_STATUS_SUCCESS;
}

config_load_status_t load_use_game_pairs_for_config(Config *config,
                                                    bool use_game_pairs) {
  config->use_game_pairs = use_game_pairs;
  return CONFIG_LOAD_STATUS_SUCCESS;
}

config_load_status_t load_random_seed_for_config(Config *config,
                                                 const char *random_seed) {
  if (!is_all_digits_or_empty(random_seed)) {
    return CONFIG_LOAD_STATUS_MALFORMED_RANDOM_SEED;
  }
  config->random_seed = string_to_uint64(random_seed);
  return CONFIG_LOAD_STATUS_SUCCESS;
}

config_load_status_t
load_number_of_threads_for_config(Config *config,
                                  const char *number_of_tiles_threads) {
  if (!is_all_digits_or_empty(number_of_tiles_threads)) {
    return CONFIG_LOAD_STATUS_MALFORMED_NUMBER_OF_THREADS;
  }
  config->thread_control->number_of_threads =
      string_to_int(number_of_tiles_threads);
  if (config->thread_control->number_of_threads < 1) {
    return CONFIG_LOAD_STATUS_MALFORMED_NUMBER_OF_THREADS;
  }
  return CONFIG_LOAD_STATUS_SUCCESS;
}

config_load_status_t
load_print_info_interval_for_config(Config *config,
                                    const char *print_info_interval) {
  if (!is_all_digits_or_empty(print_info_interval)) {
    return CONFIG_LOAD_STATUS_MALFORMED_PRINT_INFO_INTERVAL;
  }
  config->thread_control->print_info_interval =
      string_to_int(print_info_interval);
  return CONFIG_LOAD_STATUS_SUCCESS;
}

config_load_status_t
load_check_stop_interval_for_config(Config *config,
                                    const char *check_stop_interval) {
  if (!is_all_digits_or_empty(check_stop_interval)) {
    return CONFIG_LOAD_STATUS_MALFORMED_CHECK_STOP_INTERVAL;
  }
  config->thread_control->check_stopping_condition_interval =
      string_to_int(check_stop_interval);
  return CONFIG_LOAD_STATUS_SUCCESS;
}

config_load_status_t load_outfile_for_config(Config *config,
                                             const char *outfile_name) {
  set_outfile(config->thread_control, outfile_name);
  return CONFIG_LOAD_STATUS_SUCCESS;
}

config_load_status_t load_infile_for_config(Config *config,
                                            const char *infile_name) {
  set_infile(config->thread_control, infile_name);
  return CONFIG_LOAD_STATUS_SUCCESS;
}

config_load_status_t load_errorfile_for_config(Config *config,
                                               const char *errorfile_name) {
  set_errorfile(config->thread_control, errorfile_name);
  return CONFIG_LOAD_STATUS_SUCCESS;
}

config_load_status_t load_mode_for_config(Config *config,
                                          exec_mode_t config_mode_type,
                                          const char *command_filename) {
  if (config->mode != EXEC_MODE_SINGLE_COMMAND) {
    return CONFIG_LOAD_STATUS_MULTIPLE_EXEC_MODES;
  }
  config->mode = config_mode_type;
  if (config->mode == EXEC_MODE_COMMAND_FILE) {
    config->command_file = get_formatted_string("%s", command_filename);
  }
  return CONFIG_LOAD_STATUS_SUCCESS;
}

int set_command_type_for_config(Config *config, ParsedArgs *parsed_args) {
  config->command_type = COMMAND_TYPE_UNKNOWN;
  int number_of_tokens_parsed = 0;
  for (int i = 0;
       VALID_COMMAND_SEQUENCES[i].command_type != COMMAND_TYPE_UNKNOWN; i++) {
    bool sequence_matches = true;
    number_of_tokens_parsed = 0;
    for (int j = 0; VALID_COMMAND_SEQUENCES[i].arg_token_sequence[j] !=
                    NUMBER_OF_ARG_TOKENS;
         j++) {
      if (!parsed_args->args[j]->has_value ||
          parsed_args->args[j]->token !=
              VALID_COMMAND_SEQUENCES[i].arg_token_sequence[j]) {
        sequence_matches = false;
        break;
      }
      number_of_tokens_parsed++;
    }
    if (sequence_matches) {
      config->command_type = VALID_COMMAND_SEQUENCES[i].command_type;
      break;
    }
  }
  return number_of_tokens_parsed;
}

config_load_status_t set_cgp_string_for_config(Config *config,
                                               SingleArg *cgp_arg) {
  if (config->cgp) {
    free(config->cgp);
  }
  // At this point it is guaranteed that cgp_arg has 4 values.
  config->cgp = get_formatted_string("%s %s %s %s", cgp_arg->values[0],
                                     cgp_arg->values[1], cgp_arg->values[2],
                                     cgp_arg->values[3]);
  config->command_set_cgp = true;
  return CONFIG_LOAD_STATUS_SUCCESS;
}

config_load_status_t load_player_name_for_config(Config *config,
                                                 int player_index,
                                                 const char *player_name) {
  players_data_set_name(config->players_data, player_index, player_name);
  return CONFIG_LOAD_STATUS_SUCCESS;
}

bool lexicons_are_compatible(const char *p1_lexicon_name,
                             const char *p2_lexicon_name) {
  char *p1_ld = get_default_letter_distribution_name(p1_lexicon_name);
  char *p2_ld = get_default_letter_distribution_name(p2_lexicon_name);
  bool compatible = strings_equal(p1_ld, p2_ld) ||
                    // English and French use the same letters so they are
                    // allowed to play against each other.
                    (strings_equal(p1_ld, ENGLISH_LETTER_DISTRIBUTION_NAME) &&
                     strings_equal(p2_ld, FRENCH_LETTER_DISTRIBUTION_NAME)) ||
                    (strings_equal(p2_ld, ENGLISH_LETTER_DISTRIBUTION_NAME) &&
                     strings_equal(p1_ld, FRENCH_LETTER_DISTRIBUTION_NAME));
  free(p1_ld);
  free(p2_ld);
  return compatible;
}

// Returns true on success and
// return false if data was considered missing
config_load_status_t load_players_data_for_config(
    Config *config, players_data_t players_data_type,
    const char *p1_new_data_name, const char *p1_default_data_name,
    const char *p2_new_data_name, const char *p2_default_data_name,
    bool allow_null_data) {

  bool p1_has_data = p1_new_data_name || p1_default_data_name;
  bool p2_has_data = p2_new_data_name || p2_default_data_name;

  // Do not allow just a single data to be specified. Enforce all
  // or nothing to simplify logic.
  if ((p1_has_data && !p2_has_data) || (!p1_has_data && p2_has_data)) {
    return CONFIG_LOAD_STATUS_LEXICON_MISSING;
  }

  // If no data is specified, return now
  if (!p1_has_data && !p2_has_data) {
    if (allow_null_data) {
      return CONFIG_LOAD_STATUS_SUCCESS;
    } else {
      return CONFIG_LOAD_STATUS_LEXICON_MISSING;
    }
  }

  const char *new_data_names[2];
  new_data_names[0] = p1_new_data_name;
  new_data_names[1] = p2_new_data_name;
  const char *default_data_names[2];
  default_data_names[0] = p1_default_data_name;
  default_data_names[1] = p2_default_data_name;
  char *final_data_names[2];
  final_data_names[0] = NULL;
  final_data_names[1] = NULL;

  for (int player_index = 0; player_index < 2; player_index++) {
    if (!is_string_empty_or_null(new_data_names[player_index])) {
      final_data_names[player_index] =
          get_formatted_string("%s", new_data_names[player_index]);
    } else {
      final_data_names[player_index] = default_data_names[player_index];
    }
  }

  config_load_status_t config_load_status = CONFIG_LOAD_STATUS_SUCCESS;

  // Since KWG and KLV share names, they can both be arguments
  // to the compatibility function.
  if (!lexicons_are_compatible(final_leaves_names[0], final_leaves_names[1])) {
    config_load_status = CONFIG_LOAD_STATUS_INCOMPATIBLE_LEXICONS;
  } else {
    set_players_data(config->players_data, players_data_type,
                     final_leaves_names[0], final_leaves_names[1]);
  }
  for (int player_index = 0; player_index < 2; player_index++) {
    free(final_leaves_names[player_index]);
  }
  return config_load_status;
}

config_load_status_t load_lexicons_for_config(Config *config,
                                              const char *p1_new_lexicon_name,
                                              const char *p2_new_lexicon_name,
                                              bool allow_null_lexicons) {
  // The defaults are the existing names
  const char *p1_default_lexicon_name = players_data_get_data_name(
      config->players_data, PLAYERS_DATA_TYPE_KWG, 0);
  const char *p2_default_lexicon_name = players_data_get_data_name(
      config->players_data, PLAYERS_DATA_TYPE_KWG, 1);
  config_load_status_t config_load_status = CONFIG_LOAD_STATUS_SUCCESS;
  return load_players_data_for_config(
      config, PLAYERS_DATA_TYPE_KWG, p1_new_lexicon_name,
      p1_default_lexicon_name, p2_new_lexicon_name, p2_default_lexicon_name,
      allow_null_lexicons);
}

char *get_default_klv_name(const char *lexicon_name) {
  return get_formatted_string("%s", lexicon_name);
}

config_load_status_t load_leaves_for_config(Config *config,
                                            const char *p1_new_leaves_name,
                                            const char *p2_new_leaves_name) {
  // These might be null
  // if we are doing a coldstart
  const char *p1_existing_lexicon_name = players_data_get_data_name(
      config->players_data, PLAYERS_DATA_TYPE_KWG, 0);
  const char *p2_existing_lexicon_name = players_data_get_data_name(
      config->players_data, PLAYERS_DATA_TYPE_KWG, 1);

  // Lexicons are guarantted to either both be null or both be nonnull
  if (!p1_existing_lexicon_name && !p2_existing_lexicon_name) {
    // If both lexicon names are null, we can safely assume
    // we are in a cold start.
    if (p1_new_leaves_name || p2_new_leaves_name) {
      return CONFIG_LOAD_STATUS_LEXICON_MISSING;
    } else {
      return CONFIG_LOAD_STATUS_SUCCESS;
    }
  }

  const char *p1_default_leaves_name =
      get_default_klv_name(p1_existing_lexicon_name);
  const char *p2_default_leaves_name =
      get_default_klv_name(p2_existing_lexicon_name);
  config_load_status_t config_load_status = CONFIG_LOAD_STATUS_SUCCESS;
  return load_players_data_for_config(
      config, PLAYERS_DATA_TYPE_KLV, p1_new_leaves_name, p1_default_leaves_name,
      p2_new_leaves_name, p2_default_leaves_name, false);
}

config_load_status_t
load_letter_distribution_for_config(Config *config,
                                    const char *new_letter_distribution_name) {

  // Use player 1 lexicon to get a default distribution
  const char *p1_lexicon_name = players_data_get_data_name(
      config->players_data, PLAYERS_DATA_TYPE_KWG, 0);

  // If p1_lexicon_name is null, we can safely assume
  // we are in a cold start that does not require
  // lexicons
  if (is_string_empty_or_null(p1_lexicon_name)) {
    if (is_string_empty_or_null(new_letter_distribution_name)) {
      return CONFIG_LOAD_STATUS_SUCCESS;
    } else {
      return CONFIG_LOAD_STATUS_LEXICON_MISSING;
    }
  }

  char *letter_distribution_name = NULL;

  if (!is_string_empty_or_null(new_letter_distribution_name)) {
    letter_distribution_name =
        get_formatted_string("%s", new_letter_distribution_name);
  } else {
    letter_distribution_name =
        get_default_letter_distribution_name(p1_lexicon_name);
  }

  if (strings_equal(config->ld_name, letter_distribution_name)) {
    free(letter_distribution_name);
    return CONFIG_LOAD_STATUS_SUCCESS;
  }
  LetterDistribution *new_letter_distribution =
      create_letter_distribution(letter_distribution_name);
  if (config->letter_distribution) {
    destroy_letter_distribution(config->letter_distribution);
  }
  config->letter_distribution = new_letter_distribution;

  if (config->ld_name) {
    free(config->ld_name);
  }
  config->ld_name = letter_distribution_name;
  return CONFIG_LOAD_STATUS_SUCCESS;
}

bool is_coldstart_arg(arg_token_t arg_token) {
  return arg_token == ARG_TOKEN_UCGI_MODE ||
         arg_token == ARG_TOKEN_CONSOLE_MODE ||
         arg_token == ARG_TOKEN_COMMAND_FILE;
}

config_load_status_t load_config_with_parsed_args(Config *config,
                                                  ParsedArgs *parsed_args,
                                                  bool coldstart) {

  int args_start_index = set_command_type_for_config(config, parsed_args);

  if (config->command_type == COMMAND_TYPE_UNKNOWN) {
    return CONFIG_LOAD_STATUS_UNRECOGNIZED_COMMAND;
  }

  // Set the names using the args
  // and load the data once the args
  // are parsed.
  const char *new_p1_lexicon_name = NULL;
  const char *new_p1_leaves_name = NULL;
  const char *new_p2_lexicon_name = NULL;
  const char *new_p2_leaves_name = NULL;
  const char *new_letter_distribution_name = NULL;
  const char *new_win_pct_name = NULL;
  const char *rack = NULL;
  config_load_status_t config_load_status = CONFIG_LOAD_STATUS_SUCCESS;
  for (int i = args_start_index; i < NUMBER_OF_ARG_TOKENS; i++) {
    if (!parsed_args->args[i]->has_value) {
      continue;
    }
    arg_token_t arg_token = single_arg->token;
    if (!coldstart && is_coldstart_arg(arg_token)) {
      config_load_status = CONFIG_LOAD_STATUS_FOUND_COLDSTART_ONLY_OPTION;
      break;
    }
    SingleArg *single_arg = parsed_args->args[i];
    char **arg_values = single_arg->values;
    switch (arg_token) {
    case ARG_TOKEN_POSITION:
    case ARG_TOKEN_GO:
    case ARG_TOKEN_SIM:
    case ARG_TOKEN_INFER:
    case ARG_TOKEN_AUTOPLAY:
    case ARG_TOKEN_SET_OPTIONS:
      config_load_status = CONFIG_LOAD_STATUS_MISPLACED_COMMAND;
      break;
    case ARG_TOKEN_CGP:
      config_load_status = set_cgp_string_for_config(config, single_arg);
      break;
    case ARG_TOKEN_BINGO_BONUS:
      config_load_status = load_bingo_bonus_for_config(config, arg_values[0]);
      break;
    case ARG_TOKEN_BOARD_LAYOUT:
      config_load_status = load_board_layout_for_config(config, arg_values[0]);
      break;
    case ARG_TOKEN_GAME_VARIANT:
      config_load_status = load_game_variant_for_config(config, arg_values[0]);
      break;
    case ARG_TOKEN_LETTER_DISTRIBUTION:
      new_letter_distribution_name = arg_values[0];
      break;
    case ARG_TOKEN_LEXICON:
      new_p1_lexicon_name = arg_values[0];
      new_p2_lexicon_name = arg_values[0];
      break;
    case ARG_TOKEN_P1_NAME:
      config_load_status =
          load_player_name_for_config(config, 0, arg_values[0]);
      break;
    case ARG_TOKEN_P1_LEXICON:
      new_p1_lexicon_name = arg_values[0];
      break;
    case ARG_TOKEN_P1_LEAVES:
      new_p1_leaves_name = arg_values[0];
      break;
    case ARG_TOKEN_P1_MOVE_SORT_TYPE:
      config_load_status =
          load_move_sort_type_for_config(config, arg_values[0], 0);
      break;
    case ARG_TOKEN_P1_MOVE_RECORD_TYPE:
      config_load_status =
          load_move_record_type_for_config(config, arg_values[0], 0);
      break;
    case ARG_TOKEN_P2_NAME:
      config_load_status =
          load_player_name_for_config(config, 1, arg_values[0]);
      break;
    case ARG_TOKEN_P2_LEXICON:
      new_p2_lexicon_name = arg_values[0];
      break;
    case ARG_TOKEN_P2_LEAVES:
      new_p2_leaves_name = arg_values[0];
      break;
    case ARG_TOKEN_P2_MOVE_SORT_TYPE:
      config_load_status =
          load_move_sort_type_for_config(config, arg_values[0], 1);
      break;
    case ARG_TOKEN_P2_MOVE_RECORD_TYPE:
      config_load_status =
          load_move_record_type_for_config(config, arg_values[0], 1);
      break;
    case ARG_TOKEN_KNOWN_OPP_RACK:
      rack = arg_values[0];
      break;
    case ARG_TOKEN_WIN_PCT:
      new_win_pct_name = arg_values[0];
      break;
    case ARG_TOKEN_NUMBER_OF_PLAYS:
      config_load_status = load_num_plays_for_config(config, arg_values[0]);
      break;
    case ARG_TOKEN_PLIES:
      config_load_status = load_plies_for_config(config, arg_values[0]);
      break;
    case ARG_TOKEN_MAX_ITERATIONS:
      config_load_status =
          load_max_iterations_for_config(config, arg_values[0]);
      break;
    case ARG_TOKEN_STOPPING_CONDITION:
      config_load_status =
          load_stopping_condition_for_config(config, arg_values[0]);
      break;
    case ARG_TOKEN_STATIC_SEARCH_ON:
      config_load_status = load_static_search_only_for_config(config, true);
      break;
    case ARG_TOKEN_STATIC_SEARCH_OFF:
      config_load_status = load_static_search_only_for_config(config, false);
      break;
    case ARG_TOKEN_PLAYER_TO_INFER_INDEX:
      config_load_status =
          load_player_to_infer_index_for_config(config, arg_values[0]);
      break;
    case ARG_TOKEN_SCORE:
      config_load_status = load_score_for_config(config, arg_values[0]);
      break;
    case ARG_TOKEN_EQUITY_MARGIN:
      config_load_status = load_equity_margin_for_config(config, arg_values[0]);
      break;
    case ARG_TOKEN_NUMBER_OF_TILES_EXCHANGED:
      config_load_status =
          load_number_of_tiles_exchanged_for_config(config, arg_values[0]);
      break;
    case ARG_TOKEN_GAME_PAIRS_ON:
      config_load_status = load_use_game_pairs_for_config(config, true);
      break;
    case ARG_TOKEN_GAME_PAIRS_OFF:
      config_load_status = load_use_game_pairs_for_config(config, false);
      break;
    case ARG_TOKEN_RANDOM_SEED:
      config_load_status = load_random_seed_for_config(config, arg_values[0]);
      break;
    case ARG_TOKEN_NUMBER_OF_THREADS:
      config_load_status =
          load_number_of_threads_for_config(config, arg_values[0]);
      break;
    case ARG_TOKEN_PRINT_INFO_INTERVAL:
      config_load_status =
          load_print_info_interval_for_config(config, arg_values[0]);
      break;
    case ARG_TOKEN_CHECK_STOP_INTERVAL:
      config_load_status =
          load_check_stop_interval_for_config(config, arg_values[0]);
      break;
    case ARG_TOKEN_INFILE:
      config_load_status = load_infile_for_config(config, arg_values[0]);
      break;
    case ARG_TOKEN_OUTFILE:
      config_load_status = load_outfile_for_config(config, arg_values[0]);
      break;
    case ARG_TOKEN_ERRORFILE:
      config_load_status = load_errorfile_for_config(config, arg_values[0]);
      break;
    case ARG_TOKEN_CONSOLE_MODE:
      config_load_status =
          load_mode_for_config(config, EXEC_MODE_CONSOLE, NULL);
      break;
    case ARG_TOKEN_UCGI_MODE:
      config_load_status = load_mode_for_config(config, EXEC_MODE_UCGI, NULL);
      break;
    case ARG_TOKEN_COMMAND_FILE:
      config_load_status =
          load_mode_for_config(config, EXEC_MODE_COMMAND_FILE, arg_values[0]);
      break;
    case NUMBER_OF_ARG_TOKENS:
      log_fatal("invalid token found in args\n");
      break;
    }
    if (config_load_status != CONFIG_LOAD_STATUS_SUCCESS) {
      return config_load_status;
    }
  }

  // Lexicons must be loaded before other data
  // since they are used to get default data
  // for leaves, letter distribution, etc.
  // if those values are not specified
  config_load_status = load_lexicons_for_config(config, new_p1_lexicon_name,
                                                new_p2_lexicon_name, coldstart);
  if (config_load_status != CONFIG_LOAD_STATUS_SUCCESS) {
    return config_load_status;
  }

  config_load_status =
      load_leaves_for_config(config, new_p1_leaves_name, new_p2_leaves_name);
  if (config_load_status != CONFIG_LOAD_STATUS_SUCCESS) {
    return config_load_status;
  }

  config_load_status =
      load_letter_distribution_for_config(config, new_letter_distribution_name);
  if (config_load_status != CONFIG_LOAD_STATUS_SUCCESS) {
    return config_load_status;
  }

  // The letter distribution may have changed,
  // so we might need to update the rack.
  // If this is a coldstart, we might not have a
  // letter distribution in which case we do not update
  // the rack.
  if (config->letter_distribution) {
    update_or_create_rack(&config->rack, config->letter_distribution->size);
  }

  // Now that the letter distribution has been loaded
  // we can read the rack.
  config_load_status = load_rack_for_config(config, rack);
  if (config_load_status != CONFIG_LOAD_STATUS_SUCCESS) {
    return config_load_status;
  }

  return load_winpct_for_config(config, new_win_pct_name);
}

config_load_status_t load_config(Config *config, const char *cmd,
                                 bool coldstart) {

  // If the command is empty, consider this a set options
  // command where zero options are set and return without error.
  if (is_all_whitespace_or_empty(cmd)) {
    config->command_type = COMMAND_TYPE_SET_OPTIONS;
    return CONFIG_LOAD_STATUS_SUCCESS;
  }

  ParsedArgs *parsed_args = create_parsed_args();
  StringSplitter *cmd_split_string = split_string_by_whitespace(cmd, true);
  // CGP values can have semicolons at the end, so
  // we trim these off to make loading easier.
  string_splitter_trim_char(cmd_split_string, ';');
  config_load_status_t config_load_status =
      init_parsed_args(parsed_args, cmd_split_string);

  config->command_set_cgp = false;
  if (config_load_status == CONFIG_LOAD_STATUS_SUCCESS) {
    config_load_status =
        load_config_with_parsed_args(config, parsed_args, coldstart);
  }

  destroy_parsed_args(parsed_args);
  destroy_string_splitter(cmd_split_string);

  return config_load_status;
}

Config *create_default_config() {
  Config *config = malloc_or_die(sizeof(Config));
  config->command_type = COMMAND_TYPE_UNKNOWN;
  config->command_set_cgp = false;
  config->letter_distribution = NULL;
  config->ld_name = NULL;
  config->cgp = NULL;
  config->bingo_bonus = DEFAULT_BINGO_BONUS;
  config->board_layout = DEFAULT_BOARD_LAYOUT;
  config->game_variant = DEFAULT_GAME_VARIANT;
  config->players_data = create_players_data();
  config->rack = NULL;
  config->player_to_infer_index = 0;
  config->actual_score = 0;
  config->number_of_tiles_exchanged = 0;
  config->equity_margin = 0;
  config->win_pcts = NULL;
  config->win_pct_name = NULL;
  config->num_plays = DEFAULT_MOVE_LIST_CAPACITY;
  config->plies = 2;
  config->max_iterations = 0;
  config->stopping_condition = DEFAULT_SIMMING_STOPPING_CONDITION;
  config->static_search_only = false;
  config->use_game_pairs = false;
  config->random_seed = 0;
  config->thread_control = create_thread_control();
  config->mode = EXEC_MODE_SINGLE_COMMAND;
  config->command_file = NULL;
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
  if (config->rack) {
    destroy_rack(config->rack);
  }
  if (config->win_pcts) {
    destroy_winpct(config->win_pcts);
  }
  if (config->win_pct_name) {
    free(config->win_pct_name);
  }
  destroy_players_data(config->players_data);
  destroy_thread_control(config->thread_control);
  free(config);
}