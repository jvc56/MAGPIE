#include "config.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

#include "../def/board_defs.h"
#include "../def/config_defs.h"
#include "../def/game_defs.h"
#include "../def/letter_distribution_defs.h"
#include "../def/move_defs.h"
#include "../def/players_data_defs.h"
#include "../def/simmer_defs.h"
#include "../def/thread_control_defs.h"
#include "../def/win_pct_defs.h"
#include "../util/log.h"
#include "../util/string_util.h"
#include "../util/util.h"
#include "board.h"
#include "letter_distribution.h"
#include "players_data.h"
#include "rack.h"
#include "thread_control.h"
#include "win_pct.h"

#define DEFAULT_BOARD_LAYOUT BOARD_LAYOUT_CROSSWORD_GAME
#define DEFAULT_GAME_VARIANT GAME_VARIANT_CLASSIC
#define DEFAULT_MOVE_LIST_CAPACITY 1
#define DEFAULT_SIMMING_STOPPING_CONDITION SIM_STOPPING_CONDITION_NONE

#define ARG_POSITION "position"
#define ARG_CGP "cgp"
#define ARG_MOVES "m"
#define ARG_GO "go"
#define ARG_GEN "gen"
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
#define ARG_P1_WIN_PCT "wp1"
#define ARG_P1_PLIES "plies1"
#define ARG_P1_NUMBER_OF_PLAYS "np1"
#define ARG_P1_MAX_ITERATIONS "mi1"
#define ARG_P1_STOPPING_CONDITION "cond1"
#define ARG_P2_WIN_PCT "wp2"
#define ARG_P2_PLIES "plies2"
#define ARG_P2_NUMBER_OF_PLAYS "np2"
#define ARG_P2_MAX_ITERATIONS "mi2"
#define ARG_P2_STOPPING_CONDITION "cond2"
#define ARG_P2_MAX_ITERATIONS "mi2"
#define ARG_P2_NAME "p2"
#define ARG_P2_LEXICON "l2"
#define ARG_P2_LEAVES "k2"
#define ARG_P2_MOVE_SORT_TYPE "s2"
#define ARG_P2_MOVE_RECORD_TYPE "r2"
#define ARG_KNOWN_OPP_RACK "rack"
#define ARG_WIN_PCT "win_pct"
#define ARG_PLIES "plies"
#define ARG_NUMBER_OF_PLAYS "numplays"
#define ARG_MAX_ITERATIONS "i"
#define ARG_STOPPING_CONDITION "cond"
#define ARG_TARGET_INDEX "pindex"
#define ARG_SCORE "score"
#define ARG_EQUITY_MARGIN "eq"
#define ARG_TARGET_NUMBER_OF_TILES_EXCHANGED "exch"
#define ARG_NUMBER_OF_AUTOPLAY_ROUNDS "rounds"
#define ARG_GAME_PAIRS_ON "gp"
#define ARG_GAME_PAIRS_OFF "nogp"
#define ARG_RANDOM_SEED "rs"
#define ARG_NUMBER_OF_THREADS "threads"
#define ARG_PRINT_INFO_INTERVAL "info"
#define ARG_CHECK_STOP_INTERVAL "check"
#define ARG_INFILE "infile"
#define ARG_OUTFILE "outfile"
#define ARG_CONSOLE_MODE "console"
#define ARG_UCGI_MODE "ucgi"

#define ARG_VAL_MOVE_SORT_EQUITY "equity"
#define ARG_VAL_MOVE_SORT_SCORE "score"

#define ARG_VAL_MOVE_RECORD_BEST "best"
#define ARG_VAL_MOVE_RECORD_ALL "all"

struct Config {
  // Transient fields
  // these fields are reset
  // every time the config is loaded
  bool command_set_infile;
  bool command_set_exec_mode;

  // Persistent fields
  // these fields maintain their
  // value between config loads

  command_t command_type;
  // Game
  LetterDistribution *ld;
  char *ld_name;
  bool ld_name_changed;
  char *cgp;
  char *moves;
  int bingo_bonus;
  board_layout_t board_layout;
  game_variant_t game_variant;
  PlayersData *players_data;
  // Inference
  Rack *rack;
  int target_index;
  int target_score;
  int target_number_of_tiles_exchanged;
  double equity_margin;
  // Sim
  WinPct *win_pcts;
  char *win_pct_name;
  int num_plays;
  int plies;
  int max_iterations;
  sim_stopping_condition_t stopping_condition;
  // Autoplay
  int num_autoplay_rounds;
  bool use_game_pairs;
  uint64_t seed;
  // Thread Control
  ThreadControl *thread_control;
  // Config mode and command file execution
  exec_mode_t exec_mode;
};

