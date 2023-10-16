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
#define DEFAULT_MOVE_RECORD_TYPE MOVE_RECORD_BEST

#define ARG_POSITION "position"
#define ARG_CGP "cgp"
#define ARG_GO "go"
#define ARG_SIM "sim"
#define ARG_INFER "infer"
#define ARG_AUTOPLAY "autoplay"
#define ARG_SET "set"
#define ARG_OPTIONS "options"
#define ARG_BINGO_BONUS "bb"
#define ARG_BOARD_LAYOUT "bdn"
#define ARG_GAME_VARIANT "var"
#define ARG_LETTER_DISTRIBUTION "ld"
#define ARG_LEXICON "lex"
#define ARG_P1_LEXICON "l1"
#define ARG_P1_LEAVES "k1"
#define ARG_P1_MOVE_SORT_TYPE "s1"
#define ARG_P1_MOVE_RECORD_TYPE "r1"
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
#define ARG_STATIC_SEARCH_ONLY "static"
#define ARG_PLAYER_TO_INFER_INDEX "pindex"
#define ARG_SCORE "score"
#define ARG_EQUITY_MARGIN "eq"
#define ARG_NUMBER_OF_TILES_EXCHANGED "exch"
#define ARG_USE_GAME_PAIRS "gp"
#define ARG_RANDOM_SEED "rs"
#define ARG_NUMBER_OF_THREADS "numthreads"
#define ARG_PRINT_INFO_INTERVAL "print"
#define ARG_CHECK_STOP_INTERVAL "check"

typedef enum {
  // Commands
  ARG_TOKEN_POSITION,
  ARG_TOKEN_CGP,
  ARG_TOKEN_GO,
  ARG_TOKEN_SIM,
  ARG_TOKEN_INFER,
  ARG_TOKEN_AUTOPLAY,
  ARG_TOKEN_SET,
  ARG_TOKEN_OPTIONS,
  // Game
  // shared between players
  ARG_TOKEN_BINGO_BONUS,
  ARG_TOKEN_BOARD_LAYOUT,
  ARG_TOKEN_GAME_VARIANT,
  ARG_TOKEN_LETTER_DISTRIBUTION,
  ARG_TOKEN_LEXICON,
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
  ARG_TOKEN_WIN_PCT,
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
  ARG_TOKEN_RANDOM_SEED,
  // number of iterations shared with sim
  // Thread Control
  ARG_TOKEN_NUMBER_OF_THREADS,
  ARG_TOKEN_PRINT_INFO_INTERVAL,
  ARG_TOKEN_CHECK_STOP_INTERVAL,
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
} VALID_COMMAND_SEQUENCES[5] = {{
    // The valid sequences
    {COMMAND_TYPE_LOAD_CGP,
     // The NUMBER_OF_ARG_TOKENS token denotes the end of the arg token
     // sequence
     {ARG_TOKEN_POSITION, ARG_TOKEN_CGP, NUMBER_OF_ARG_TOKENS}},
    {COMMAND_TYPE_SIM, {ARG_TOKEN_GO, ARG_TOKEN_SIM, NUMBER_OF_ARG_TOKENS}},
    {COMMAND_TYPE_INFER, {ARG_TOKEN_GO, ARG_TOKEN_INFER, NUMBER_OF_ARG_TOKENS}},
    {COMMAND_TYPE_AUTOPLAY,
     {ARG_TOKEN_GO, ARG_TOKEN_AUTOPLAY, NUMBER_OF_ARG_TOKENS}},
    {COMMAND_TYPE_SET_OPTIONS,
     {ARG_TOKEN_SET, ARG_TOKEN_OPTIONS, NUMBER_OF_ARG_TOKENS}},
    // The unknown command type denotes the end of the sequences
    {COMMAND_TYPE_UNKNOWN, {}},
}};

typedef struct SingleArg {
  arg_token_t token;
  char *name;
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
  single_arg->position = NUMBER_OF_ARG_TOKENS + 1;
  parsed_args->args[index] = single_arg;
}