typedef enum {
  // Commands
  ARG_TOKEN_POSITION,
  ARG_TOKEN_CGP,
  ARG_TOKEN_MOVES,
  ARG_TOKEN_GO,
  ARG_TOKEN_GEN,
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
  // Sim (separate configs per player)
  ARG_TOKEN_P1_WIN_PCT,
  ARG_TOKEN_P1_PLIES,
  ARG_TOKEN_P1_NUMBER_OF_PLAYS,
  ARG_TOKEN_P1_MAX_ITERATIONS,
  ARG_TOKEN_P1_STOPPING_CONDITION,
  ARG_TOKEN_P2_WIN_PCT,
  ARG_TOKEN_P2_PLIES,
  ARG_TOKEN_P2_NUMBER_OF_PLAYS,
  ARG_TOKEN_P2_MAX_ITERATIONS,
  ARG_TOKEN_P2_STOPPING_CONDITION,
  // Sim
  ARG_TOKEN_KNOWN_OPP_RACK,
  ARG_TOKEN_WIN_PCT,
  ARG_TOKEN_PLIES,
  ARG_TOKEN_NUMBER_OF_PLAYS,
  ARG_TOKEN_MAX_ITERATIONS,
  ARG_TOKEN_STOPPING_CONDITION,
  // Infer
  // Rack is shared with sim
  ARG_TOKEN_target_index,
  ARG_TOKEN_SCORE,
  ARG_TOKEN_EQUITY_MARGIN,
  ARG_TOKEN_target_number_of_tiles_exchanged,
  // Autoplay
  ARG_TOKEN_NUMBER_OF_AUTOPLAY_ROUNDS,
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

  ARG_TOKEN_CONSOLE_MODE,
  ARG_TOKEN_UCGI_MODE,
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
} VALID_COMMAND_SEQUENCES[] = {
    // The valid sequences
    // The NUMBER_OF_ARG_TOKENS denotes the end of the given sequence
    {COMMAND_TYPE_LOAD_CGP, {ARG_TOKEN_POSITION, NUMBER_OF_ARG_TOKENS}},
    {COMMAND_TYPE_GEN, {ARG_TOKEN_GO, ARG_TOKEN_GEN, NUMBER_OF_ARG_TOKENS}},
    {COMMAND_TYPE_SIM, {ARG_TOKEN_GO, ARG_TOKEN_SIM, NUMBER_OF_ARG_TOKENS}},
    {COMMAND_TYPE_INFER, {ARG_TOKEN_GO, ARG_TOKEN_INFER, NUMBER_OF_ARG_TOKENS}},
    {COMMAND_TYPE_AUTOPLAY,
     {ARG_TOKEN_GO, ARG_TOKEN_AUTOPLAY, NUMBER_OF_ARG_TOKENS}},
    {COMMAND_TYPE_SET_OPTIONS, {ARG_TOKEN_SET_OPTIONS, NUMBER_OF_ARG_TOKENS}},
    // A sequence with just NUMBER_OF_ARG_TOKENS denotes the end of the
    // sequences
    {0, {NUMBER_OF_ARG_TOKENS}},
};

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

bool config_get_command_set_infile(const Config *config) {
  return config->command_set_infile;
}

bool config_get_command_set_exec_mode(const Config *config) {
  return config->command_set_exec_mode;
}

command_t config_get_command_type(const Config *config) {
  return config->command_type;
}

LetterDistribution *config_get_ld(const Config *config) { return config->ld; }

char *config_get_ld_name(const Config *config) { return config->ld_name; }

bool config_get_ld_name_changed(const Config *config) {
  return config->ld_name_changed;
}

const char *config_get_cgp(const Config *config) { return config->cgp; }

const char *config_get_moves(const Config *config) { return config->moves; }

int config_get_bingo_bonus(const Config *config) { return config->bingo_bonus; }

game_variant_t config_get_game_variant(const Config *config) {
  return config->game_variant;
}

PlayersData *config_get_players_data(const Config *config) {
  return config->players_data;
}

Rack *config_get_rack(const Config *config) { return config->rack; }

int config_get_target_index(const Config *config) {
  return config->target_index;
}

int config_get_target_score(const Config *config) {
  return config->target_score;
}

int config_get_target_number_of_tiles_exchanged(const Config *config) {
  return config->target_number_of_tiles_exchanged;
}

double config_get_equity_margin(const Config *config) {
  return config->equity_margin;
}

int config_get_num_autoplay_rounds(const Config *config) {
  return config->num_autoplay_rounds;
}

WinPct *config_get_win_pcts(const Config *config) { return config->win_pcts; }

int config_get_num_plays(const Config *config) { return config->num_plays; }

int config_get_plies(const Config *config) { return config->plies; }

int config_get_max_iterations(const Config *config) {
  return config->max_iterations;
}

sim_stopping_condition_t config_get_stopping_condition(const Config *config) {
  return config->stopping_condition;
}

bool config_get_use_game_pairs(const Config *config) {
  return config->use_game_pairs;
}

uint64_t config_get_seed(const Config *config) { return config->seed; }

ThreadControl *config_get_thread_control(const Config *config) {
  return config->thread_control;
}

exec_mode_t config_get_exec_mode(const Config *config) {
  return config->exec_mode;
}

SingleArg *create_single_arg() {
  SingleArg *single_arg = malloc_or_die(sizeof(SingleArg));
  return single_arg;
}

void destroy_single_arg(SingleArg *single_arg) {
  if (!single_arg) {
    return;
  }
  free(single_arg->name);
  if (single_arg->has_value) {
    for (int i = 0; i < single_arg->number_of_values; i++) {
      free(single_arg->values[i]);
    }
  }
  free(single_arg->values);
  free(single_arg);
}

void set_single_arg(ParsedArgs *parsed_args, int index, arg_token_t arg_token,
                    const char *arg_name, int number_of_values) {
  SingleArg *single_arg = create_single_arg();
  single_arg->token = arg_token;
  single_arg->name = string_duplicate(arg_name);
  single_arg->number_of_values = number_of_values;
  single_arg->values = NULL;
  if (number_of_values > 0) {
    single_arg->values = malloc_or_die(sizeof(char *) * number_of_values);
  }
  single_arg->has_value = false;
  single_arg->position = NUMBER_OF_ARG_TOKENS + 1;
  parsed_args->args[index] = single_arg;
}

ParsedArgs *create_parsed_args() {
  ParsedArgs *parsed_args = malloc_or_die(sizeof(ParsedArgs));

  int index = 0;
  // Command args
  set_single_arg(parsed_args, index++, ARG_TOKEN_POSITION, ARG_POSITION, 0);
  set_single_arg(parsed_args, index++, ARG_TOKEN_CGP, ARG_CGP, 4);
  set_single_arg(parsed_args, index++, ARG_TOKEN_MOVES, ARG_MOVES, 1);
  set_single_arg(parsed_args, index++, ARG_TOKEN_GO, ARG_GO, 0);
  set_single_arg(parsed_args, index++, ARG_TOKEN_SIM, ARG_SIM, 0);
  set_single_arg(parsed_args, index++, ARG_TOKEN_GEN, ARG_GEN, 0);
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
  set_single_arg(parsed_args, index++, ARG_TOKEN_P1_WIN_PCT, ARG_P1_WIN_PCT, 1);
  set_single_arg(parsed_args, index++, ARG_TOKEN_P1_PLIES, ARG_P1_PLIES, 1);
  set_single_arg(parsed_args, index++, ARG_TOKEN_P1_NUMBER_OF_PLAYS,
                 ARG_P1_NUMBER_OF_PLAYS, 1);
  set_single_arg(parsed_args, index++, ARG_TOKEN_P1_MAX_ITERATIONS,
                 ARG_P1_MAX_ITERATIONS, 1);
  set_single_arg(parsed_args, index++, ARG_TOKEN_STOPPING_CONDITION,
                 ARG_P1_STOPPING_CONDITION, 1);

  // Player 2
  set_single_arg(parsed_args, index++, ARG_TOKEN_P2_NAME, ARG_P2_NAME, 1);
  set_single_arg(parsed_args, index++, ARG_TOKEN_P2_LEXICON, ARG_P2_LEXICON, 1);
  set_single_arg(parsed_args, index++, ARG_TOKEN_P2_LEAVES, ARG_P2_LEAVES, 1);
  set_single_arg(parsed_args, index++, ARG_TOKEN_P2_MOVE_SORT_TYPE,
                 ARG_P2_MOVE_SORT_TYPE, 1);
  set_single_arg(parsed_args, index++, ARG_TOKEN_P2_MOVE_RECORD_TYPE,
                 ARG_P2_MOVE_RECORD_TYPE, 1);
  set_single_arg(parsed_args, index++, ARG_TOKEN_P2_WIN_PCT, ARG_P2_WIN_PCT, 1);
  set_single_arg(parsed_args, index++, ARG_TOKEN_P2_PLIES, ARG_P2_PLIES, 1);
  set_single_arg(parsed_args, index++, ARG_TOKEN_P2_NUMBER_OF_PLAYS,
                 ARG_P2_NUMBER_OF_PLAYS, 1);
  set_single_arg(parsed_args, index++, ARG_TOKEN_P2_MAX_ITERATIONS,
                 ARG_P2_MAX_ITERATIONS, 1);
  set_single_arg(parsed_args, index++, ARG_TOKEN_STOPPING_CONDITION,
                 ARG_P2_STOPPING_CONDITION, 1);

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

  // Inference args
  // rack is KNOWN_OPP_RACK shared with sim
  set_single_arg(parsed_args, index++, ARG_TOKEN_target_index, ARG_TARGET_INDEX,
                 1);
  set_single_arg(parsed_args, index++, ARG_TOKEN_SCORE, ARG_SCORE, 1);
  set_single_arg(parsed_args, index++, ARG_TOKEN_EQUITY_MARGIN,
                 ARG_EQUITY_MARGIN, 1);
  set_single_arg(parsed_args, index++,
                 ARG_TOKEN_target_number_of_tiles_exchanged,
                 ARG_TARGET_NUMBER_OF_TILES_EXCHANGED, 1);
  // Autoplay
  set_single_arg(parsed_args, index++, ARG_TOKEN_NUMBER_OF_AUTOPLAY_ROUNDS,
                 ARG_NUMBER_OF_AUTOPLAY_ROUNDS, 1);
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

  set_single_arg(parsed_args, index++, ARG_TOKEN_CONSOLE_MODE, ARG_CONSOLE_MODE,
                 0);
  set_single_arg(parsed_args, index++, ARG_TOKEN_UCGI_MODE, ARG_UCGI_MODE, 0);

  assert(index == NUMBER_OF_ARG_TOKENS);

  return parsed_args;
}

void destroy_parsed_args(ParsedArgs *parsed_args) {
  if (!parsed_args) {
    return;
  }
  for (int i = 0; i < NUMBER_OF_ARG_TOKENS; i++) {
    destroy_single_arg(parsed_args->args[i]);
  }
  free(parsed_args);
}

void sort_parsed_args(ParsedArgs *parsed_args) {
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
}