ParsedArgs *create_parsed_args(StringSplitter *cmd) {
  ParsedArgs *parsed_args = malloc_or_die(sizeof(ParsedArgs));

  int index = 0;
  // Command args
  set_single_arg(parsed_args, index++, ARG_TOKEN_POSITION, ARG_POSITION, 0);
  set_single_arg(parsed_args, index++, ARG_TOKEN_CGP, ARG_CGP, 4);
  set_single_arg(parsed_args, index++, ARG_TOKEN_GO, ARG_GO, 0);
  set_single_arg(parsed_args, index++, ARG_TOKEN_SIM, ARG_SIM, 0);
  set_single_arg(parsed_args, index++, ARG_TOKEN_INFER, ARG_INFER, 0);
  set_single_arg(parsed_args, index++, ARG_TOKEN_AUTOPLAY, ARG_AUTOPLAY, 0);
  set_single_arg(parsed_args, index++, ARG_TOKEN_SET, ARG_SET, 0);
  set_single_arg(parsed_args, index++, ARG_TOKEN_OPTIONS, ARG_OPTIONS, 0);

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
  set_single_arg(parsed_args, index++, ARG_TOKEN_P1_LEXICON, ARG_P1_LEXICON, 1);
  set_single_arg(parsed_args, index++, ARG_TOKEN_P1_LEAVES, ARG_P1_LEAVES, 1);
  set_single_arg(parsed_args, index++, ARG_TOKEN_P1_MOVE_SORT_TYPE,
                 ARG_P1_MOVE_SORT_TYPE, 1);
  set_single_arg(parsed_args, index++, ARG_TOKEN_P1_MOVE_RECORD_TYPE,
                 ARG_P1_MOVE_RECORD_TYPE, 1);

  // Player 2
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
  set_single_arg(parsed_args, index++, ARG_TOKEN_NUMBER_OF_PLAYS,
                 ARG_NUMBER_OF_PLAYS, 1);
  set_single_arg(parsed_args, index++, ARG_TOKEN_PLIES, ARG_PLIES, 1);
  set_single_arg(parsed_args, index++, ARG_TOKEN_MAX_ITERATIONS,
                 ARG_MAX_ITERATIONS, 1);
  set_single_arg(parsed_args, index++, ARG_TOKEN_STOPPING_CONDITION,
                 ARG_STOPPING_CONDITION, 1);
  set_single_arg(parsed_args, index++, ARG_TOKEN_STATIC_SEARCH_ONLY,
                 ARG_STATIC_SEARCH_ONLY, 0);

  // Inference args
  // rack is KNOWN_OPP_RACK shared with sim
  set_single_arg(parsed_args, index++, ARG_TOKEN_PLAYER_TO_INFER_INDEX,
                 ARG_PLAYER_TO_INFER_INDEX, 1);
  set_single_arg(parsed_args, index++, ARG_TOKEN_SCORE, ARG_SCORE, 1);
  set_single_arg(parsed_args, index++, ARG_TOKEN_EQUITY_MARGIN,
                 ARG_EQUITY_MARGIN, 1);

  // Autoplay
  set_single_arg(parsed_args, index++, ARG_TOKEN_USE_GAME_PAIRS,
                 ARG_USE_GAME_PAIRS, 1);
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
  for (int i = 0; i < number_of_input_args;) {
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
          i += single_arg->number_of_values + 1;
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

void get_index_of_kwg_to_load(Config *config, const char *lexicon_name,
                              int player_index) {
  int kwg_to_use = -1;
  if (!lexicon_name) {
    // Use existing
    kwg_to_use = player_index;
  } else {
    // Use existing if names match
    for (int i = 0; i < 2; i++) {
      if (strings_equal(config->player_strategy_params[i]->kwg_name,
                        lexicon_name)) {
        kwg_to_use = i;
      }
    }
  }
}

// FIXME: make this generic
config_load_status_t load_lexicons_for_config(Config *config,
                                              const char *p1_lexicon_name,
                                              const char *p2_lexicon_name) {
  int p1_kwg_to_use_index =
      get_index_of_kwg_to_load(config, p1_lexicon_name, 0);
  int p2_kwg_to_use_index =
      get_index_of_kwg_to_load(config, p2_lexicon_name, 1);

  KWG *p1_kwg;
  const char *p1_kwg_name;

  if (p1_kwg_to_use_index < 0) {
    p1_kwg = create_kwg(p1_lexicon_name);
    p1_kwg_name = p1_lexicon_name;
  } else {
    p1_kwg = config->player_strategy_params[p1_kwg_to_use_index]->kwg;
    p1_kwg_name = config->player_strategy_params[p1_kwg_to_use_index]->kwg_name;
  }

  KWG *p2_kwg;
  char *p2_kwg_name;

  if (p2_kwg_to_use_index < 0) {
    if (strings_equal(p1_lexicon_name, p2_lexicon_name)) {
      p2_kwg = p1_kwg;
      p2_kwg_name = p1_lexicon_name;
    } else {
      p2_kwg = create_kwg(p2_lexicon_name);
      p2_kwg_name = p2_lexicon_name;
    }
  } else {
    p2_kwg = config->player_strategy_params[p2_kwg_to_use_index]->kwg;
    p2_kwg_name = config->player_strategy_params[p2_kwg_to_use_index]->kwg_name;
  }

  for (int i = 0; i < 2; i++) {
    KWG *existing_kwg = config->player_strategy_params[i]->kwg;
    if (existing_kwg != p1_kwg && existing_kwg != p2_kwg) {
      destroy_kwg(existing_kwg);
    }
  }

  if (!p1_kwg || !p2_kwg) {
    return CONFIG_LOAD_STATUS_MISSING_LEXICON;
  }

  config->player_strategy_params[0]->kwg = p1_kwg;
  config->player_strategy_params[1]->kwg = p2_kwg;

  if (!strings_equal(config->player_strategy_params[0]->kwg_name,
                     p1_lexicon_name)) {
    free(config->player_strategy_params[0]->kwg_name);
    config->player_strategy_params[0]->kwg_name =
        get_formatted_string("%s", p1_lexicon_name);
  }
  if (!strings_equal(config->player_strategy_params[1]->kwg_name,
                     p2_lexicon_name)) {
    free(config->player_strategy_params[1]->kwg_name);
    config->player_strategy_params[1]->kwg_name =
        get_formatted_string("%s", p2_lexicon_name);
  }
  config->kwg_is_shared =
      strings_equal(config->player_strategy_params[0]->kwg_name,
                    config->player_strategy_params[1]->kwg_name);
}

config_load_status_t load_move_sort_type_for_config(
    Config *config, const char *move_sort_type_string, int player_index) {
  config_load_status_t config_load_status = CONFIG_LOAD_STATUS_SUCCESS;
  if (strings_equal(move_sort_type_string, MOVE_SORT_EQUITY_NAME)) {
    config->player_strategy_params[player_index]->move_sort_type =
        MOVE_SORT_EQUITY;
  } else if (strings_equal(move_sort_type_string, MOVE_SORT_SCORE_NAME)) {
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
  if (strings_equal(move_record_type_string, MOVE_RECORD_BEST_NAME)) {
    config->player_strategy_params[player_index]->move_record_type =
        MOVE_RECORD_BEST;
  } else if (strings_equal(move_record_type_string, MOVE_RECORD_ALL_NAME)) {
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
  int number_of_letters_set =
      set_rack_to_string(config->rack, rack, config->letter_distribution);
  if (number_of_letters_set < 0) {
    return CONFIG_LOAD_STATUS_MALFORMED_RACK;
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
  if (strings_equal(stopping_condition, "95")) {
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
  config->score = string_to_int(score);
  return CONFIG_LOAD_STATUS_SUCCESS;
}

config_load_status_t load_equity_margin_for_config(Config *config,
                                                   const char *equity_margin) {
  if (!is_decimal_number(equity_margin)) {
    return CONFIG_LOAD_STATUS_MALFORMED_PLAYER_TO_INFER_INDEX;
  }
  config->equity_margin = string_to_double(equity_margin);
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
  config->number_of_threads = string_to_int(number_of_tiles_threads);
  return CONFIG_LOAD_STATUS_SUCCESS;
}

config_load_status_t
load_print_info_interval_for_config(Config *config,
                                    const char *print_info_interval) {
  if (!is_all_digits_or_empty(print_info_interval)) {
    return CONFIG_LOAD_STATUS_MALFORMED_PRINT_INFO_INTERVAL;
  }
  config->print_info_interval = string_to_int(print_info_interval);
  return CONFIG_LOAD_STATUS_SUCCESS;
}

config_load_status_t
load_check_stop_interval_for_config(Config *config,
                                    const char *check_stop_interval) {
  if (!is_all_digits_or_empty(check_stop_interval)) {
    return CONFIG_LOAD_STATUS_MALFORMED_CHECK_STOP_INTERVAL;
  }
  config->check_stopping_condition_interval =
      string_to_int(check_stop_interval);
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
      arg_token_t arg_token = VALID_COMMAND_SEQUENCES[i].arg_token_sequence[j];
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

// The CGP is a special arg since it acts
// as both a subcommand and an arg token
config_load_status_t load_cgp_for_config(Config *config,
                                         ParsedArgs *parsed_args,
                                         int args_start_index) {
  SingleArg *cgp_arg = NULL;
  for (int i = 0; i < args_start_index; i++) {
    if (parsed_args->args[i]->token == ARG_TOKEN_CGP) {
      cgp_arg = parsed_args->args[i];
      break;
    }
  }
  if (!cgp_arg || !cgp_arg->has_value) {
    return CONFIG_LOAD_STATUS_INVALID_CGP_ARG;
  }
  config->cgp = get_formatted_string("%s %s %s %s", cgp_arg->values[0],
                                     cgp_arg->values[1], cgp_arg->values[2],
                                     cgp_arg->values[3]);
  return CONFIG_LOAD_STATUS_SUCCESS;
}

char *get_default_letter_distribution_name(const char *lexicon_name) {
  return "";
}

char *get_default_klv_name(const char *lexicon_name) {}

config_load_status_t load_config_with_parsed_args(Config *config,
                                                  ParsedArgs *parsed_args) {

  int args_start_index = set_command_type_for_config(config, parsed_args);

  if (config->command_type == COMMAND_TYPE_UNKNOWN) {
    return CONFIG_LOAD_STATUS_UNRECOGNIZED_COMMAND;
  }

  // Set the names using the args
  // and load the data once the args
  // are parsed.
  const char *p1_lexicon_name = NULL;
  const char *p1_leaves_name = NULL;
  const char *p2_lexicon_name = NULL;
  const char *p2_leaves_name = NULL;
  const char *win_pct_name = NULL;
  config_load_status_t config_load_status = CONFIG_LOAD_STATUS_SUCCESS;
  for (int i = args_start_index; i < NUMBER_OF_ARG_TOKENS; i++) {
    if (!parsed_args->has_value[i]) {
      continue;
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
      config_load_status =
          load_letter_distribution_for_config(config, arg_values[0]);
      break;
    case ARG_TOKEN_LEXICON:
      p1_lexicon_name = arg_values[0];
      p2_lexicon_name = arg_values[0];
      break;
    case ARG_TOKEN_P1_LEXICON:
      p1_lexicon_name = arg_values[0];
      break;
    case ARG_TOKEN_P1_LEAVES:
      p1_leaves_name = arg_values[0];
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
      p2_lexicon_name = arg_values[0];
      break;
    case ARG_TOKEN_P2_LEAVES:
      p2_leaves_name = arg_values[0];
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
      config_load_status = load_rack_for_config(config, arg_values[0]);
      break;
    case ARG_TOKEN_WIN_PCT:
      win_pct_name = arg_values[0];
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
    case ARG_TOKEN_STATIC_SEARCH_ONLY:
      config_load_status = load_static_search_only_for_config(config, true);
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
    case ARG_TOKEN_USE_GAME_PAIRS:
      config_load_status = load_use_game_pairs_for_config(config, true);
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
          load_check_stop_interval_threads_for_config(config, arg_values[0]);
      break;
    case NUMBER_OF_ARG_TOKENS:
      log_fatal("invalid token found in args\n");
      break;
    }
    if (config_load_status != CONFIG_LOAD_STATUS_SUCCESS) {
      return config_load_status;
    }
  }

  if (config->command_type == COMMAND_TYPE_LOAD_CGP) {
    config_load_status = load_cgp_for_config(config, parsed_args);
    if (config_load_status != CONFIG_LOAD_STATUS_SUCCESS) {
      return config_load_status;
    }
  }

  config_load_status =
      load_lexicons_for_config(config, p1_lexicon_name, p2_lexicon_name);
  if (config_load_status != CONFIG_LOAD_STATUS_SUCCESS) {
    return config_load_status;
  }

  load_leaves_for_config(config, p1_leaves_name, p2_leaves_name);
  if (config_load_status != CONFIG_LOAD_STATUS_SUCCESS) {
    return config_load_status;
  }

  return load_winpct_for_config(config, win_pct_name);
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

Config *create_default_config() {
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
  config->num_plays = DEFAULT_MOVE_LIST_CAPACITY;
  config->plies = 2;
  config->max_iterations = 0;
  config->stopping_condition = SIM_STOPPING_CONDITION_NONE;
  config->static_search_only = false;
  config->use_game_pairs = true;
  config->random_seed = 0;
  config->number_of_threads = 1;
  config->print_info_interval = 0;
  config->check_stopping_condition_interval = 0;
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
  free(config);
}