config_load_status_t init_parsed_args(const StringSplitter *cmd,
                                      ParsedArgs *parsed_args) {
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
          // Since the input arg name has been recognized, parse the
          // next single_arg->number_of_values to load the values
          // for this arg.
          for (int k = 0; k < single_arg->number_of_values; k++) {
            single_arg->values[k] = get_formatted_string(
                "%s", string_splitter_get_item(cmd, i + k + 1));
          }
          single_arg->has_value = true;
          single_arg->position = i;
          // Advance the current arg past all of the
          // values that were just loaded.
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

  sort_parsed_args(parsed_args);

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

game_variant_t get_game_variant_type_from_name(const char *variant_name) {
  game_variant_t game_variant = GAME_VARIANT_UNKNOWN;
  if (strings_equal(variant_name, GAME_VARIANT_CLASSIC_NAME)) {
    game_variant = GAME_VARIANT_CLASSIC;
  } else if (strings_equal(variant_name, GAME_VARIANT_WORDSMOG_NAME)) {
    game_variant = GAME_VARIANT_WORDSMOG;
  }
  return game_variant;
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

config_load_status_t load_win_pct_for_config(Config *config,
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

  WinPct *new_win_pcts = win_pct_create(win_pct_name);
  win_pct_destroy(config->win_pcts);
  config->win_pcts = new_win_pcts;

  free(config->win_pct_name);
  config->win_pct_name = string_duplicate(win_pct_name);
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

config_load_status_t load_max_iterations_for_config(
    Config *config, const char *max_iterations) {
  if (!is_all_digits_or_empty(max_iterations)) {
    return CONFIG_LOAD_STATUS_MALFORMED_MAX_ITERATIONS;
  }
  int mi = string_to_int(max_iterations);
  if (mi < 1) {
    return CONFIG_LOAD_STATUS_MALFORMED_MAX_ITERATIONS;
  }
  config->max_iterations = mi;
  return CONFIG_LOAD_STATUS_SUCCESS;
}

config_load_status_t load_stopping_condition_for_config(
    Config *config, const char *stopping_condition) {
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

config_load_status_t load_target_index_for_config(Config *config,
                                                  const char *target_index) {
  if (!is_all_digits_or_empty(target_index)) {
    return CONFIG_LOAD_STATUS_MALFORMED_PLAYER_INDEX;
  }
  config->target_index = string_to_int(target_index);
  if (config->target_index != 0 && config->target_index != 1) {
    return CONFIG_LOAD_STATUS_MALFORMED_PLAYER_INDEX;
  }
  return CONFIG_LOAD_STATUS_SUCCESS;
}

config_load_status_t load_score_for_config(Config *config, const char *score) {
  if (!is_all_digits_or_empty(score)) {
    return CONFIG_LOAD_STATUS_MALFORMED_SCORE;
  }
  config->target_score = string_to_int(score);
  return CONFIG_LOAD_STATUS_SUCCESS;
}

config_load_status_t load_equity_margin_for_config(
    Config *config, const char *equity_margin_string) {
  if (!is_decimal_number(equity_margin_string)) {
    return CONFIG_LOAD_STATUS_MALFORMED_EQUITY_MARGIN;
  }
  // The equity margin cannot be < 0 since the is_decimal_number
  // function returns false if the string contains "-"
  double equity_margin = string_to_double(equity_margin_string);
  config->equity_margin = equity_margin;
  return CONFIG_LOAD_STATUS_SUCCESS;
}

config_load_status_t load_target_number_of_tiles_exchanged_for_config(
    Config *config, const char *target_number_of_tiles_exchanged) {
  if (!is_all_digits_or_empty(target_number_of_tiles_exchanged)) {
    return CONFIG_LOAD_STATUS_MALFORMED_NUMBER_OF_TILES_EXCHANGED;
  }
  config->target_number_of_tiles_exchanged =
      string_to_int(target_number_of_tiles_exchanged);
  return CONFIG_LOAD_STATUS_SUCCESS;
}

config_load_status_t load_num_autoplay_rounds_for_config(
    Config *config, const char *num_autoplay_rounds) {
  if (!is_all_digits_or_empty(num_autoplay_rounds)) {
    return CONFIG_LOAD_STATUS_MALFORMED_MAX_ITERATIONS;
  }
  int rounds = string_to_int(num_autoplay_rounds);
  if (rounds < 1) {
    return CONFIG_LOAD_STATUS_MALFORMED_MAX_ITERATIONS;
  }
  config->num_autoplay_rounds = rounds;
  return CONFIG_LOAD_STATUS_SUCCESS;
}

config_load_status_t load_use_game_pairs_for_config(Config *config,
                                                    bool use_game_pairs) {
  config->use_game_pairs = use_game_pairs;
  return CONFIG_LOAD_STATUS_SUCCESS;
}

config_load_status_t load_random_seed_for_config(Config *config,
                                                 const char *seed) {
  if (!is_all_digits_or_empty(seed)) {
    return CONFIG_LOAD_STATUS_MALFORMED_RANDOM_SEED;
  }
  config->seed = string_to_uint64(seed);
  return CONFIG_LOAD_STATUS_SUCCESS;
}

config_load_status_t load_number_of_threads_for_config(
    Config *config, const char *number_of_threads_string) {
  if (!is_all_digits_or_empty(number_of_threads_string)) {
    return CONFIG_LOAD_STATUS_MALFORMED_NUMBER_OF_THREADS;
  }
  int number_of_threads = string_to_int(number_of_threads_string);
  if (number_of_threads < 1) {
    return CONFIG_LOAD_STATUS_MALFORMED_NUMBER_OF_THREADS;
  }
  if (number_of_threads > MAX_THREADS) {
    return CONFIG_LOAD_STATUS_EXCEEDED_MAX_NUMBER_OF_THREADS;
  }
  thread_control_set_threads(config->thread_control, number_of_threads);
  return CONFIG_LOAD_STATUS_SUCCESS;
}

config_load_status_t load_print_info_interval_for_config(
    Config *config, const char *print_info_interval) {
  if (!is_all_digits_or_empty(print_info_interval)) {
    return CONFIG_LOAD_STATUS_MALFORMED_PRINT_INFO_INTERVAL;
  }
  set_print_info_interval(config->thread_control,
                          string_to_int(print_info_interval));
  return CONFIG_LOAD_STATUS_SUCCESS;
}

config_load_status_t load_check_stop_interval_for_config(
    Config *config, const char *check_stop_interval) {
  if (!is_all_digits_or_empty(check_stop_interval)) {
    return CONFIG_LOAD_STATUS_MALFORMED_CHECK_STOP_INTERVAL;
  }
  thread_control_set_check_stop_interval(config->thread_control,
                                         string_to_int(check_stop_interval));
  return CONFIG_LOAD_STATUS_SUCCESS;
}

config_load_status_t load_io_for_config(Config *config, const char *infile_name,
                                        const char *outfile_name) {
  thread_control_set_io(config->thread_control, infile_name, outfile_name);
  return CONFIG_LOAD_STATUS_SUCCESS;
}

config_load_status_t load_mode_for_config(Config *config,
                                          exec_mode_t config_mode_type) {
  config->exec_mode = config_mode_type;
  config->command_set_exec_mode = true;
  return CONFIG_LOAD_STATUS_SUCCESS;
}

// Starts at the beginning of the parsed args to determine the
// type of command specified by the user. Does the following:
// - Sets the command type, if a valid one is found
// - Returns how many args were used for the command, so
//   further arg parsing can contiue without rereading the same args.
int set_command_type_for_config(Config *config, const ParsedArgs *parsed_args) {
  // If no valid sequences are found we use
  // the set options command as a default
  config->command_type = COMMAND_TYPE_SET_OPTIONS;
  int number_of_tokens_parsed = 0;
  for (int i = 0;
       VALID_COMMAND_SEQUENCES[i].arg_token_sequence[0] != NUMBER_OF_ARG_TOKENS;
       i++) {
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
      return number_of_tokens_parsed;
    }
  }
  return 0;
}

config_load_status_t set_cgp_string_for_config(Config *config,
                                               const SingleArg *cgp_arg) {
  free(config->cgp);
  // At this point it is guaranteed that cgp_arg has 4 values.
  config->cgp = get_formatted_string("%s %s %s %s", cgp_arg->values[0],
                                     cgp_arg->values[1], cgp_arg->values[2],
                                     cgp_arg->values[3]);
  return CONFIG_LOAD_STATUS_SUCCESS;
}

config_load_status_t set_moves_string_for_config(Config *config,
                                                 const char *moves) {
  free(config->moves);
  config->moves = string_duplicate(moves);
  return CONFIG_LOAD_STATUS_SUCCESS;
}

config_load_status_t load_player_name_for_config(Config *config,
                                                 int player_index,
                                                 const char *player_name) {
  players_data_set_name(config->players_data, player_index, player_name);
  return CONFIG_LOAD_STATUS_SUCCESS;
}

bool lds_are_compatible(const char *ld_name_1, const char *ld_name_2) {
  return strings_equal(ld_name_1, ld_name_2) ||
         // English and French use the same letters so they are
         // board_is_letter_allowed_in_cross_set to play against each other.
         (strings_equal(ld_name_1, ENGLISH_LETTER_DISTRIBUTION_NAME) &&
          strings_equal(ld_name_2, FRENCH_LETTER_DISTRIBUTION_NAME)) ||
         (strings_equal(ld_name_2, ENGLISH_LETTER_DISTRIBUTION_NAME) &&
          strings_equal(ld_name_1, FRENCH_LETTER_DISTRIBUTION_NAME));
}

bool lexicons_are_compatible(const char *p1_lexicon_name,
                             const char *p2_lexicon_name) {
  char *p1_ld = ld_get_default_name(p1_lexicon_name);
  char *p2_ld = ld_get_default_name(p2_lexicon_name);
  bool compatible = lds_are_compatible(p1_ld, p2_ld);
  free(p1_ld);
  free(p2_ld);
  return compatible;
}

bool ld_is_compatible_with_lexicon(const char *lexicon_name,
                                   const char *ld_name) {
  char *ld_from_lexicon_name = ld_get_default_name(lexicon_name);
  bool compatible = lds_are_compatible(ld_from_lexicon_name, ld_name);
  free(ld_from_lexicon_name);
  return compatible;
}

char *get_default_klv_name(const char *lexicon_name) {
  return string_duplicate(lexicon_name);
}

bool is_lexicon_required(const Config *config, const char *new_p1_leaves_name,
                         const char *new_p2_leaves_name,
                         const char *new_ld_name, const char *new_rack) {
  return config->command_type != COMMAND_TYPE_SET_OPTIONS || config->cgp ||
         config->cgp || config->moves || new_p1_leaves_name ||
         new_p2_leaves_name || new_ld_name || new_rack;
}

config_load_status_t load_lexicon_dependent_data_for_config(
    Config *config, const char *new_p1_lexicon_name,
    const char *new_p2_lexicon_name, const char *new_p1_leaves_name,
    const char *new_p2_leaves_name, const char *new_ld_name,
    const char *new_rack) {
  // Load lexicons first
  const char *existing_p1_lexicon_name = players_data_get_data_name(
      config->players_data, PLAYERS_DATA_TYPE_KWG, 0);
  const char *existing_p2_lexicon_name = players_data_get_data_name(
      config->players_data, PLAYERS_DATA_TYPE_KWG, 1);

  const char *updated_p1_lexicon_name = new_p1_lexicon_name;
  if (!updated_p1_lexicon_name) {
    updated_p1_lexicon_name = existing_p1_lexicon_name;
  }

  const char *updated_p2_lexicon_name = new_p2_lexicon_name;
  if (!updated_p2_lexicon_name) {
    updated_p2_lexicon_name = existing_p2_lexicon_name;
  }

  // Both or neither players must have lexical data
  if ((updated_p1_lexicon_name && !updated_p2_lexicon_name) ||
      (!updated_p1_lexicon_name && updated_p2_lexicon_name)) {
    return CONFIG_LOAD_STATUS_LEXICON_MISSING;
  }

  // Both lexicons are not specified, so we don't
  // load any of the lexicon dependent data
  if (!updated_p1_lexicon_name && !updated_p2_lexicon_name) {
    // We can use the new_* variables here since if lexicons
    // are null, it is guaranteed that there are no leaves, ld, or
    // rack since they are all set after this if check.
    if (is_lexicon_required(config, new_p1_leaves_name, new_p2_leaves_name,
                            new_ld_name, new_rack)) {
      return CONFIG_LOAD_STATUS_LEXICON_MISSING;
    } else {
      return CONFIG_LOAD_STATUS_SUCCESS;
    }
  }

  if (!lexicons_are_compatible(updated_p1_lexicon_name,
                               updated_p2_lexicon_name)) {
    return CONFIG_LOAD_STATUS_INCOMPATIBLE_LEXICONS;
  }

  players_data_set(config->players_data, PLAYERS_DATA_TYPE_KWG,
                   updated_p1_lexicon_name, updated_p2_lexicon_name);

  // Load the leaves

  char *updated_p1_leaves_name;
  if (new_p1_leaves_name) {
    updated_p1_leaves_name = string_duplicate(new_p1_leaves_name);
  } else {
    updated_p1_leaves_name = get_default_klv_name(updated_p1_lexicon_name);
  }

  char *updated_p2_leaves_name;
  if (new_p2_leaves_name) {
    updated_p2_leaves_name = string_duplicate(new_p2_leaves_name);
  } else {
    updated_p2_leaves_name = get_default_klv_name(updated_p2_lexicon_name);
  }

  if (!lexicons_are_compatible(updated_p1_leaves_name,
                               updated_p2_leaves_name) ||
      !lexicons_are_compatible(updated_p1_lexicon_name,
                               updated_p1_leaves_name) ||
      !lexicons_are_compatible(updated_p2_lexicon_name,
                               updated_p2_leaves_name)) {
    free(updated_p1_leaves_name);
    free(updated_p2_leaves_name);
    return CONFIG_LOAD_STATUS_INCOMPATIBLE_LEXICONS;
  }

  players_data_set(config->players_data, PLAYERS_DATA_TYPE_KLV,
                   updated_p1_leaves_name, updated_p2_leaves_name);

  free(updated_p1_leaves_name);
  free(updated_p2_leaves_name);

  // Load letter distribution

  char *updated_ld_name;
  if (!is_string_empty_or_null(new_ld_name)) {
    // if the user specified letter distribution
    // isn't compatible, return an error
    if (!ld_is_compatible_with_lexicon(updated_p1_lexicon_name, new_ld_name)) {
      return CONFIG_LOAD_STATUS_INCOMPATIBLE_LETTER_DISTRIBUTION;
    }
    updated_ld_name = string_duplicate(new_ld_name);
  } else if (is_string_empty_or_null(config->ld_name)) {
    // No letter distribution was specified and the current
    // letter distribution is null, so load the default letter
    // distribution based on player one's lexicon
    updated_ld_name = ld_get_default_name(updated_p1_lexicon_name);
  } else {
    // If the existing letter distribution isn't compatible,
    // just assume we want to use the default.
    if (!ld_is_compatible_with_lexicon(updated_p1_lexicon_name,
                                       config->ld_name)) {
      updated_ld_name = ld_get_default_name(updated_p1_lexicon_name);
    } else {
      updated_ld_name = string_duplicate(config->ld_name);
    }
  }

  // Here we can assume that the lexicons are already
  // compatible with each other, so we only need to
  // check the letter distribution with 1 lexicon

  // If the letter distribution name has changed, update it
  if (!strings_equal(config->ld_name, updated_ld_name)) {
    LetterDistribution *updated_ld = ld_create(updated_ld_name);
    ld_destroy(config->ld);
    config->ld = updated_ld;

    free(config->ld_name);
    config->ld_name = updated_ld_name;
    config->ld_name_changed = true;
  } else {
    free(updated_ld_name);
    config->ld_name_changed = false;
  }

  rack_destroy(config->rack);

  config->rack = rack_create(ld_get_size(config->ld));

  if (new_rack) {
    if (!strings_equal(EMPTY_RACK_STRING, new_rack)) {
      int number_of_letters_set =
          rack_set_to_string(config->ld, config->rack, new_rack);
      if (number_of_letters_set < 0) {
        return CONFIG_LOAD_STATUS_MALFORMED_RACK;
      }
    } else {
      rack_reset(config->rack);
    }
  }

  return CONFIG_LOAD_STATUS_SUCCESS;
}

config_load_status_t load_config_with_parsed_args(
    Config *config, const ParsedArgs *parsed_args) {
  int args_start_index = set_command_type_for_config(config, parsed_args);

  // Set the names using the args
  // and load the data once the args
  // are parsed.
  const char *new_p1_lexicon_name = NULL;
  const char *new_p1_leaves_name = NULL;
  const char *new_p2_lexicon_name = NULL;
  const char *new_p2_leaves_name = NULL;
  const char *new_ld_name = NULL;
  const char *new_p1_win_pct_name = NULL;
  const char *new_p2_win_pct_name = NULL;
  const char *new_rack = NULL;
  const char *outfile = NULL;
  const char *infile = NULL;
  config_load_status_t config_load_status = CONFIG_LOAD_STATUS_SUCCESS;
  for (int i = args_start_index; i < NUMBER_OF_ARG_TOKENS; i++) {
    if (!parsed_args->args[i]->has_value) {
      continue;
    }
    const SingleArg *single_arg = parsed_args->args[i];
    arg_token_t arg_token = single_arg->token;
    char **arg_values = single_arg->values;
    switch (arg_token) {
      case ARG_TOKEN_POSITION:
      case ARG_TOKEN_GO:
      case ARG_TOKEN_GEN:
      case ARG_TOKEN_SIM:
      case ARG_TOKEN_INFER:
      case ARG_TOKEN_AUTOPLAY:
      case ARG_TOKEN_SET_OPTIONS:
        config_load_status = CONFIG_LOAD_STATUS_MISPLACED_COMMAND;
        break;
      case ARG_TOKEN_CGP:
        config_load_status = set_cgp_string_for_config(config, single_arg);
        break;
      case ARG_TOKEN_MOVES:
        config_load_status = set_moves_string_for_config(config, arg_values[0]);
        break;
      case ARG_TOKEN_BINGO_BONUS:
        config_load_status = load_bingo_bonus_for_config(config, arg_values[0]);
        break;
      case ARG_TOKEN_BOARD_LAYOUT:
        config_load_status =
            load_board_layout_for_config(config, arg_values[0]);
        break;
      case ARG_TOKEN_GAME_VARIANT:
        config_load_status =
            load_game_variant_for_config(config, arg_values[0]);
        break;
      case ARG_TOKEN_LETTER_DISTRIBUTION:
        new_ld_name = arg_values[0];
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
        new_rack = arg_values[0];
        break;
      case ARG_TOKEN_WIN_PCT:
        new_p1_win_pct_name = arg_values[0];
        new_p2_win_pct_name = arg_values[0];
        break;
      case ARG_TOKEN_P1_WIN_PCT:
        new_p1_win_pct_name = arg_values[0];
        break;
      case ARG_TOKEN_P2_WIN_PCT:
        new_p2_win_pct_name = arg_values[0];
        break;
      case ARG_TOKEN_NUMBER_OF_PLAYS:
        config_load_status =
            players_data_load_num_plays(config->players_data, arg_values[0], 0);
        config_load_status =
            players_data_load_num_plays(config->players_data, arg_values[0], 1);
        break;
      case ARG_TOKEN_P1_NUMBER_OF_PLAYS:
        config_load_status =
            players_data_load_num_plays(config->players_data, arg_values[0], 0);
        break;
      case ARG_TOKEN_P2_NUMBER_OF_PLAYS:
        config_load_status =
            players_data_load_num_plays(config->players_data, arg_values[0], 1);
        break;
      case ARG_TOKEN_PLIES:
        config_load_status =
            players_data_load_plies(config->players_data, arg_values[0], 0);
        config_load_status =
            players_data_load_plies(config->players_data, arg_values[0], 1);
        break;
      case ARG_TOKEN_P1_PLIES:
        config_load_status =
            players_data_load_plies(config->players_data, arg_values[0], 0);
        break;
      case ARG_TOKEN_P2_PLIES:
        config_load_status =
            players_data_load_plies(config->players_data, arg_values[0], 1);
        break;
      case ARG_TOKEN_NUMBER_OF_AUTOPLAY_ROUNDS:
        config_load_status =
            load_num_autoplay_rounds_for_config(config, arg_values[0]);
        break;
      case ARG_TOKEN_MAX_ITERATIONS:
        config_load_status =
            load_num_autoplay_rounds_for_config(config, arg_values[0]);
        config_load_status = players_data_load_max_iterations(
            config->players_data, arg_values[0], 0);
        config_load_status = players_data_load_max_iterations(
            config->players_data, arg_values[0], 1);
        break;
      case ARG_TOKEN_P1_MAX_ITERATIONS:
        config_load_status = players_data_load_max_iterations(
            config->players_data, arg_values[0], 0);
        break;
      case ARG_TOKEN_P2_MAX_ITERATIONS:
        config_load_status = players_data_load_max_iterations(
            config->players_data, arg_values[0], 1);
        break;
      case ARG_TOKEN_STOPPING_CONDITION:
        config_load_status = players_data_load_stopping_condition(
            config->players_data, arg_values[0], 0);
        players_data_load_stopping_condition(config->players_data,
                                             arg_values[0], 1);
        break;
      case ARG_TOKEN_P1_STOPPING_CONDITION:
        config_load_status = players_data_load_stopping_condition(
            config->players_data, arg_values[0], 0);
        break;
      case ARG_TOKEN_P2_STOPPING_CONDITION:
        config_load_status = players_data_load_stopping_condition(
            config->players_data, arg_values[0], 1);
        break;
      case ARG_TOKEN_target_index:
        config_load_status =
            load_target_index_for_config(config, arg_values[0]);
        break;
      case ARG_TOKEN_SCORE:
        config_load_status = load_score_for_config(config, arg_values[0]);
        break;
      case ARG_TOKEN_EQUITY_MARGIN:
        config_load_status =
            load_equity_margin_for_config(config, arg_values[0]);
        break;
      case ARG_TOKEN_target_number_of_tiles_exchanged:
        config_load_status = load_target_number_of_tiles_exchanged_for_config(
            config, arg_values[0]);
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
        config->command_set_infile = true;
        infile = arg_values[0];
        break;
      case ARG_TOKEN_OUTFILE:
        outfile = arg_values[0];
        break;
      case ARG_TOKEN_CONSOLE_MODE:
        if (config->command_set_exec_mode) {
          config_load_status = CONFIG_LOAD_STATUS_MULTIPLE_EXEC_MODES;
        } else {
          config_load_status = load_mode_for_config(config, EXEC_MODE_CONSOLE);
        }
        break;
      case ARG_TOKEN_UCGI_MODE:
        if (config->command_set_exec_mode) {
          config_load_status = CONFIG_LOAD_STATUS_MULTIPLE_EXEC_MODES;
        } else {
          config_load_status = load_mode_for_config(config, EXEC_MODE_UCGI);
        }
        break;
      case NUMBER_OF_ARG_TOKENS:
        log_fatal("invalid token found in args\n");
        break;
    }
    if (config_load_status != CONFIG_LOAD_STATUS_SUCCESS) {
      return config_load_status;
    }
  }

  config_load_status = load_lexicon_dependent_data_for_config(
      config, new_p1_lexicon_name, new_p2_lexicon_name, new_p1_leaves_name,
      new_p2_leaves_name, new_ld_name, new_rack);
  if (config_load_status != CONFIG_LOAD_STATUS_SUCCESS) {
    return config_load_status;
  }

  config_load_status = load_io_for_config(config, infile, outfile);
  if (config_load_status != CONFIG_LOAD_STATUS_SUCCESS) {
    return config_load_status;
  }

  if (players_data_load_win_pct(config->players_data, new_p1_win_pct_name, 0) !=
      CONFIG_LOAD_STATUS_SUCCESS) {
    return config_load_status;
  }

  return players_data_load_win_pct(config->players_data, new_p2_win_pct_name,
                                   1);
}

void reset_transient_fields(Config *config) {
  config->command_set_infile = false;
  config->command_set_exec_mode = false;
  config->seed = time(NULL);
  free(config->cgp);
  config->cgp = NULL;
  free(config->moves);
  config->moves = NULL;
}

config_load_status_t config_load(Config *config, const char *cmd) {
  reset_transient_fields(config);
  // If the command is empty, consider this a set options
  // command where zero options are set and return without error.
  if (is_string_empty_or_whitespace(cmd)) {
    config->command_type = COMMAND_TYPE_SET_OPTIONS;
    return CONFIG_LOAD_STATUS_SUCCESS;
  }

  ParsedArgs *parsed_args = create_parsed_args();
  StringSplitter *cmd_split_string = split_string_by_whitespace(cmd, true);
  // CGP values can have semicolons at the end, so
  // we trim these off to make loading easier.
  string_splitter_trim_char(cmd_split_string, ';');
  config_load_status_t config_load_status =
      init_parsed_args(cmd_split_string, parsed_args);

  if (config_load_status == CONFIG_LOAD_STATUS_SUCCESS) {
    config_load_status = load_config_with_parsed_args(config, parsed_args);
  }

  destroy_parsed_args(parsed_args);
  destroy_string_splitter(cmd_split_string);

  return config_load_status;
}

Config *config_create_default() {
  Config *config = malloc_or_die(sizeof(Config));
  config->command_set_infile = false;
  config->command_set_exec_mode = false;
  config->moves = NULL;
  config->command_type = COMMAND_TYPE_SET_OPTIONS;
  config->ld = NULL;
  config->ld_name = NULL;
  config->ld_name_changed = false;
  config->cgp = NULL;
  config->bingo_bonus = DEFAULT_BINGO_BONUS;
  config->board_layout = DEFAULT_BOARD_LAYOUT;
  config->game_variant = DEFAULT_GAME_VARIANT;
  config->players_data = players_data_create();
  config->rack = NULL;
  config->target_index = 0;
  config->target_score = 0;
  config->target_number_of_tiles_exchanged = 0;
  config->equity_margin = 0;
  config->num_autoplay_rounds = 0;
  config->win_pcts = NULL;
  config->win_pct_name = NULL;
  config->num_plays = DEFAULT_MOVE_LIST_CAPACITY;
  config->plies = 2;
  config->max_iterations = 0;
  config->stopping_condition = DEFAULT_SIMMING_STOPPING_CONDITION;
  config->use_game_pairs = false;
  // The seed is set to a random value by default for each
  // load in reset_transient_fields.
  config->seed = 0;
  config->thread_control = thread_control_create();
  config->exec_mode = EXEC_MODE_CONSOLE;
  return config;
}

void config_destroy(Config *config) {
  if (!config) {
    return;
  }
  ld_destroy(config->ld);
  rack_destroy(config->rack);
  free(config->ld_name);
  free(config->cgp);
  free(config->moves);
  players_data_destroy(config->players_data);
  thread_control_destroy(config->thread_control);
  free(config);
}