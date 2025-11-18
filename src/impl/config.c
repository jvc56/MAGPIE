#include "config.h"

#include "../compat/ctime.h"
#include "../def/autoplay_defs.h"
#include "../def/bai_defs.h"
#include "../def/config_defs.h"
#include "../def/equity_defs.h"
#include "../def/game_defs.h"
#include "../def/game_history_defs.h"
#include "../def/inference_args_defs.h"
#include "../def/move_defs.h"
#include "../def/players_data_defs.h"
#include "../def/rack_defs.h"
#include "../def/sim_defs.h"
#include "../def/thread_control_defs.h"
#include "../def/validated_move_defs.h"
#include "../ent/autoplay_results.h"
#include "../ent/bag.h"
#include "../ent/bai_result.h"
#include "../ent/board.h"
#include "../ent/board_layout.h"
#include "../ent/conversion_results.h"
#include "../ent/endgame_results.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/game_history.h"
#include "../ent/inference_results.h"
#include "../ent/klv.h"
#include "../ent/klv_csv.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/player.h"
#include "../ent/players_data.h"
#include "../ent/rack.h"
#include "../ent/sim_results.h"
#include "../ent/thread_control.h"
#include "../ent/validated_move.h"
#include "../ent/win_pct.h"
#include "../str/game_string.h"
#include "../str/move_string.h"
#include "../str/rack_string.h"
#include "../str/validated_moves_string.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "autoplay.h"
#include "cgp.h"
#include "convert.h"
#include "endgame.h"
#include "gameplay.h"
#include "gcg.h"
#include "get_gcg.h"
#include "inference.h"
#include "move_gen.h"
#include "random_variable.h"
#include "simmer.h"
#include <assert.h>
#include <ctype.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
  ARG_TOKEN_SET,
  ARG_TOKEN_CGP,
  ARG_TOKEN_MOVES,
  ARG_TOKEN_RACK,
  ARG_TOKEN_GEN,
  ARG_TOKEN_SIM,
  ARG_TOKEN_INFER,
  ARG_TOKEN_ENDGAME,
  ARG_TOKEN_AUTOPLAY,
  ARG_TOKEN_CONVERT,
  ARG_TOKEN_LEAVE_GEN,
  ARG_TOKEN_CREATE_DATA,
  ARG_TOKEN_DATA_PATH,
  ARG_TOKEN_BINGO_BONUS,
  ARG_TOKEN_CHALLENGE_BONUS,
  ARG_TOKEN_BOARD_LAYOUT,
  ARG_TOKEN_GAME_VARIANT,
  ARG_TOKEN_LETTER_DISTRIBUTION,
  ARG_TOKEN_LEXICON,
  ARG_TOKEN_USE_WMP,
  ARG_TOKEN_LEAVES,
  ARG_TOKEN_P1_NAME,
  ARG_TOKEN_P1_LEXICON,
  ARG_TOKEN_P1_USE_WMP,
  ARG_TOKEN_P1_LEAVES,
  ARG_TOKEN_P1_MOVE_SORT_TYPE,
  ARG_TOKEN_P1_MOVE_RECORD_TYPE,
  ARG_TOKEN_P1_SIM_PLIES,
  ARG_TOKEN_P1_SIM_NUM_PLAYS,
  ARG_TOKEN_P1_SIM_MAX_ITERATIONS,
  ARG_TOKEN_P1_SIM_STOP_COND_PCT,
  ARG_TOKEN_P1_SIM_MIN_PLAY_ITERATIONS,
  ARG_TOKEN_P1_SIM_USE_INFERENCE,
  ARG_TOKEN_P2_NAME,
  ARG_TOKEN_P2_LEXICON,
  ARG_TOKEN_P2_USE_WMP,
  ARG_TOKEN_P2_LEAVES,
  ARG_TOKEN_P2_MOVE_SORT_TYPE,
  ARG_TOKEN_P2_MOVE_RECORD_TYPE,
  ARG_TOKEN_P2_SIM_PLIES,
  ARG_TOKEN_P2_SIM_NUM_PLAYS,
  ARG_TOKEN_P2_SIM_MAX_ITERATIONS,
  ARG_TOKEN_P2_SIM_STOP_COND_PCT,
  ARG_TOKEN_P2_SIM_MIN_PLAY_ITERATIONS,
  ARG_TOKEN_P2_SIM_USE_INFERENCE,
  ARG_TOKEN_WIN_PCT,
  ARG_TOKEN_PLIES,
  ARG_TOKEN_ENDGAME_PLIES,
  ARG_TOKEN_NUMBER_OF_PLAYS,
  ARG_TOKEN_NUMBER_OF_SMALL_PLAYS,
  ARG_TOKEN_NUM_LEAVES,
  ARG_TOKEN_MAX_ITERATIONS,
  ARG_TOKEN_STOP_COND_PCT,
  ARG_TOKEN_EQ_MARGIN_INFERENCE,
  ARG_TOKEN_EQ_MARGIN_MOVEGEN,
  ARG_TOKEN_MIN_PLAY_ITERATIONS,
  ARG_TOKEN_USE_GAME_PAIRS,
  ARG_TOKEN_USE_SMALL_PLAYS,
  ARG_TOKEN_SIM_WITH_INFERENCE,
  ARG_TOKEN_WRITE_BUFFER_SIZE,
  ARG_TOKEN_HUMAN_READABLE,
  ARG_TOKEN_RANDOM_SEED,
  ARG_TOKEN_NUMBER_OF_THREADS,
  ARG_TOKEN_MULTI_THREADED_SIMS,
  ARG_TOKEN_PRINT_INFO_INTERVAL,
  ARG_TOKEN_EXEC_MODE,
  ARG_TOKEN_TT_FRACTION_OF_MEM,
  ARG_TOKEN_TIME_LIMIT,
  ARG_TOKEN_SAMPLING_RULE,
  ARG_TOKEN_THRESHOLD,
  ARG_TOKEN_LOAD,
  ARG_TOKEN_NEW_GAME,
  ARG_TOKEN_EXPORT,
  ARG_TOKEN_COMMIT,
  ARG_TOKEN_CHALLENGE,
  ARG_TOKEN_UNCHALLENGE,
  ARG_TOKEN_OVERTIME,
  ARG_TOKEN_SWITCH_NAMES,
  ARG_TOKEN_SHOW,
  ARG_TOKEN_NEXT,
  ARG_TOKEN_PREVIOUS,
  ARG_TOKEN_GOTO,
  ARG_TOKEN_PRINT_BOARDS,
  ARG_TOKEN_BOARD_COLOR,
  ARG_TOKEN_BOARD_TILE_GLYPHS,
  ARG_TOKEN_BOARD_BORDER,
  ARG_TOKEN_BOARD_COLUMN_LABEL,
  ARG_TOKEN_ON_TURN_MARKER,
  ARG_TOKEN_ON_TURN_COLOR,
  ARG_TOKEN_ON_TURN_SCORE_STYLE,
  ARG_TOKEN_PRETTY,
  ARG_TOKEN_PRINT_ON_FINISH,
  ARG_TOKEN_SHOW_PROMPT,
  // This must always be the last
  // token for the count to be accurate
  NUMBER_OF_ARG_TOKENS
} arg_token_t;

typedef void (*command_exec_func_t)(Config *, ErrorStack *);
typedef char *(*command_api_func_t)(Config *, ErrorStack *);
typedef char *(*command_status_func_t)(Config *);

typedef struct ParsedArg {
  char *name;
  char **values;
  int num_req_values;
  int num_values;
  int num_set_values;
  command_exec_func_t exec_func;
  command_api_func_t api_func;
  command_status_func_t status_func;
} ParsedArg;

struct Config {
  ParsedArg *pargs[NUMBER_OF_ARG_TOKENS];
  char *data_paths;
  arg_token_t exec_parg_token;
  bool ld_changed;
  exec_mode_t exec_mode;
  int bingo_bonus;
  int challenge_bonus;
  int num_plays;
  int p1_sim_num_plays;
  int p2_sim_num_plays;
  int num_small_plays;
  int num_leaves;
  int plies;
  int p1_sim_plies;
  int p2_sim_plies;
  int endgame_plies;
  int max_iterations;
  int p1_sim_max_iterations;
  int p2_sim_max_iterations;
  int min_play_iterations;
  int p1_sim_min_play_iterations;
  int p2_sim_min_play_iterations;
  double stop_cond_pct;
  double p1_sim_stop_cond_pct;
  double p2_sim_stop_cond_pct;
  bool p1_sim_use_inference;
  bool p2_sim_use_inference;
  Equity eq_margin_inference;
  Equity eq_margin_movegen;
  bool use_game_pairs;
  bool human_readable;
  bool use_small_plays;
  bool sim_with_inference;
  bool print_boards;
  bool print_on_finish;
  bool show_prompt;
  char *record_filepath;
  double tt_fraction_of_mem;
  int time_limit_seconds;
  int num_threads;
  int print_interval;
  uint64_t seed;
  bai_sampling_rule_t sampling_rule;
  bai_threshold_t threshold;
  game_variant_t game_variant;
  WinPct *win_pcts;
  BoardLayout *board_layout;
  LetterDistribution *ld;
  PlayersData *players_data;
  ThreadControl *thread_control;
  Game *game;
  Game *game_backup;
  GameHistory *game_history;
  GameHistory *game_history_backup;
  MoveList *move_list;
  EndgameSolver *endgame_solver;
  SimResults *sim_results;
  InferenceResults *inference_results;
  EndgameResults *endgame_results;
  AutoplayResults *autoplay_results;
  ConversionResults *conversion_results;
  GameStringOptions *game_string_options;
};

void parsed_arg_create(Config *config, arg_token_t arg_token, const char *name,
                       int num_req_values, int num_values,
                       command_exec_func_t command_exec_func,
                       command_api_func_t command_api_func,
                       command_status_func_t command_status_func) {
  ParsedArg *parsed_arg = malloc_or_die(sizeof(ParsedArg));
  parsed_arg->num_req_values = num_req_values;
  parsed_arg->num_values = num_values;
  parsed_arg->values = malloc_or_die(sizeof(char *) * parsed_arg->num_values);
  for (int i = 0; i < parsed_arg->num_values; i++) {
    parsed_arg->values[i] = NULL;
  }
  parsed_arg->name = string_duplicate(name);
  parsed_arg->num_set_values = 0;
  parsed_arg->exec_func = command_exec_func;
  parsed_arg->api_func = command_api_func;
  parsed_arg->status_func = command_status_func;

  config->pargs[arg_token] = parsed_arg;
}

void parsed_arg_destroy(ParsedArg *parsed_arg) {
  if (!parsed_arg) {
    return;
  }

  free(parsed_arg->name);

  if (parsed_arg->values) {
    for (int i = 0; i < parsed_arg->num_values; i++) {
      free(parsed_arg->values[i]);
    }
    free(parsed_arg->values);
  }

  free(parsed_arg);
}

// Parsed arg getters

ParsedArg *config_get_parg(const Config *config, arg_token_t arg_token) {
  return config->pargs[arg_token];
}

command_exec_func_t config_get_parg_exec_func(const Config *config,
                                              arg_token_t arg_token) {
  return config_get_parg(config, arg_token)->exec_func;
}

command_api_func_t config_get_parg_api_func(const Config *config,
                                            arg_token_t arg_token) {
  return config_get_parg(config, arg_token)->api_func;
}

command_status_func_t config_get_parg_status_func(const Config *config,
                                                  arg_token_t arg_token) {
  return config_get_parg(config, arg_token)->status_func;
}

const char *config_get_parg_name(const Config *config, arg_token_t arg_token) {
  return config_get_parg(config, arg_token)->name;
}

const char *config_get_current_exec_name(const Config *config) {
  return config_get_parg_name(config, config->exec_parg_token);
}

// Returns NULL if the value was not set in the most recent config load call.
const char *config_get_parg_value(const Config *config, arg_token_t arg_token,
                                  int value_index) {
  ParsedArg *parg = config_get_parg(config, arg_token);
  if (value_index >= parg->num_values) {
    log_fatal("value index exceeds number of values for %d: %d >= %d",
              arg_token, value_index, parg->num_values);
  }
  const char *ret_val = NULL;
  if (value_index < parg->num_set_values) {
    ret_val = parg->values[value_index];
  }
  return ret_val;
}

int config_get_parg_num_set_values(const Config *config,
                                   arg_token_t arg_token) {
  return config_get_parg(config, arg_token)->num_set_values;
}

int config_get_parg_num_req_values(const Config *config,
                                   arg_token_t arg_token) {
  return config_get_parg(config, arg_token)->num_req_values;
}

// Config getters

const char *config_get_data_paths(const Config *config) {
  return config->data_paths;
}

exec_mode_t config_get_exec_mode(const Config *config) {
  return config->exec_mode;
}

int config_get_bingo_bonus(const Config *config) { return config->bingo_bonus; }

int config_get_num_plays(const Config *config) { return config->num_plays; }

int config_get_num_leaves(const Config *config) { return config->num_leaves; }

int config_get_num_small_plays(const Config *config) {
  return config->num_small_plays;
}
int config_get_plies(const Config *config) { return config->plies; }

int config_get_endgame_plies(const Config *config) {
  return config->endgame_plies;
}

int config_get_max_iterations(const Config *config) {
  return config->max_iterations;
}

double config_get_stop_cond_pct(const Config *config) {
  return config->stop_cond_pct;
}

int config_get_time_limit_seconds(const Config *config) {
  return config->time_limit_seconds;
}

uint64_t config_get_seed(const Config *config) { return config->seed; }

double config_get_tt_fraction_of_mem(const Config *config) {
  return config->tt_fraction_of_mem;
}

BoardLayout *config_get_board_layout(const Config *config) {
  return config->board_layout;
}

game_variant_t config_get_game_variant(const Config *config) {
  return config->game_variant;
}

WinPct *config_get_win_pcts(const Config *config) { return config->win_pcts; }

bool config_get_use_game_pairs(const Config *config) {
  return config->use_game_pairs;
}

bool config_get_use_small_plays(const Config *config) {
  return config->use_small_plays;
}

bool config_get_human_readable(const Config *config) {
  return config->human_readable;
}

bool config_get_show_prompt(const Config *config) {
  return config->show_prompt;
}

PlayersData *config_get_players_data(const Config *config) {
  return config->players_data;
}

LetterDistribution *config_get_ld(const Config *config) { return config->ld; }

ThreadControl *config_get_thread_control(const Config *config) {
  return config->thread_control;
}

Game *config_get_game(const Config *config) { return config->game; }

GameHistory *config_get_game_history(const Config *config) {
  return config->game_history;
}

MoveList *config_get_move_list(const Config *config) {
  return config->move_list;
}

SimResults *config_get_sim_results(const Config *config) {
  return config->sim_results;
}

EndgameResults *config_get_endgame_results(const Config *config) {
  return config->endgame_results;
}

AutoplayResults *config_get_autoplay_results(const Config *config) {
  return config->autoplay_results;
}

bool config_exec_parg_is_set(const Config *config) {
  return config->exec_parg_token != NUMBER_OF_ARG_TOKENS;
}

bool config_continue_on_coldstart(const Config *config) {
  return !config_exec_parg_is_set(config) ||
         config->exec_parg_token == ARG_TOKEN_CGP ||
         config_get_parg_num_set_values(config, ARG_TOKEN_EXEC_MODE) > 0;
}

// Config command execution helper functions

bool is_game_recreation_required(const Config *config) {
  // If the ld changes (bag and rack size)
  // a recreation is required to resize the
  // dynamically allocated fields.
  return config->ld_changed;
}

void config_fill_game_args(const Config *config, GameArgs *game_args) {
  game_args->players_data = config->players_data;
  game_args->board_layout = config->board_layout;
  game_args->ld = config->ld;
  game_args->bingo_bonus = config->bingo_bonus;
  game_args->game_variant = config->game_variant;
  game_args->seed = config->seed;
}

Game *config_game_create(const Config *config) {
  GameArgs game_args;
  config_fill_game_args(config, &game_args);
  return game_create(&game_args);
}

void config_game_update(const Config *config, Game *game) {
  GameArgs game_args;
  config_fill_game_args(config, &game_args);
  game_update(game, &game_args);
}

// Creates a game if none exists or recreates the game
// if the ld size or other dynamically allocated
// data sizes have changed.
//
// Preconditions:
//  - The config is loaded
void config_init_game(Config *config) {
  if (config->game && is_game_recreation_required(config)) {
    game_destroy(config->game);
    config->game = NULL;
  }

  if (!config->game) {
    config->game = config_game_create(config);
  } else {
    config_game_update(config, config->game);
  }
}

void config_reset_move_list(Config *config) {
  if (config->move_list) {
    move_list_reset(config->move_list);
  }
}

void config_init_move_list(Config *config, int capacity) {
  if (!config->move_list) {
    config->move_list = move_list_create(capacity);
  }
}

void config_recreate_move_list(Config *config, int capacity,
                               move_list_type_t list_type) {
  if (list_type == MOVE_LIST_TYPE_DEFAULT) {
    config_init_move_list(config, capacity);
    if (move_list_get_capacity(config->move_list) == capacity) {
      move_list_reset(config->move_list);
    } else {
      move_list_destroy(config->move_list);
      config->move_list = move_list_create(capacity);
    }
  } else if (list_type == MOVE_LIST_TYPE_SMALL) {
    if (!config->move_list) {
      config->move_list = move_list_create_small(capacity);
      move_list_reset(config->move_list);
    }
  }
}

void string_to_int_or_push_error(const char *int_str_name, const char *int_str,
                                 int min, int max, error_code_t error_code,
                                 int *dest, ErrorStack *error_stack) {
  *dest = string_to_int(int_str, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_push(
        error_stack, error_code,
        get_formatted_string("failed to parse value '%s' for argument '%s'",
                             int_str, int_str_name));
  }
  if (*dest < min || *dest > max) {
    error_stack_push(
        error_stack, error_code,
        get_formatted_string(
            "value for %s must be between %d and %d inclusive, but got '%s'",
            int_str_name, min, max, int_str));
  }
}

void load_rack_or_push_to_error_stack(const char *rack_str,
                                      const LetterDistribution *ld,
                                      error_code_t error_code, Rack *rack,
                                      ErrorStack *error_stack) {
  const int num_mls = rack_set_to_string(ld, rack, rack_str);
  if (num_mls < 0) {
    error_stack_push(
        error_stack, error_code,
        get_formatted_string("failed to parse rack '%s'", rack_str));
  } else if (num_mls > RACK_SIZE) {
    error_stack_push(
        error_stack, error_code,
        get_formatted_string(
            "rack '%s' has %d tiles which exceeds the maximum rack size of %d",
            rack_str, num_mls, RACK_SIZE));
  }
}

// Returns true if the config has all of the required
// data to create a game.
bool config_has_game_data(const Config *config) {
  // We use the letter distribution as a proxy for
  // whether the game data has been loaded because
  // the config load logic ensures that lexicons
  // and leaves are guaranteed to be loaded successfully before
  // the letter distribution.
  return config->ld != NULL;
}

// Config execution functions

// Config loading for primitive types

void config_load_int(const Config *config, arg_token_t arg_token, int min,
                     int max, int *value, ErrorStack *error_stack) {
  const char *int_str = config_get_parg_value(config, arg_token, 0);
  if (!int_str) {
    return;
  }
  int new_value = string_to_int(int_str, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_push(
        error_stack, ERROR_STATUS_CONFIG_LOAD_MALFORMED_INT_ARG,
        get_formatted_string("failed to parse integer value for %s: %s",
                             config_get_parg_name(config, arg_token), int_str));
    return;
  }
  if (new_value < min || new_value > max) {
    error_stack_push(
        error_stack, ERROR_STATUS_CONFIG_LOAD_INT_ARG_OUT_OF_BOUNDS,
        get_formatted_string("int value for %s must be between %d and %d "
                             "inclusive, but got %s",
                             config_get_parg_name(config, arg_token), min, max,
                             int_str));
    return;
  }
  *value = new_value;
}

void config_load_double(const Config *config, arg_token_t arg_token, double min,
                        double max, double *value, ErrorStack *error_stack) {
  const char *double_str = config_get_parg_value(config, arg_token, 0);
  if (!double_str) {
    return;
  }
  double new_value = string_to_double(double_str, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_push(error_stack, ERROR_STATUS_CONFIG_LOAD_MALFORMED_DOUBLE_ARG,
                     get_formatted_string(
                         "failed to parse decimal value for %s: %s",
                         config_get_parg_name(config, arg_token), double_str));
    return;
  }
  if (new_value < min || new_value > max) {
    error_stack_push(
        error_stack, ERROR_STATUS_CONFIG_LOAD_DOUBLE_ARG_OUT_OF_BOUNDS,
        get_formatted_string(
            "double value for %s must be between %f and %f inclusive, but "
            "got %s",
            config_get_parg_name(config, arg_token), min, max, double_str));
    return;
  }
  *value = new_value;
}

void config_load_bool(const Config *config, arg_token_t arg_token, bool *value,
                      ErrorStack *error_stack) {
  const char *bool_str = config_get_parg_value(config, arg_token, 0);
  if (!bool_str) {
    return;
  }
  if (has_iprefix(bool_str, "true")) {
    *value = true;
  } else if (has_iprefix(bool_str, "false")) {
    *value = false;
  } else {
    error_stack_push(error_stack, ERROR_STATUS_CONFIG_LOAD_MALFORMED_BOOL_ARG,
                     get_formatted_string(
                         "failed to parse bool value for %s: %s",
                         config_get_parg_name(config, arg_token), bool_str));
  }
}

void config_load_uint64(const Config *config, arg_token_t arg_token,
                        uint64_t *value, ErrorStack *error_stack) {
  const char *int_str = config_get_parg_value(config, arg_token, 0);
  if (!int_str) {
    return;
  }
  uint64_t new_value;
  bool success = true;
  if (!is_all_digits_or_empty(int_str)) {
    success = false;
  } else {
    new_value = string_to_uint64(int_str, error_stack);
  }
  if (!success || !error_stack_is_empty(error_stack)) {
    error_stack_push(error_stack, ERROR_STATUS_CONFIG_LOAD_MALFORMED_INT_ARG,
                     get_formatted_string(
                         "failed to parse nonnegative integer value for %s: %s",
                         config_get_parg_name(config, arg_token), int_str));
  } else {
    *value = new_value;
  }
}

// Generic execution and status functions

// Used for pargs that are not commands.
void execute_fatal(Config *config,
                   ErrorStack __attribute__((unused)) * error_stack) {
  log_fatal("attempted to execute nonexecutable argument (arg token %d)",
            config->exec_parg_token);
}

char *str_api_fatal(Config *config,
                    ErrorStack __attribute__((unused)) * error_stack) {
  execute_fatal(config, error_stack);
  return empty_string();
}

// Used for commands that only update the config state
void execute_noop(Config __attribute__((unused)) * config,
                  ErrorStack __attribute__((unused)) * error_stack) {}

char *str_api_noop(Config __attribute__((unused)) * config,
                   ErrorStack __attribute__((unused)) * error_stack) {
  return empty_string();
}

char *get_status_finished_str(const Config *config) {
  return get_formatted_string("%s %s\n", COMMAND_FINISHED_KEYWORD,
                              config_get_current_exec_name(config));
}

char *get_status_running_str(const Config *config) {
  return get_formatted_string("%s %s\n", COMMAND_RUNNING_KEYWORD,
                              config_get_current_exec_name(config));
}

char *status_generic(Config *config) {
  char *status_str = NULL;
  if (thread_control_get_status(config->thread_control) ==
      THREAD_CONTROL_STATUS_FINISHED) {
    status_str = get_status_finished_str(config);
  } else {
    status_str = get_status_running_str(config);
  }
  return status_str;
}

// Load CGP

void impl_load_cgp(Config *config, ErrorStack *error_stack) {
  if (!config_has_game_data(config)) {
    error_stack_push(error_stack, ERROR_STATUS_CONFIG_LOAD_GAME_DATA_MISSING,
                     string_duplicate("cannot load cgp without lexicon"));
    return;
  }

  config_init_game(config);

  StringBuilder *cgp_builder = string_builder_create();
  int num_req_values = config_get_parg_num_req_values(config, ARG_TOKEN_CGP);
  for (int i = 0; i < num_req_values; i++) {
    const char *cgp_component = config_get_parg_value(config, ARG_TOKEN_CGP, i);
    if (!cgp_component) {
      // This should have been caught earlier by the insufficient values error
      log_fatal("missing cgp component: %d", i);
    }
    string_builder_add_string(cgp_builder, cgp_component);
    string_builder_add_char(cgp_builder, ' ');
  }

  const char *cgp = string_builder_peek(cgp_builder);
  // First duplicate the game so that potential
  // cgp parse failures don't corrupt it.
  Game *game_dupe = game_duplicate(config->game);
  game_load_cgp(game_dupe, cgp, error_stack);
  game_destroy(game_dupe);
  if (error_stack_is_empty(error_stack)) {
    // Now that the duplicate game has been successfully loaded
    // with the cgp, load the actual game. A cgp parse failure
    // here should be impossible.
    game_load_cgp(config->game, cgp, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      error_stack_print_and_reset(error_stack);
      log_fatal("cgp load unexpected failed");
    }
    config_reset_move_list(config);
  }
  string_builder_destroy(cgp_builder);
}

// Adding moves

void impl_add_moves(Config *config, ErrorStack *error_stack) {
  if (!config_has_game_data(config)) {
    error_stack_push(error_stack, ERROR_STATUS_CONFIG_LOAD_GAME_DATA_MISSING,
                     string_duplicate("cannot add moves without lexicon"));
    return;
  }

  config_init_game(config);

  const char *moves = config_get_parg_value(config, ARG_TOKEN_MOVES, 0);

  int player_on_turn_index = game_get_player_on_turn_index(config->game);

  ValidatedMoves *new_validated_moves =
      validated_moves_create(config->game, player_on_turn_index, moves, true,
                             false, true, error_stack);

  if (error_stack_is_empty(error_stack)) {
    const LetterDistribution *ld = game_get_ld(config->game);
    const Board *board = game_get_board(config->game);
    StringBuilder *phonies_sb = string_builder_create();
    int number_of_new_moves =
        validated_moves_get_number_of_moves(new_validated_moves);
    for (int i = 0; i < number_of_new_moves; i++) {
      char *phonies_formed = validated_moves_get_phonies_string(
          game_get_ld(config->game), new_validated_moves, i);
      if (phonies_formed) {
        string_builder_clear(phonies_sb);
        string_builder_add_string(phonies_sb, "invalid words formed from ");
        string_builder_add_move(
            phonies_sb, board, validated_moves_get_move(new_validated_moves, i),
            ld);
        string_builder_add_string(phonies_sb, ": ");
        string_builder_add_string(phonies_sb, phonies_formed);
        write_to_stream_out(string_builder_peek(phonies_sb));
      }
      free(phonies_formed);
    }
    string_builder_destroy(phonies_sb);
    config_init_move_list(config, number_of_new_moves);
    validated_moves_add_to_sorted_move_list(new_validated_moves,
                                            config->move_list);
  }

  validated_moves_destroy(new_validated_moves);
}

// Setting player rack

void impl_set_rack(Config *config, ErrorStack *error_stack) {
  if (!config_has_game_data(config)) {
    error_stack_push(
        error_stack, ERROR_STATUS_CONFIG_LOAD_GAME_DATA_MISSING,
        string_duplicate("cannot set player rack without lexicon"));
    return;
  }

  config_init_game(config);

  const int player_index = game_get_player_on_turn_index(config->game);

  Rack new_rack;
  rack_copy(&new_rack,
            player_get_rack(game_get_player(config->game, player_index)));

  const char *rack_str = config_get_parg_value(config, ARG_TOKEN_RACK, 0);
  load_rack_or_push_to_error_stack(rack_str, config->ld,
                                   ERROR_STATUS_CONFIG_LOAD_MALFORMED_RACK_ARG,
                                   &new_rack, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  if (rack_is_drawable(config->game, player_index, &new_rack)) {
    return_rack_to_bag(config->game, player_index);
    if (!draw_rack_from_bag(config->game, player_index, &new_rack)) {
      log_fatal(
          "unexpectedly failed to draw rack from bag in set rack command");
    }
  } else {
    error_stack_push(error_stack, ERROR_STATUS_CONFIG_LOAD_RACK_NOT_IN_BAG,
                     get_formatted_string(
                         "rack '%s' is not available in the bag", rack_str));
  }
}

// Move generation

void impl_move_gen(Config *config, ErrorStack *error_stack) {
  if (!config_has_game_data(config)) {
    error_stack_push(error_stack, ERROR_STATUS_CONFIG_LOAD_GAME_DATA_MISSING,
                     string_duplicate("cannot generate moves without lexicon"));
    return;
  }

  config_init_game(config);
  if (!config_get_use_small_plays(config)) {
    config_recreate_move_list(config, config_get_num_plays(config),
                              MOVE_LIST_TYPE_DEFAULT);
  } else {
    config_recreate_move_list(config, config_get_num_small_plays(config),
                              MOVE_LIST_TYPE_SMALL);
  }
  const MoveGenArgs args = {
      .game = config->game,
      .move_list = config->move_list,
      .thread_index = 0,
      .eq_margin_movegen = config->eq_margin_movegen,
  };
  generate_moves_for_game(&args);
  move_list_sort_moves(config->move_list);
}

// Inference

void config_fill_infer_args(const Config *config, bool use_game_history,
                            int target_index, Equity target_score,
                            int target_num_exch, Rack *target_played_tiles,
                            Rack *target_known_rack, Rack *nontarget_known_rack,
                            InferenceArgs *args) {
  args->target_index = target_index;
  args->target_score = target_score;
  args->target_num_exch = target_num_exch;
  args->move_capacity = config_get_num_leaves(config);
  args->equity_margin = config->eq_margin_inference;
  args->target_played_tiles = target_played_tiles;
  args->target_known_rack = target_known_rack;
  args->nontarget_known_rack = nontarget_known_rack;
  args->use_game_history = use_game_history;
  args->game_history = config->game_history;
  args->game = config_get_game(config);
  args->num_threads = config->num_threads;
  args->print_interval = config->print_interval;
  args->thread_control = config->thread_control;
  args->movegen_thread_offset = 0; // No offset for standalone inference
  args->skip_return_racks_to_bag = use_game_history; // Set to true if using game history
}

// Use target_index < 0 to infer using the game history
void config_infer(const Config *config, bool use_game_history, int target_index,
                  Equity target_score, int target_num_exch,
                  Rack *target_played_tiles, Rack *target_known_rack,
                  Rack *nontarget_known_rack, InferenceResults *results,
                  ErrorStack *error_stack) {
  InferenceArgs args;
  config_fill_infer_args(config, use_game_history, target_index, target_score,
                         target_num_exch, target_played_tiles,
                         target_known_rack, nontarget_known_rack, &args);
  return infer(&args, results, error_stack);
}

void impl_infer(Config *config, ErrorStack *error_stack) {
  if (!config_has_game_data(config)) {
    error_stack_push(error_stack, ERROR_STATUS_CONFIG_LOAD_GAME_DATA_MISSING,
                     string_duplicate("cannot infer without lexicon"));
    return;
  }

  config_init_game(config);
  const LetterDistribution *ld = game_get_ld(config->game);
  const int ld_size = ld_get_size(game_get_ld(config->game));
  Rack target_played_tiles;
  rack_set_dist_size_and_reset(&target_played_tiles, ld_size);
  Rack target_known_rack;
  rack_set_dist_size_and_reset(&target_known_rack, ld_size);
  Rack nontarget_known_rack;
  rack_set_dist_size_and_reset(&nontarget_known_rack, ld_size);

  if (config_get_parg_num_set_values(config, ARG_TOKEN_INFER) == 0) {
    config_infer(config, true, 0, 0, 0, &target_played_tiles,
                 &target_known_rack, &nontarget_known_rack,
                 config->inference_results, error_stack);
    return;
  }

  const char *target_name_or_index_str =
      config_get_parg_value(config, ARG_TOKEN_INFER, 0);
  int target_index;

  if (is_all_digits_or_empty(target_name_or_index_str)) {
    string_to_int_or_push_error("inferred player", target_name_or_index_str, 1,
                                2, ERROR_STATUS_CONFIG_LOAD_MALFORMED_INT_ARG,
                                &target_index, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
    // Convert from 1-indexed to 0-indexed
    target_index--;
  } else {
    target_index = -1;
    for (int i = 0; i < 2; i++) {
      const char *player_name =
          player_get_name(game_get_player(config->game, i));
      if (strings_iequal(player_name, target_name_or_index_str)) {
        target_index = i;
        break;
      }
    }
    if (target_index == -1) {
      error_stack_push(error_stack,
                       ERROR_STATUS_CONFIG_LOAD_MALFORMED_PLAYER_NAME,
                       get_formatted_string("unrecognized player name '%s'",
                                            target_name_or_index_str));
      return;
    }
  }

  const char *target_played_tiles_or_num_exch_str =
      config_get_parg_value(config, ARG_TOKEN_INFER, 1);

  int target_num_exch = 0;
  bool is_tile_placement_move = false;

  if (is_all_digits_or_empty(target_played_tiles_or_num_exch_str)) {
    string_to_int_or_push_error(
        "inferred player number exchanged", target_played_tiles_or_num_exch_str,
        0, RACK_SIZE, ERROR_STATUS_CONFIG_LOAD_MALFORMED_INT_ARG,
        &target_num_exch, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
  } else {
    load_rack_or_push_to_error_stack(
        target_played_tiles_or_num_exch_str, ld,
        ERROR_STATUS_CONFIG_LOAD_MALFORMED_RACK_ARG, &target_played_tiles,
        error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
    is_tile_placement_move = true;
  }

  Equity target_score = 0;
  int next_arg_index = 2;

  if (is_tile_placement_move) {
    const char *target_score_str =
        config_get_parg_value(config, ARG_TOKEN_INFER, 2);
    if (!target_score_str) {
      error_stack_push(error_stack, ERROR_STATUS_CONFIG_LOAD_MISSING_ARG,
                       string_duplicate("missing inferred player score"));
      return;
    }
    int target_score_int;
    string_to_int_or_push_error("inferred player score", target_score_str, 0,
                                INT_MAX,
                                ERROR_STATUS_CONFIG_LOAD_MALFORMED_INT_ARG,
                                &target_score_int, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
    target_score = int_to_equity(target_score_int);
    next_arg_index++;
  }

  const char *target_known_rack_str =
      config_get_parg_value(config, ARG_TOKEN_INFER, next_arg_index);
  if (target_known_rack_str) {
    load_rack_or_push_to_error_stack(
        target_known_rack_str, ld, ERROR_STATUS_CONFIG_LOAD_MALFORMED_RACK_ARG,
        &target_known_rack, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
    next_arg_index++;
    const char *nontarget_known_rack_str =
        config_get_parg_value(config, ARG_TOKEN_INFER, next_arg_index);
    if (nontarget_known_rack_str) {
      load_rack_or_push_to_error_stack(
          nontarget_known_rack_str, ld,
          ERROR_STATUS_CONFIG_LOAD_MALFORMED_RACK_ARG, &nontarget_known_rack,
          error_stack);
      if (!error_stack_is_empty(error_stack)) {
        return;
      }
    }
  }

  config_infer(config, false, target_index, target_score, target_num_exch,
               &target_played_tiles, &target_known_rack, &nontarget_known_rack,
               config->inference_results, error_stack);
}

// Sim

void config_fill_sim_args(const Config *config, Rack *known_opp_rack,
                          Rack *target_played_tiles,
                          Rack *nontarget_known_tiles,
                          Rack *target_known_inference_tiles,
                          SimArgs *sim_args) {
  sim_args->num_plies = config_get_plies(config);
  sim_args->move_list = config_get_move_list(config);
  sim_args->known_opp_rack = known_opp_rack;
  sim_args->win_pcts = config_get_win_pcts(config);
  sim_args->inference_results = config->inference_results;
  sim_args->thread_control = config->thread_control;
  sim_args->game = config_get_game(config);
  sim_args->move_list = config_get_move_list(config);
  sim_args->use_inference = config->sim_with_inference;
  sim_args->num_threads = config->num_threads;
  sim_args->movegen_thread_index = 0; // Standalone sim always uses base index 0
  sim_args->print_interval = config->print_interval;
  sim_args->seed = config->seed;
  if (sim_args->use_inference) {
    // FIXME: enable sim inferences using data from the last play instead of the
    // whole history so that autoplay does not have to keep a whole history
    // and play to turn for each inference which will probably incur more
    // overhead than we would like.
    config_fill_infer_args(config, true, 0, 0, 0, target_played_tiles,
                           target_known_inference_tiles, nontarget_known_tiles,
                           &sim_args->inference_args);
  }
  sim_args->bai_options.sample_limit = config_get_max_iterations(config);
  sim_args->bai_options.sample_minimum = config->min_play_iterations;
  const double percentile = config_get_stop_cond_pct(config);
  if (percentile > 100 || config->threshold == BAI_THRESHOLD_NONE) {
    sim_args->bai_options.threshold = BAI_THRESHOLD_NONE;
  } else {
    sim_args->bai_options.delta =
        1.0 - (config_get_stop_cond_pct(config) / 100.0);
    sim_args->bai_options.threshold = config->threshold;
  }
  sim_args->bai_options.time_limit_seconds =
      config_get_time_limit_seconds(config);
  sim_args->bai_options.sampling_rule = config->sampling_rule;
  sim_args->bai_options.num_threads = config->num_threads;
}

void config_simulate(const Config *config, Rack *known_opp_rack,
                     SimResults *sim_results, ErrorStack *error_stack) {
  SimArgs args;
  const int ld_size = ld_get_size(game_get_ld(config->game));
  Rack target_played_tiles;
  rack_set_dist_size_and_reset(&target_played_tiles, ld_size);
  Rack nontarget_known_tiles;
  rack_set_dist_size_and_reset(&nontarget_known_tiles, ld_size);
  // For inferences in a sim, this will always be empty.
  Rack target_known_inference_tiles;
  rack_set_dist_size_and_reset(&target_known_inference_tiles, ld_size);
  config_fill_sim_args(config, known_opp_rack, &target_played_tiles,
                       &nontarget_known_tiles, &target_known_inference_tiles,
                       &args);
  if (args.use_inference &&
      game_history_get_num_events(config->game_history) == 0) {
    error_stack_push(
        error_stack, ERROR_STATUS_SIM_GAME_HISTORY_MISSING,
        string_duplicate(
            "cannot sim with inference with an empty game history"));
    return;
  }
  return simulate(&args, sim_results, error_stack);
}

void impl_sim(Config *config, ErrorStack *error_stack) {
  if (!config_has_game_data(config)) {
    error_stack_push(error_stack, ERROR_STATUS_CONFIG_LOAD_GAME_DATA_MISSING,
                     string_duplicate("cannot simulate without lexicon"));
    return;
  }

  config_init_game(config);

  const int ld_size = ld_get_size(game_get_ld(config->game));
  const char *known_opp_rack_str =
      config_get_parg_value(config, ARG_TOKEN_SIM, 0);
  Rack known_opp_rack;
  rack_set_dist_size_and_reset(&known_opp_rack, ld_size);

  if (known_opp_rack_str) {
    load_rack_or_push_to_error_stack(
        known_opp_rack_str, game_get_ld(config->game),
        ERROR_STATUS_CONFIG_LOAD_MALFORMED_RACK_ARG, &known_opp_rack,
        error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
  }

  config_simulate(config, &known_opp_rack, config->sim_results, error_stack);
}

char *status_sim(Config *config) {
  SimResults *sim_results = config->sim_results;
  if (!sim_results) {
    return string_duplicate("simmer has not been initialized");
  }
  return ucgi_sim_stats(config->game, sim_results,
                        (double)sim_results_get_node_count(sim_results) /
                            bai_result_get_elapsed_seconds(
                                sim_results_get_bai_result(sim_results)),
                        true);
}

// Endgame

void config_fill_endgame_args(Config *config, EndgameArgs *endgame_args) {
  endgame_args->thread_control = config->thread_control;
  endgame_args->game = config->game;
  endgame_args->plies = config->endgame_plies;
  endgame_args->tt_fraction_of_mem = config->tt_fraction_of_mem;
  endgame_args->initial_small_move_arena_size =
      DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE;
}

void config_endgame(Config *config, EndgameResults *endgame_results,
                    ErrorStack *error_stack) {
  EndgameArgs endgame_args;
  config_fill_endgame_args(config, &endgame_args);
  endgame_solve(config->endgame_solver, &endgame_args, endgame_results,
                error_stack);
}

void impl_endgame(Config *config, ErrorStack *error_stack) {
  if (!config_has_game_data(config)) {
    error_stack_push(error_stack, ERROR_STATUS_CONFIG_LOAD_GAME_DATA_MISSING,
                     string_duplicate("cannot run endgame without lexicon"));
    return;
  }
  config_init_game(config);
  config_endgame(config, config->endgame_results, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
}

// Autoplay

void config_fill_autoplay_args(const Config *config,
                               AutoplayArgs *autoplay_args,
                               autoplay_t autoplay_type,
                               const char *num_games_or_min_rack_targets,
                               int games_before_force_draw_start) {
  autoplay_args->type = autoplay_type;
  autoplay_args->num_games_or_min_rack_targets = num_games_or_min_rack_targets;
  autoplay_args->games_before_force_draw_start = games_before_force_draw_start;
  autoplay_args->use_game_pairs = config_get_use_game_pairs(config);
  autoplay_args->human_readable = config_get_human_readable(config);
  autoplay_args->print_boards = config->print_boards;
  autoplay_args->num_threads = config->num_threads;
  autoplay_args->print_interval = config->print_interval;
  autoplay_args->seed = config->seed;
  autoplay_args->win_pcts = config_get_win_pcts(config);
  autoplay_args->num_leaves = config_get_num_leaves(config);
  autoplay_args->multi_threaded_sims = false; // Default to concurrent games mode
  ErrorStack *error_stack = error_stack_create();
  config_load_bool(config, ARG_TOKEN_MULTI_THREADED_SIMS,
                   &autoplay_args->multi_threaded_sims, error_stack);
  error_stack_destroy(error_stack);
  autoplay_args->thread_control = config_get_thread_control(config);
  autoplay_args->data_paths = config_get_data_paths(config);
  autoplay_args->game_string_options = config->game_string_options;
  autoplay_args->inference_results = config->inference_results;
  autoplay_args->equity_margin = config->eq_margin_inference;
  config_fill_game_args(config, autoplay_args->game_args);
}

void config_autoplay(const Config *config, AutoplayResults *autoplay_results,
                     autoplay_t autoplay_type,
                     const char *num_games_or_min_rack_targets,
                     int games_before_force_draw_start,
                     ErrorStack *error_stack) {
  AutoplayArgs args;
  GameArgs game_args;
  args.game_args = &game_args;
  config_fill_autoplay_args(config, &args, autoplay_type,
                            num_games_or_min_rack_targets,
                            games_before_force_draw_start);
  autoplay(&args, autoplay_results, error_stack);
}

void impl_autoplay(Config *config, ErrorStack *error_stack) {
  if (!config_has_game_data(config)) {
    error_stack_push(error_stack, ERROR_STATUS_CONFIG_LOAD_GAME_DATA_MISSING,
                     string_duplicate("cannot autoplay without lexicon"));
    return;
  }

  autoplay_results_set_options(
      config->autoplay_results,
      config_get_parg_value(config, ARG_TOKEN_AUTOPLAY, 0), error_stack);

  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  const char *num_games_str =
      config_get_parg_value(config, ARG_TOKEN_AUTOPLAY, 1);

  config_autoplay(config, config->autoplay_results, AUTOPLAY_TYPE_DEFAULT,
                  num_games_str, 0, error_stack);
}

// Conversion

void config_fill_conversion_args(const Config *config, ConversionArgs *args) {
  args->conversion_type_string =
      config_get_parg_value(config, ARG_TOKEN_CONVERT, 0);
  args->data_paths = config_get_data_paths(config);
  args->input_and_output_name =
      config_get_parg_value(config, ARG_TOKEN_CONVERT, 1);
  args->ld_name = config_get_parg_value(config, ARG_TOKEN_CONVERT, 2);
}

void config_convert(const Config *config, ConversionResults *results,
                    ErrorStack *error_stack) {
  ConversionArgs args;
  config_fill_conversion_args(config, &args);
  convert(&args, results, error_stack);
}

void impl_convert(Config *config, ErrorStack *error_stack) {
  config_convert(config, config->conversion_results, error_stack);
}

// Leave Gen

void impl_leave_gen(Config *config, ErrorStack *error_stack) {
  if (!config_has_game_data(config)) {
    error_stack_push(
        error_stack, ERROR_STATUS_CONFIG_LOAD_GAME_DATA_MISSING,
        string_duplicate("cannot generate leaves without lexicon"));
    return;
  }

  if (!players_data_get_is_shared(config->players_data,
                                  PLAYERS_DATA_TYPE_KWG) ||
      !players_data_get_is_shared(config->players_data,
                                  PLAYERS_DATA_TYPE_KLV)) {
    error_stack_push(
        error_stack, ERROR_STATUS_LEAVE_GEN_DIFFERENT_LEXICA_OR_LEAVES,
        string_duplicate("cannot generate leaves with different lexica or "
                         "different leaves for the players"));
    return;
  }

  autoplay_results_set_options(config->autoplay_results, "games", error_stack);

  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  const char *min_rack_targets_str =
      config_get_parg_value(config, ARG_TOKEN_LEAVE_GEN, 0);

  const char *games_before_force_draw_start_str =
      config_get_parg_value(config, ARG_TOKEN_LEAVE_GEN, 1);
  int games_before_force_draw_start;
  string_to_int_or_push_error("games before force draw start",
                              games_before_force_draw_start_str, 0, INT_MAX,
                              ERROR_STATUS_CONFIG_LOAD_INT_ARG_OUT_OF_BOUNDS,
                              &games_before_force_draw_start, error_stack);

  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  config_autoplay(config, config->autoplay_results, AUTOPLAY_TYPE_LEAVE_GEN,
                  min_rack_targets_str, games_before_force_draw_start,
                  error_stack);
}

// Create

// This only implements creating a klv for now.
void impl_create_data(const Config *config, ErrorStack *error_stack) {
  const char *create_type_str =
      config_get_parg_value(config, ARG_TOKEN_CREATE_DATA, 0);

  if (has_iprefix(create_type_str, "klv")) {
    const char *klv_name_str =
        config_get_parg_value(config, ARG_TOKEN_CREATE_DATA, 1);
    const char *ld_name_arg =
        config_get_parg_value(config, ARG_TOKEN_CREATE_DATA, 2);
    LetterDistribution *ld = config_get_ld(config);
    if (ld_name_arg) {
      ld = ld_create(config_get_data_paths(config), ld_name_arg, error_stack);
      if (!error_stack_is_empty(error_stack)) {
        return;
      }
    }
    KLV *klv = klv_create_empty(ld, klv_name_str);
    klv_write(klv, config_get_data_paths(config), klv_name_str, error_stack);
    klv_destroy(klv);
    if (ld_name_arg) {
      ld_destroy(ld);
    }
  } else {
    error_stack_push(
        error_stack, ERROR_STATUS_CONFIG_LOAD_UNRECOGNIZED_CREATE_DATA_TYPE,
        get_formatted_string("data creation not supported for type %s",
                             create_type_str));
  }
}

// Show game

char *impl_show(Config *config, ErrorStack *error_stack) {
  if (!config_has_game_data(config)) {
    error_stack_push(error_stack, ERROR_STATUS_CONFIG_LOAD_GAME_DATA_MISSING,
                     string_duplicate("cannot show game without lexicon"));
    return empty_string();
  }

  config_init_game(config);

  // Create string builder to hold the game display
  StringBuilder *game_string = string_builder_create();

  // Add the game to the string builder
  string_builder_add_game(config->game, NULL, config->game_string_options,
                          game_string);

  // Get the string and destroy the builder
  char *result = string_builder_dump(game_string, NULL);
  string_builder_destroy(game_string);

  return result;
}

void execute_show(Config *config, ErrorStack *error_stack) {
  char *result = impl_show(config, error_stack);
  if (error_stack_is_empty(error_stack)) {
    thread_control_print(config->thread_control, result);
  }
  free(result);
}

char *str_api_show(Config *config, ErrorStack *error_stack) {
  return impl_show(config, error_stack);
}

// Start new game

void update_game_history_with_config(Config *config) {
  if (!config->ld) {
    return;
  }
  const char *ld_name = ld_get_name(config->ld);
  if (ld_name) {
    game_history_set_ld_name(config->game_history, ld_name);
  }
  const char *existing_p1_lexicon_name = players_data_get_data_name(
      config->players_data, PLAYERS_DATA_TYPE_KWG, 0);
  if (existing_p1_lexicon_name) {
    game_history_set_lexicon_name(config->game_history,
                                  existing_p1_lexicon_name);
  }
  game_history_set_board_layout_name(
      config->game_history, board_layout_get_name(config->board_layout));
  game_history_set_game_variant(config->game_history, config->game_variant);

  for (int player_index = 0; player_index < 2; player_index++) {
    // This will have been specified by the CLI and will not have any whitespace
    const char *player_name =
        players_data_get_name(config->players_data, player_index);
    game_history_player_reset(config->game_history, player_index, player_name,
                              player_name);
  }
}

char *impl_new_game(Config *config, ErrorStack *error_stack) {
  if (!config_has_game_data(config)) {
    error_stack_push(
        error_stack, ERROR_STATUS_CONFIG_LOAD_GAME_DATA_MISSING,
        string_duplicate("cannot create new game without lexicon"));
    return empty_string();
  }
  config_init_game(config);
  game_reset(config->game);
  game_history_reset(config->game_history);
  update_game_history_with_config(config);
  for (int player_index = 0; player_index < 2; player_index++) {
    const char *player_name =
        config_get_parg_value(config, ARG_TOKEN_NEW_GAME, player_index);
    // Since the player_name is specified by the CLI which guarantees
    // that it does not contain whitespace, which means we can use it as
    // the nickname as well as the name.
    game_history_player_reset(config->game_history, player_index, player_name,
                              player_name);
  }
  return empty_string();
}

void execute_new_game(Config *config, ErrorStack *error_stack) {
  char *result = impl_new_game(config, error_stack);
  if (error_stack_is_empty(error_stack)) {
    thread_control_print(config->thread_control, result);
    execute_show(config, error_stack);
  }
  free(result);
}

char *str_api_new_game(Config *config, ErrorStack *error_stack) {
  return impl_new_game(config, error_stack);
}

// Export GCG

char *impl_export(Config *config, ErrorStack *error_stack) {
  if (!config_has_game_data(config)) {
    error_stack_push(error_stack, ERROR_STATUS_CONFIG_LOAD_GAME_DATA_MISSING,
                     string_duplicate("cannot export game without lexicon"));
    return empty_string();
  }
  if (game_history_get_num_events(config->game_history) == 0) {
    error_stack_push(error_stack, ERROR_STATUS_EXPORT_NO_GAME_EVENTS,
                     string_duplicate("cannot export an empty game history"));
    return empty_string();
  }
  config_init_game(config);

  game_history_set_gcg_filename(
      config->game_history, config_get_parg_value(config, ARG_TOKEN_EXPORT, 0));

  write_gcg(game_history_get_gcg_filename(config->game_history), config->ld,
            config->game_history, error_stack);

  if (!error_stack_is_empty(error_stack)) {
    return empty_string();
  }

  return get_formatted_string(
      "Saved GCG file to %s\n",
      game_history_get_gcg_filename(config->game_history));
}

void execute_export(Config *config, ErrorStack *error_stack) {
  char *result = impl_export(config, error_stack);
  if (error_stack_is_empty(error_stack)) {
    thread_control_print(config->thread_control, result);
  }
  free(result);
}

char *str_api_export(Config *config, ErrorStack *error_stack) {
  return impl_export(config, error_stack);
}

// Commit move

void config_backup_game_and_history(Config *config) {
  game_destroy(config->game_backup);
  config->game_backup = game_duplicate(config->game);
  game_history_destroy(config->game_history_backup);
  config->game_history_backup = game_history_duplicate(config->game_history);
}

void config_restore_game_and_history(Config *config) {
  game_destroy(config->game);
  config->game = config->game_backup;
  config->game_backup = NULL;
  game_history_destroy(config->game_history);
  config->game_history = config->game_history_backup;
  config->game_history_backup = NULL;
}

// Runs game_play_n_events and then calculates if
// - the consecutive pass game end procedures should be applied
// - the game history needs to wait for the final pass/challenge from the user
void config_game_play_events(Config *config, ErrorStack *error_stack) {
  Game *game = config->game;
  GameHistory *game_history = config->game_history;
  game_play_n_events(game_history, game,
                     game_history_get_num_played_events(game_history), true,
                     error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
  const int num_events = game_history_get_num_events(game_history);
  const int num_played_events =
      game_history_get_num_played_events(game_history);

  const int player_on_turn_index = game_get_player_on_turn_index(game);
  const Rack *player_off_turn_rack =
      player_get_rack(game_get_player(game, 1 - player_on_turn_index));
  game_history_set_waiting_for_final_pass_or_challenge(
      game_history,
      game_get_game_end_reason(game) == GAME_END_REASON_STANDARD &&
          game_event_get_type(
              game_history_get_event(game_history, num_played_events - 1)) ==
              GAME_EVENT_TILE_PLACEMENT_MOVE &&
          rack_is_empty(player_off_turn_rack) &&
          bag_is_empty(game_get_bag(game)));

  if (num_played_events < num_events ||
      !game_reached_max_scoreless_turns(game) ||
      game_history_contains_end_rack_penalty_event(game_history)) {
    // We have either:
    //
    //  - played into the "middle" of the game history with more events
    //    that have yet to be played, which are either more moves or other end
    //    of game events
    //
    //  - the game has not reached the maximum number of scoreless turns
    //
    //  - the game already has end rack penalty events
    //
    // so there is no need to create the end rack penalty events
    return;
  }

  // Add the consecutive pass rack end penalties for both players
  int player_index = game_get_player_on_turn_index(game);
  const LetterDistribution *ld = game_get_ld(game);
  for (int i = 0; i < 2; i++) {
    if (i == 1) {
      player_index = 1 - player_index;
    }
    const Player *player = game_get_player(game, player_index);
    const Rack *player_rack = player_get_rack(player);
    const Rack *rack_to_draw_before_pass_out_game_end =
        game_history_player_get_rack_to_draw_before_pass_out_game_end(
            game_history, player_index);
    if (rack_get_dist_size(rack_to_draw_before_pass_out_game_end) != 0) {
      if (!rack_is_drawable(game, player_index,
                            rack_to_draw_before_pass_out_game_end)) {
        StringBuilder *sb = string_builder_create();
        string_builder_add_rack(sb, rack_to_draw_before_pass_out_game_end, ld,
                                false);
        error_stack_push(
            error_stack, ERROR_STATUS_COMMIT_PASS_OUT_RACK_NOT_IN_BAG,
            get_formatted_string(
                "rack to draw before game end pass out '%s' for player '%s'"
                "is not available in the bag",
                string_builder_peek(sb),
                game_history_player_get_name(game_history, player_index)));
        string_builder_destroy(sb);
        return;
      }
      return_rack_to_bag(game, player_index);
      draw_rack_from_bag(game, player_index,
                         rack_to_draw_before_pass_out_game_end);
    } else {
      // Get the rack from the previous pass
      bool found_pass = false;
      for (int j = num_events - 1; j >= 0; j--) {
        GameEvent *game_event = game_history_get_event(game_history, j);
        if (game_event_get_type(game_event) == GAME_EVENT_PASS &&
            game_event_get_player_index(game_event) == player_index) {
          const Rack *prev_pass_rack = game_event_get_rack(game_event);
          if (!rack_is_drawable(game, player_index, prev_pass_rack)) {
            StringBuilder *sb = string_builder_create();
            string_builder_add_rack(sb, prev_pass_rack, ld, false);
            error_stack_push(
                error_stack, ERROR_STATUS_COMMIT_PASS_OUT_RACK_NOT_IN_BAG,
                get_formatted_string(
                    "rack to draw before game end pass out '%s' for player '%s'"
                    "is not available in the bag",
                    string_builder_peek(sb),
                    game_history_player_get_name(game_history, player_index)));
            string_builder_destroy(sb);
            return;
          }
          return_rack_to_bag(game, player_index);
          draw_rack_from_bag(game, player_index, prev_pass_rack);
          found_pass = true;
          break;
        }
      }
      if (!found_pass) {
        error_stack_push(
            error_stack, ERROR_STATUS_COMMIT_PREVIOUS_PASS_NOT_FOUND,
            get_formatted_string(
                "did not find expected previous pass for player '%s' when "
                "processing consecutive pass game end penalty",
                game_history_player_get_name(game_history, player_index)));
      }
    }

    draw_to_full_rack(game, player_index);

    const Equity end_rack_penalty = calculate_end_rack_penalty(player_rack, ld);
    GameEvent *rack_penalty_event =
        game_history_add_game_event(game_history, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
    game_event_set_player_index(rack_penalty_event, player_index);
    game_event_set_type(rack_penalty_event, GAME_EVENT_END_RACK_PENALTY);
    rack_copy(game_event_get_rack(rack_penalty_event), player_rack);
    game_event_set_score_adjustment(rack_penalty_event, end_rack_penalty);
    game_event_set_cumulative_score(
        rack_penalty_event, player_get_score(player) + end_rack_penalty);
    game_event_set_cgp_move_string(rack_penalty_event, NULL);
    game_event_set_move_score(rack_penalty_event, 0);
    game_history_next(game_history, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
  }
  // Replay the game with the added end rack penalties
  game_play_n_events(game_history, game,
                     // Play to the end
                     game_history_get_num_events(game_history), true,
                     error_stack);
}

void config_add_end_rack_points(Config *config, const int player_index,
                                ErrorStack *error_stack) {
  game_history_truncate_to_played_events(config->game_history);

  GameEvent *game_event =
      game_history_add_game_event(config->game_history, error_stack);

  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  const Rack *end_rack_points_rack =
      player_get_rack(game_get_player(config->game, player_index));
  const Equity end_rack_points_equity =
      calculate_end_rack_points(end_rack_points_rack, config->ld);
  const int end_rack_points_player_index = 1 - player_index;

  // Find previous game event for player
  const int num_events = game_history_get_num_events(config->game_history);
  // Start at num_events - 2 to avoid the game event that was just added
  int cumulative_score = 0;
  bool found_previous_game_event = false;
  for (int i = num_events - 2; i >= 0; i--) {
    const GameEvent *gei = game_history_get_event(config->game_history, i);
    if (game_event_get_player_index(gei) == end_rack_points_player_index) {
      cumulative_score = game_event_get_cumulative_score(gei);
      found_previous_game_event = true;
      break;
    }
  }
  if (!found_previous_game_event) {
    log_fatal(
        "failed to find previous game event when adding end rack points\n");
  }

  game_event_set_player_index(game_event, end_rack_points_player_index);
  game_event_set_type(game_event, GAME_EVENT_END_RACK_POINTS);
  game_event_set_cgp_move_string(game_event, NULL);
  game_event_set_score_adjustment(game_event, end_rack_points_equity);
  game_event_set_cumulative_score(game_event,
                                  cumulative_score + end_rack_points_equity);
  game_event_set_move_score(game_event, 0);
  rack_copy(game_event_get_rack(game_event), end_rack_points_rack);

  game_history_next(config->game_history, error_stack);
}

void config_add_game_event(Config *config, const int player_index,
                           game_event_t game_event_type, const Move *move,
                           const char *ucgi_move_string,
                           const Rack *player_rack,
                           const Equity score_adjustment,
                           ErrorStack *error_stack) {
  game_history_truncate_to_played_events(config->game_history);
  const bool waiting_for_final_pass_or_challenge =
      game_history_get_waiting_for_final_pass_or_challenge(
          config->game_history);
  if (waiting_for_final_pass_or_challenge &&
      (game_event_type != GAME_EVENT_PASS &&
       game_event_type != GAME_EVENT_CHALLENGE_BONUS)) {
    error_stack_push(error_stack,
                     ERROR_STATUS_COMMIT_WAITING_FOR_PASS_OR_CHALLENGE_BONUS,
                     string_duplicate("waiting for final pass or challenge "
                                      "bonus but got a different game event"));
    return;
  }

  Equity cumulative_score = EQUITY_INITIAL_VALUE;
  Equity move_score = EQUITY_INITIAL_VALUE;
  Rack game_event_rack;
  rack_set_dist_size_and_reset(&game_event_rack, ld_get_size(config->ld));
  bool add_event_to_history = true;
  bool add_rack_end_points = false;
  switch (game_event_type) {
  case GAME_EVENT_TILE_PLACEMENT_MOVE:
  case GAME_EVENT_EXCHANGE:
    rack_copy(&game_event_rack, player_rack);
    cumulative_score =
        player_get_score(game_get_player(config->game, player_index)) +
        move_get_score(move);
    move_score = move_get_score(move);
    break;
  case GAME_EVENT_PASS:
    if (waiting_for_final_pass_or_challenge) {
      // End the game by adding an end rack points event
      add_rack_end_points = true;
      add_event_to_history = false;
      cumulative_score =
          player_get_score(game_get_player(config->game, player_index));
    } else {
      // Add an actual pass
      rack_copy(&game_event_rack, player_rack);
      cumulative_score =
          player_get_score(game_get_player(config->game, player_index)) +
          move_get_score(move);
      move_score = move_get_score(move);
    }
    break;
  case GAME_EVENT_PHONY_TILES_RETURNED:;
    const GameEvent *previous_game_event = game_history_get_event(
        config->game_history,
        game_history_get_num_played_events(config->game_history) - 1);
    rack_copy(&game_event_rack, game_event_get_const_rack(previous_game_event));
    cumulative_score =
        player_get_score(game_get_player(config->game, player_index)) -
        game_event_get_move_score(previous_game_event);
    move_score = 0;
    break;
  case GAME_EVENT_CHALLENGE_BONUS:
    // Challenge bonus events are handled elsewhere by inserting, not appending
    // to the history
    log_fatal("got unexpected challenge bonus game event when adding "
              "event to game history in the config\n");
    break;
  case GAME_EVENT_TIME_PENALTY:
    // Time penalty events are never submitted directly by the user and
    // should have been handled automatically
    log_fatal("got unexpected time penalty game event when adding "
              "event to game history in the config\n");
    break;
  case GAME_EVENT_END_RACK_POINTS:
    // End rack points events are never submitted directly by the user and are
    // handled elsewhere
    log_fatal("got unexpected end rack points game event when adding "
              "event to game history in the config\n");
    break;
  case GAME_EVENT_END_RACK_PENALTY:
    // End rack penalty events are never submitted directly by the user and are
    // handled elsewhere
    log_fatal("got unexpected end rack penalty game event when adding "
              "event to game history in the config\n");
    break;
  case GAME_EVENT_UNKNOWN:
    log_fatal("got unknown game event type when adding event to game history "
              "in the config\n");
    break;
  }

  if (add_event_to_history) {
    GameEvent *game_event =
        game_history_add_game_event(config->game_history, error_stack);

    if (!error_stack_is_empty(error_stack)) {
      return;
    }

    game_event_set_player_index(game_event, player_index);
    game_event_set_type(game_event, game_event_type);
    game_event_set_cgp_move_string(
        game_event, string_duplicate_allow_null(ucgi_move_string));
    game_event_set_score_adjustment(game_event, score_adjustment);
    game_event_set_cumulative_score(game_event, cumulative_score);
    game_event_set_move_score(game_event, move_score);
    rack_copy(game_event_get_rack(game_event), &game_event_rack);

    game_history_next(config->game_history, error_stack);

    if (!error_stack_is_empty(error_stack)) {
      return;
    }
  }

  if (add_rack_end_points) {
    config_add_end_rack_points(config, player_index, error_stack);
  }

  config_game_play_events(config, error_stack);

  if (!error_stack_is_empty(error_stack)) {
    return;
  }
}

void parse_commit(Config *config, StringBuilder *move_string_builder,
                  ValidatedMoves **vms, ErrorStack *error_stack) {
  const char *commit_pos_arg_1 =
      config_get_parg_value(config, ARG_TOKEN_COMMIT, 0);
  const char *commit_pos_arg_2 =
      config_get_parg_value(config, ARG_TOKEN_COMMIT, 1);
  const char *commit_pos_arg_3 =
      config_get_parg_value(config, ARG_TOKEN_COMMIT, 2);
  Move move;
  const int player_on_turn_index = game_get_player_on_turn_index(config->game);
  const Rack *player_rack =
      player_get_rack(game_get_player(config->game, player_on_turn_index));
  game_event_t game_event_type = GAME_EVENT_UNKNOWN;
  const char *rack_to_draw_before_pass_out_game_end_str = NULL;
  if (strings_iequal(commit_pos_arg_1, UCGI_PASS_MOVE)) {
    // Commit pass
    if (commit_pos_arg_2) {
      error_stack_push(
          error_stack, ERROR_STATUS_COMMIT_EXTRANEOUS_ARG,
          get_formatted_string(
              "extraneous argument '%s' provided when committing pass move",
              commit_pos_arg_3));
      return;
    }
    move_set_as_pass(&move);
    string_builder_add_string(move_string_builder, UCGI_PASS_MOVE);
    game_event_type = GAME_EVENT_PASS;
  } else if (is_all_digits_or_empty(commit_pos_arg_1)) {
    // Commit move by index
    if (commit_pos_arg_3) {
      error_stack_push(
          error_stack, ERROR_STATUS_COMMIT_EXTRANEOUS_ARG,
          get_formatted_string(
              "extraneous argument '%s' provided when committing move by index",
              commit_pos_arg_3));
      return;
    }
    int move_list_count = 0;
    if (config->move_list) {
      move_list_count = move_list_get_count(config->move_list);
    }
    if (move_list_count == 0) {
      error_stack_push(
          error_stack, ERROR_STATUS_COMMIT_MOVE_INDEX_OUT_OF_RANGE,
          get_formatted_string(
              "cannot commit move with index '%s' with no generated moves",
              commit_pos_arg_1));
      return;
    }
    // If no second arg is provided and the first arg is all digits, the digits
    // represent the rank of the move to commit.

    int commit_move_index;
    string_to_int_or_push_error("move index", commit_pos_arg_1, 1,
                                move_list_count,
                                ERROR_STATUS_COMMIT_MOVE_INDEX_OUT_OF_RANGE,
                                &commit_move_index, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
    // Convert from 1-indexed user input to 0-indexed internal representation
    commit_move_index--;
    move_copy(&move, move_list_get_move(config->move_list, commit_move_index));
    if (move_get_type(&move) != GAME_EVENT_EXCHANGE && commit_pos_arg_2) {
      error_stack_push(
          error_stack, ERROR_STATUS_COMMIT_EXTRANEOUS_ARG,
          get_formatted_string("extraneous argument '%s' provided when "
                               "committing non-exchange move by index",
                               commit_pos_arg_2));
      return;
    }
    string_builder_add_ucgi_move(move_string_builder, &move,
                                 game_get_board(config->game), config->ld);
    game_event_type = move_get_type(&move);
    rack_to_draw_before_pass_out_game_end_str = commit_pos_arg_2;
  } else {
    // Commit UCGI move
    if (!commit_pos_arg_2) {
      error_stack_push(
          error_stack, ERROR_STATUS_COMMIT_MISSING_EXCHANGE_OR_PLAY,
          get_formatted_string("missing exchange or play for commit '%s'",
                               commit_pos_arg_1));
      return;
    }
    string_builder_add_formatted_string(move_string_builder, "%s%c%s%c",
                                        commit_pos_arg_1, UCGI_DELIMITER,
                                        commit_pos_arg_2, UCGI_DELIMITER);
    string_builder_add_rack(move_string_builder, player_rack, config->ld,
                            false);
    *vms = validated_moves_create(config->game, player_on_turn_index,
                                  string_builder_peek(move_string_builder),
                                  true, false, true, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
    move_copy(&move, validated_moves_get_move(*vms, 0));
    if (move_get_type(&move) != GAME_EVENT_EXCHANGE && commit_pos_arg_3) {
      error_stack_push(
          error_stack, ERROR_STATUS_COMMIT_EXTRANEOUS_ARG,
          get_formatted_string("extraneous argument '%s' provided when "
                               "committing non-exchange move by index",
                               commit_pos_arg_3));
      return;
    }
    game_event_type = move_get_type(&move);
    rack_to_draw_before_pass_out_game_end_str = commit_pos_arg_3;
  }

  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  Rack *rack_to_draw_before_pass_out_game_end =
      game_history_player_get_rack_to_draw_before_pass_out_game_end(
          config->game_history, player_on_turn_index);
  if (rack_to_draw_before_pass_out_game_end_str) {
    int num_mls =
        rack_set_to_string(config->ld, rack_to_draw_before_pass_out_game_end,
                           rack_to_draw_before_pass_out_game_end_str);
    if (num_mls < 0) {
      error_stack_push(
          error_stack, ERROR_STATUS_COMMIT_INVALID_PASS_OUT_RACK,
          string_duplicate("invalid rack '%s' for consecutive pass game end"));
      return;
    }
    if (num_mls > RACK_SIZE) {
      error_stack_push(
          error_stack, ERROR_STATUS_COMMIT_INVALID_PASS_OUT_RACK,
          get_formatted_string("rack '%s' for consecutive pass game end has %d "
                               "tiles which exceeds the "
                               "maximum rack size of %d",
                               rack_to_draw_before_pass_out_game_end_str,
                               num_mls, RACK_SIZE));
      return;
    }
  } else {
    memset(rack_to_draw_before_pass_out_game_end, 0, sizeof(Rack));
  }

  config_add_game_event(config, player_on_turn_index, game_event_type, &move,
                        string_builder_peek(move_string_builder), player_rack,
                        0, error_stack);
}

char *impl_commit(Config *config, ErrorStack *error_stack) {
  if (!config_has_game_data(config)) {
    error_stack_push(error_stack, ERROR_STATUS_CONFIG_LOAD_GAME_DATA_MISSING,
                     string_duplicate("cannot commit a move without lexicon"));
    return empty_string();
  }

  config_init_game(config);

  StringBuilder *move_string_builder = string_builder_create();
  ValidatedMoves *vms = NULL;

  config_backup_game_and_history(config);

  parse_commit(config, move_string_builder, &vms, error_stack);

  string_builder_destroy(move_string_builder);
  validated_moves_destroy(vms);

  if (!error_stack_is_empty(error_stack)) {
    config_restore_game_and_history(config);
    return empty_string();
  }

  // FIXME: mark all results as outdated here
  move_list_reset(config->move_list);

  return empty_string();
}

void execute_commit(Config *config, ErrorStack *error_stack) {
  char *result = impl_commit(config, error_stack);
  if (error_stack_is_empty(error_stack)) {
    thread_control_print(config->thread_control, result);
    execute_show(config, error_stack);
  }
  free(result);
}

char *str_api_commit(Config *config, ErrorStack *error_stack) {
  return impl_commit(config, error_stack);
}

// Challenge

char *impl_challenge(Config *config, ErrorStack *error_stack) {
  if (!config_has_game_data(config)) {
    error_stack_push(error_stack, ERROR_STATUS_CONFIG_LOAD_GAME_DATA_MISSING,
                     string_duplicate("cannot challenge without lexicon"));
    return empty_string();
  }

  config_init_game(config);

  const int num_events = game_history_get_num_events(config->game_history);
  const int num_played_events =
      game_history_get_num_played_events(config->game_history);
  if (num_events == 0 || num_played_events == 0) {
    error_stack_push(error_stack, ERROR_STATUS_CHALLENGE_NO_PREVIOUS_MOVE,
                     string_duplicate("no moves to challenge"));
    return empty_string();
  }
  const GameEvent *prev_played_event =
      game_history_get_event(config->game_history, num_played_events - 1);
  if (game_event_get_type(prev_played_event) == GAME_EVENT_CHALLENGE_BONUS) {
    error_stack_push(
        error_stack, ERROR_STATUS_CHALLENGE_ALREADY_CHALLENGED,
        string_duplicate("the previous play already has a challenge bonus, use "
                         "the 'unchallenge' command to remove the challenge so "
                         "a new one can be applied"));
    return empty_string();
  }
  if (game_event_get_type(prev_played_event) !=
      GAME_EVENT_TILE_PLACEMENT_MOVE) {
    error_stack_push(
        error_stack, ERROR_STATUS_CHALLENGE_NO_PREVIOUS_TILE_PLACEMENT_MOVE,
        string_duplicate(
            "cannot challenge without a previous tile placement move"));
    return empty_string();
  }

  const ValidatedMoves *vms = game_event_get_vms(prev_played_event);
  const int player_on_turn_index = game_get_player_on_turn_index(config->game);
  if (validated_moves_is_phony(vms, 0)) {
    config_backup_game_and_history(config);
    config_add_game_event(config, 1 - player_on_turn_index,
                          GAME_EVENT_PHONY_TILES_RETURNED, NULL, NULL, NULL, 0,
                          error_stack);
    if (!error_stack_is_empty(error_stack)) {
      config_restore_game_and_history(config);
      return empty_string();
    }
  } else {
    int challenge_bonus_int = config->challenge_bonus;
    const char *challenge_bonus_str =
        config_get_parg_value(config, ARG_TOKEN_CHALLENGE, 0);
    if (challenge_bonus_str) {
      string_to_int_or_push_error(
          "challenge bonus", challenge_bonus_str, 0, INT_MAX,
          ERROR_STATUS_CGP_PARSE_MALFORMED_CGP_OPCODE_CHALLENGE_BONUS,
          &challenge_bonus_int, error_stack);
      if (!error_stack_is_empty(error_stack)) {
        return empty_string();
      }
    }

    config_backup_game_and_history(config);

    game_history_insert_challenge_bonus_game_event(
        config->game_history, 1 - player_on_turn_index,
        int_to_equity(challenge_bonus_int), error_stack);

    if (error_stack_is_empty(error_stack)) {
      game_history_next(config->game_history, error_stack);
      if (error_stack_is_empty(error_stack)) {
        if (game_history_get_waiting_for_final_pass_or_challenge(
                config->game_history)) {
          config_add_end_rack_points(config, player_on_turn_index, error_stack);
        }
        config_game_play_events(config, error_stack);
      }
    }

    if (!error_stack_is_empty(error_stack)) {
      config_restore_game_and_history(config);
      return empty_string();
    }
  }

  return empty_string();
}

void execute_challenge(Config *config, ErrorStack *error_stack) {
  char *result = impl_challenge(config, error_stack);
  if (error_stack_is_empty(error_stack)) {
    thread_control_print(config->thread_control, result);
    execute_show(config, error_stack);
  }
  free(result);
}

char *str_api_challenge(Config *config, ErrorStack *error_stack) {
  return impl_challenge(config, error_stack);
}

// Unchallenge (removes challenge bonus)

char *impl_unchallenge(Config *config, ErrorStack *error_stack) {
  if (!config_has_game_data(config)) {
    error_stack_push(error_stack, ERROR_STATUS_CONFIG_LOAD_GAME_DATA_MISSING,
                     string_duplicate("cannot remove challenge bonus without "
                                      "letter distribution and lexicon"));
    return empty_string();
  }

  config_init_game(config);

  const int num_events = game_history_get_num_events(config->game_history);
  const int num_played_events =
      game_history_get_num_played_events(config->game_history);

  if (num_events == 0 || num_played_events == 0) {
    error_stack_push(error_stack, ERROR_STATUS_CHALLENGE_NO_PREVIOUS_MOVE,
                     string_duplicate("no moves to challenge"));
    return empty_string();
  }
  const GameEvent *current_event =
      game_history_get_event(config->game_history, num_played_events - 1);
  if (game_event_get_type(current_event) == GAME_EVENT_PHONY_TILES_RETURNED) {
    error_stack_push(
        error_stack,
        ERROR_STATUS_CHALLENGE_CANNOT_UNCHALLENGE_PHONY_TILES_RETURNED,
        string_duplicate("only challenge bonuses can be removed, to remove a "
                         "successful challenge of a phony play, commit another "
                         "move after the phony play"));
    return empty_string();
  }
  if (game_event_get_type(current_event) != GAME_EVENT_CHALLENGE_BONUS) {
    error_stack_push(
        error_stack, ERROR_STATUS_CHALLENGE_NO_PREVIOUS_CHALLENGE_BONUS,
        string_duplicate("the current event is not a challenge bonus"));
    return empty_string();
  }

  config_backup_game_and_history(config);

  game_history_remove_challenge_bonus_game_event(config->game_history);

  game_history_previous(config->game_history, error_stack);

  if (error_stack_is_empty(error_stack)) {
    config_game_play_events(config, error_stack);
  }

  if (!error_stack_is_empty(error_stack)) {
    config_restore_game_and_history(config);
    return empty_string();
  }

  return empty_string();
}

void execute_unchallenge(Config *config, ErrorStack *error_stack) {
  char *result = impl_unchallenge(config, error_stack);
  if (error_stack_is_empty(error_stack)) {
    thread_control_print(config->thread_control, result);
    execute_show(config, error_stack);
  }
  free(result);
}

char *str_api_unchallenge(Config *config, ErrorStack *error_stack) {
  return impl_unchallenge(config, error_stack);
}

// Overtime penalty command

char *impl_overtime(Config *config, ErrorStack *error_stack) {
  if (!config_has_game_data(config)) {
    error_stack_push(
        error_stack, ERROR_STATUS_CONFIG_LOAD_GAME_DATA_MISSING,
        string_duplicate("cannot add overtime penalty without lexicon"));
    return empty_string();
  }

  config_init_game(config);

  if (game_history_get_waiting_for_final_pass_or_challenge(
          config->game_history)) {
    error_stack_push(
        error_stack, ERROR_STATUS_GCG_PARSE_PREMATURE_TIME_PENALTY,
        string_duplicate("cannot apply a time penalty while waiting for "
                         "the final pass or challenge"));
    return empty_string();
  }

  const char *player_nickname =
      config_get_parg_value(config, ARG_TOKEN_OVERTIME, 0);
  int player_index = -1;
  for (int i = 0; i < 2; i++) {
    if (strings_equal(game_history_player_get_nickname(config->game_history, i),
                      player_nickname)) {
      player_index = i;
      break;
    }
  }
  if (player_index == -1) {
    error_stack_push(
        error_stack, ERROR_STATUS_TIME_PENALTY_UNRECOGNIZED_PLAYER_NICKNAME,
        get_formatted_string(
            "player nickname '%s' specified for overtime penalty does not "
            "match either existing player '%s' or '%s'",
            player_nickname,
            game_history_player_get_nickname(config->game_history, 0),
            game_history_player_get_nickname(config->game_history, 1)));
    return empty_string();
  }

  int overtime_penalty_int;
  string_to_int_or_push_error(
      config_get_parg_name(config, ARG_TOKEN_OVERTIME),
      config_get_parg_value(config, ARG_TOKEN_OVERTIME, 1), INT_MIN, -1,
      ERROR_STATUS_TIME_PENALTY_INVALID_VALUE, &overtime_penalty_int,
      error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return empty_string();
  }

  config_backup_game_and_history(config);

  GameEvent *time_penalty_event =
      game_history_add_game_event(config->game_history, error_stack);

  if (error_stack_is_empty(error_stack)) {
    // Find the previous cumulative score for this player.
    const int last_event_index =
        game_history_get_num_events(config->game_history) - 1;
    Equity cumulative_score = EQUITY_INITIAL_VALUE;
    for (int i = last_event_index; i >= 0; i--) {
      const GameEvent *gei = game_history_get_event(config->game_history, i);
      if (game_event_get_player_index(gei) == player_index) {
        cumulative_score = game_event_get_cumulative_score(gei);
        break;
      }
    }

    if (cumulative_score == EQUITY_INITIAL_VALUE) {
      error_stack_push(
          error_stack, ERROR_STATUS_TIME_PENALTY_NO_PREVIOUS_CUMULATIVE_SCORE,
          get_formatted_string(
              "no prior game event has a cumulative score for player '%s' when "
              "applying time panelty",
              player_nickname));
      return empty_string();
    }
    const Equity overtime_penalty = int_to_equity(overtime_penalty_int);
    game_event_set_player_index(time_penalty_event, player_index);
    game_event_set_type(time_penalty_event, GAME_EVENT_TIME_PENALTY);
    game_event_set_cgp_move_string(time_penalty_event, NULL);
    game_event_set_score_adjustment(time_penalty_event, overtime_penalty);
    // Add the overtime penalty since the value is already negative
    game_event_set_cumulative_score(time_penalty_event,
                                    cumulative_score + overtime_penalty);
    game_event_set_move_score(time_penalty_event, 0);

    // When adding an overtime event, always advance to the end of the
    // history so that the game play module can play through the entire
    // game and catch any duplicate time penalty errors.
    game_history_goto(config->game_history,
                      game_history_get_num_events(config->game_history),
                      error_stack);
    if (error_stack_is_empty(error_stack)) {
      config_game_play_events(config, error_stack);
    }
  }
  if (!error_stack_is_empty(error_stack)) {
    config_restore_game_and_history(config);
    return empty_string();
  }
  return empty_string();
}

void execute_overtime(Config *config, ErrorStack *error_stack) {
  char *result = impl_overtime(config, error_stack);
  if (error_stack_is_empty(error_stack)) {
    thread_control_print(config->thread_control, result);
    execute_show(config, error_stack);
  }
  free(result);
}

char *str_api_overtime(Config *config, ErrorStack *error_stack) {
  return impl_overtime(config, error_stack);
}

// Switch names command

char *impl_switch_names(Config *config, ErrorStack *error_stack) {
  if (!config_has_game_data(config)) {
    error_stack_push(error_stack, ERROR_STATUS_CONFIG_LOAD_GAME_DATA_MISSING,
                     string_duplicate("cannot switch names without a lexicon"));
    return empty_string();
  }
  // The players_data_switch_names function needs to be called before
  // config_init_game so that the const char * player names in the
  // game are updated to the new pointers.
  players_data_switch_names(config->players_data);
  config_init_game(config);
  update_game_history_with_config(config);
  return empty_string();
}

void execute_switch_names(Config *config, ErrorStack *error_stack) {
  char *result = impl_switch_names(config, error_stack);
  if (error_stack_is_empty(error_stack)) {
    thread_control_print(config->thread_control, result);
    execute_show(config, error_stack);
  }
  free(result);
}

char *str_api_switch_names(Config *config, ErrorStack *error_stack) {
  return impl_switch_names(config, error_stack);
}

// Game navigation helper and command

char *impl_next(Config *config, ErrorStack *error_stack) {
  if (!config_has_game_data(config)) {
    error_stack_push(error_stack, ERROR_STATUS_CONFIG_LOAD_GAME_DATA_MISSING,
                     string_duplicate("cannot show next without loaded game"));
    return empty_string();
  }

  config_init_game(config);

  game_history_next(config->game_history, error_stack);

  if (!error_stack_is_empty(error_stack)) {
    return empty_string();
  }

  config_game_play_events(config, error_stack);

  if (!error_stack_is_empty(error_stack)) {
    return empty_string();
  }
  return empty_string();
}

void execute_next(Config *config, ErrorStack *error_stack) {
  char *result = impl_next(config, error_stack);
  if (error_stack_is_empty(error_stack)) {
    thread_control_print(config->thread_control, result);
    execute_show(config, error_stack);
  }
  free(result);
}

char *str_api_next(Config *config, ErrorStack *error_stack) {
  return impl_next(config, error_stack);
}

char *impl_previous(Config *config, ErrorStack *error_stack) {
  if (!config_has_game_data(config)) {
    error_stack_push(
        error_stack, ERROR_STATUS_CONFIG_LOAD_GAME_DATA_MISSING,
        string_duplicate("cannot show previous without loaded game"));
    return empty_string();
  }

  config_init_game(config);

  game_history_previous(config->game_history, error_stack);

  if (!error_stack_is_empty(error_stack)) {
    return empty_string();
  }

  config_game_play_events(config, error_stack);

  if (!error_stack_is_empty(error_stack)) {
    return empty_string();
  }
  return empty_string();
}

void execute_previous(Config *config, ErrorStack *error_stack) {
  char *result = impl_previous(config, error_stack);
  if (error_stack_is_empty(error_stack)) {
    thread_control_print(config->thread_control, result);
    execute_show(config, error_stack);
  }
  free(result);
}

char *str_api_previous(Config *config, ErrorStack *error_stack) {
  return impl_previous(config, error_stack);
}

char *impl_goto(Config *config, ErrorStack *error_stack) {
  if (!config_has_game_data(config)) {
    error_stack_push(
        error_stack, ERROR_STATUS_CONFIG_LOAD_GAME_DATA_MISSING,
        string_duplicate("cannot show specified turn without loaded game"));
    return empty_string();
  }

  const char *num_events_to_play_str =
      config_get_parg_value(config, ARG_TOKEN_GOTO, 0);
  int num_events_to_play;
  if (strings_iequal(num_events_to_play_str, "end")) {
    num_events_to_play = game_history_get_num_events(config->game_history);
  } else if (strings_iequal(num_events_to_play_str, "start")) {
    num_events_to_play = 0;
  } else {
    num_events_to_play = string_to_int(num_events_to_play_str, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return empty_string();
    }
  }

  config_init_game(config);

  const int old_num_played_events =
      game_history_get_num_played_events(config->game_history);
  game_history_goto(config->game_history, num_events_to_play, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return empty_string();
  }

  if (game_history_get_num_played_events(config->game_history) ==
      old_num_played_events) {
    return empty_string();
  }

  config_game_play_events(config, error_stack);

  if (!error_stack_is_empty(error_stack)) {
    return empty_string();
  }
  return empty_string();
}

void execute_goto(Config *config, ErrorStack *error_stack) {
  char *result = impl_goto(config, error_stack);
  if (error_stack_is_empty(error_stack)) {
    thread_control_print(config->thread_control, result);
    execute_show(config, error_stack);
  }
  free(result);
}

char *str_api_goto(Config *config, ErrorStack *error_stack) {
  return impl_goto(config, error_stack);
}

// Load GCG

void config_load_game_history(Config *config, const GameHistory *game_history,
                              ErrorStack *error_stack) {
  StringBuilder *cfg_load_cmd_builder = string_builder_create();
  const char *lexicon = game_history_get_lexicon_name(game_history);
  const char *ld_name = game_history_get_ld_name(game_history);
  const char *board_layout_name =
      game_history_get_board_layout_name(game_history);
  const game_variant_t game_variant =
      game_history_get_game_variant(game_history);
  const char *player_nicknames[2] = {NULL, NULL};

  for (int i = 0; i < 2; i++) {
    player_nicknames[i] = game_history_player_get_nickname(game_history, i);
  }

  string_builder_add_formatted_string(cfg_load_cmd_builder, "%s ",
                                      config->pargs[ARG_TOKEN_SET]->name);

  if (lexicon) {
    string_builder_add_formatted_string(cfg_load_cmd_builder, "-%s %s ",
                                        config->pargs[ARG_TOKEN_LEXICON]->name,
                                        lexicon);
  } else {
    log_fatal("missing lexicon for game history");
  }

  if (ld_name) {
    string_builder_add_formatted_string(
        cfg_load_cmd_builder, "-%s %s ",
        config->pargs[ARG_TOKEN_LETTER_DISTRIBUTION]->name, ld_name);
  } else {
    log_fatal("missing letter distribution for game history");
  }

  if (board_layout_name) {
    string_builder_add_formatted_string(
        cfg_load_cmd_builder, "-%s %s ",
        config->pargs[ARG_TOKEN_BOARD_LAYOUT]->name, board_layout_name);
  } else {
    log_fatal("missing board layout for game history");
  }

  switch (game_variant) {
  case GAME_VARIANT_CLASSIC:
    string_builder_add_formatted_string(
        cfg_load_cmd_builder, "-%s %s ",
        config->pargs[ARG_TOKEN_GAME_VARIANT]->name, GAME_VARIANT_CLASSIC_NAME);
    break;
  case GAME_VARIANT_WORDSMOG:
    string_builder_add_formatted_string(
        cfg_load_cmd_builder, "-%s %s ",
        config->pargs[ARG_TOKEN_GAME_VARIANT]->name,
        GAME_VARIANT_WORDSMOG_NAME);
    break;
  default:
    log_fatal("game history has unknown game variant enum: %d", game_variant);
  }

  for (int i = 0; i < 2; i++) {
    if (!player_nicknames[i]) {
      continue;
    }
    arg_token_t pname_arg_token =
        i == 0 ? ARG_TOKEN_P1_NAME : ARG_TOKEN_P2_NAME;
    string_builder_add_formatted_string(cfg_load_cmd_builder, "-%s %s ",
                                        config->pargs[pname_arg_token]->name,
                                        player_nicknames[i]);
  }

  char *cfg_load_cmd = string_builder_dump(cfg_load_cmd_builder, NULL);
  string_builder_destroy(cfg_load_cmd_builder);
  config_load_command(config, cfg_load_cmd, error_stack);
  free(cfg_load_cmd);
}

void config_parse_gcg_string_with_parser(Config *config, GCGParser *gcg_parser,
                                         const GameHistory *game_history,
                                         ErrorStack *error_stack) {
  parse_gcg_settings(gcg_parser, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
  config_load_game_history(config, game_history, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
  config_init_game(config);
  parse_gcg_events(gcg_parser, config->game, error_stack);
}

void config_parse_gcg_string(Config *config, const char *gcg_string,
                             GameHistory *game_history,
                             ErrorStack *error_stack) {
  if (is_string_empty_or_whitespace(gcg_string)) {
    error_stack_push(error_stack, ERROR_STATUS_GCG_PARSE_GCG_EMPTY,
                     string_duplicate("GCG is empty"));
    return;
  }
  game_history_reset(game_history);
  GCGParser *gcg_parser =
      gcg_parser_create(gcg_string, game_history,
                        players_data_get_data_name(config->players_data,
                                                   PLAYERS_DATA_TYPE_KWG, 0),
                        error_stack);
  if (error_stack_is_empty(error_stack)) {
    config_parse_gcg_string_with_parser(config, gcg_parser, game_history,
                                        error_stack);
  }
  gcg_parser_destroy(gcg_parser);
}

void config_parse_gcg(Config *config, const char *gcg_filename,
                      GameHistory *game_history, ErrorStack *error_stack) {
  char *gcg_string = get_string_from_file(gcg_filename, error_stack);
  if (error_stack_is_empty(error_stack)) {
    config_parse_gcg_string(config, gcg_string, game_history, error_stack);
  }
  free(gcg_string);
}

char *impl_load_gcg(Config *config, ErrorStack *error_stack) {
  const char *source_identifier =
      config_get_parg_value(config, ARG_TOKEN_LOAD, 0);
  GetGCGArgs download_args = {.source_identifier = source_identifier};
  char *gcg_string = get_gcg(&download_args, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    // It is guaranteed that gcg_string will be NULL here
    return empty_string();
  }

  const bool use_backup = !!config->game;
  if (use_backup) {
    config_backup_game_and_history(config);
  }

  config_parse_gcg_string(config, gcg_string, config->game_history,
                          error_stack);
  free(gcg_string);
  if (error_stack_is_empty(error_stack)) {
    game_history_goto(config->game_history, 0, error_stack);
    if (error_stack_is_empty(error_stack)) {
      config_game_play_events(config, error_stack);
    }
  }

  if (!error_stack_is_empty(error_stack)) {
    if (use_backup) {
      config_restore_game_and_history(config);
    }
    return empty_string();
  }

  return empty_string();
}

void execute_load_gcg(Config *config, ErrorStack *error_stack) {
  char *result = impl_load_gcg(config, error_stack);
  if (error_stack_is_empty(error_stack)) {
    thread_control_print(config->thread_control, result);
    execute_show(config, error_stack);
  }
  free(result);
}

char *str_api_load_gcg(Config *config, ErrorStack *error_stack) {
  return impl_load_gcg(config, error_stack);
}

// Config load helpers

void config_load_parsed_args(Config *config,
                             const StringSplitter *cmd_split_string,
                             ErrorStack *error_stack) {
  int number_of_input_strs =
      string_splitter_get_number_of_items(cmd_split_string);
  config->exec_parg_token = NUMBER_OF_ARG_TOKENS;

  for (int k = 0; k < NUMBER_OF_ARG_TOKENS; k++) {
    config->pargs[k]->num_set_values = 0;
  }

  ParsedArg *current_parg = NULL;
  arg_token_t current_arg_token;
  int current_value_index = 0;

  for (int i = 0; i < number_of_input_strs; i++) {
    const char *input_str = string_splitter_get_item(cmd_split_string, i);

    bool is_cmd = i == 0;
    bool is_arg = string_length(input_str) > 1 && input_str[0] == '-' &&
                  !isdigit(input_str[1]);

    if (is_arg || is_cmd) {
      if (current_parg) {
        if (current_value_index < current_parg->num_req_values) {
          error_stack_push(
              error_stack,
              ERROR_STATUS_CONFIG_LOAD_INSUFFICIENT_NUMBER_OF_VALUES,
              get_formatted_string("insufficient number of values for argument "
                                   "%s, expected %d, got %d",
                                   current_parg->name,
                                   current_parg->num_req_values,
                                   current_value_index));
          return;
        }
        current_parg = NULL;
      }

      // Commands do not have a leading dash, only
      // remove them from arguments
      const char *arg_name = input_str + 1;
      if (is_cmd) {
        arg_name = input_str;
      }

      bool parg_has_prefix[NUMBER_OF_ARG_TOKENS] = {false};
      bool ambiguous_arg_name = false;
      for (int k = 0; k < NUMBER_OF_ARG_TOKENS; k++) {
        if (has_prefix(arg_name, config->pargs[k]->name)) {
          parg_has_prefix[k] = true;
          if (current_parg) {
            ambiguous_arg_name = true;
          } else {
            current_parg = config->pargs[k];
            current_arg_token = k;
          }
        }
      }

      if (ambiguous_arg_name) {
        StringBuilder *sb = string_builder_create();
        string_builder_add_formatted_string(
            sb, "ambiguous command '%s' could be:", arg_name);
        for (int k = 0; k < NUMBER_OF_ARG_TOKENS; k++) {
          if (parg_has_prefix[k]) {
            string_builder_add_formatted_string(sb, " %s,",
                                                config->pargs[k]->name);
          }
        }
        // Remove the trailing comma
        string_builder_truncate(sb, string_builder_length(sb) - 1);
        error_stack_push(error_stack,
                         ERROR_STATUS_CONFIG_LOAD_AMBIGUOUS_COMMAND,
                         string_builder_dump(sb, NULL));
        string_builder_destroy(sb);
        return;
      }

      if (!current_parg) {
        error_stack_push(
            error_stack, ERROR_STATUS_CONFIG_LOAD_UNRECOGNIZED_ARG,
            get_formatted_string("unrecognized command or argument '%s'",
                                 arg_name));
        return;
      }

      if (current_parg->num_set_values > 0) {
        error_stack_push(
            error_stack, ERROR_STATUS_CONFIG_LOAD_DUPLICATE_ARG,
            get_formatted_string("command was provided more than once: %s",
                                 arg_name));
        return;
      }

      if (current_parg->exec_func != execute_fatal) {
        if (i > 0) {
          error_stack_push(error_stack,
                           ERROR_STATUS_CONFIG_LOAD_MISPLACED_COMMAND,
                           get_formatted_string(
                               "encountered unexpected command: %s", arg_name));
          return;
        }
        config->exec_parg_token = current_arg_token;
      }
      current_value_index = 0;
    } else {
      if (!current_parg || current_value_index >= current_parg->num_values) {
        error_stack_push(
            error_stack, ERROR_STATUS_CONFIG_LOAD_UNRECOGNIZED_ARG,
            get_formatted_string("unrecognized command or argument '%s'",
                                 input_str));
        return;
      }
      free(current_parg->values[current_value_index]);
      current_parg->values[current_value_index] = string_duplicate(input_str);
      current_value_index++;
      current_parg->num_set_values = current_value_index;
    }
  }
  if (current_parg && current_value_index < current_parg->num_req_values) {
    error_stack_push(
        error_stack, ERROR_STATUS_CONFIG_LOAD_INSUFFICIENT_NUMBER_OF_VALUES,
        get_formatted_string(
            "insufficient number of values provided for the '%s' command",
            current_parg->name));
    return;
  }
}

void config_load_sort_type(Config *config, const char *sort_type_str,
                           int player_index, ErrorStack *error_stack) {
  if (has_iprefix(sort_type_str, "equity")) {
    players_data_set_move_sort_type(config->players_data, player_index,
                                    MOVE_SORT_EQUITY);
  } else if (has_iprefix(sort_type_str, "score")) {
    players_data_set_move_sort_type(config->players_data, player_index,
                                    MOVE_SORT_SCORE);
  } else {
    error_stack_push(
        error_stack, ERROR_STATUS_CONFIG_LOAD_MALFORMED_MOVE_SORT_TYPE,
        get_formatted_string("unrecognized move sort type: %s", sort_type_str));
  }
}

void config_load_record_type(Config *config, const char *record_type_str,
                             int player_index, ErrorStack *error_stack) {
  if (has_iprefix(record_type_str, "best")) {
    players_data_set_move_record_type(config->players_data, player_index,
                                      MOVE_RECORD_BEST);
  } else if (has_iprefix(record_type_str, "equity")) {
    players_data_set_move_record_type(config->players_data, player_index,
                                      MOVE_RECORD_WITHIN_X_EQUITY_OF_BEST);
  } else if (has_iprefix(record_type_str, "all")) {
    players_data_set_move_record_type(config->players_data, player_index,
                                      MOVE_RECORD_ALL);
  } else if (has_iprefix(record_type_str, "small")) {
    players_data_set_move_record_type(config->players_data, player_index,
                                      MOVE_RECORD_ALL_SMALL);
  } else {
    error_stack_push(error_stack,
                     ERROR_STATUS_CONFIG_LOAD_MALFORMED_MOVE_RECORD_TYPE,
                     get_formatted_string("unrecognized move record type: %s",
                                          record_type_str));
  }
}

void config_load_sampling_rule(Config *config, const char *sampling_rule_str,
                               ErrorStack *error_stack) {
  if (has_iprefix(sampling_rule_str, "rr")) {
    config->sampling_rule = BAI_SAMPLING_RULE_ROUND_ROBIN;
  } else if (has_iprefix(sampling_rule_str, "tt")) {
    config->sampling_rule = BAI_SAMPLING_RULE_TOP_TWO_IDS;
  } else if (has_iprefix(sampling_rule_str, "oldtt")) {
    config->sampling_rule = BAI_SAMPLING_RULE_TOP_TWO;
  } else {
    error_stack_push(error_stack,
                     ERROR_STATUS_CONFIG_LOAD_MALFORMED_SAMPLING_RULE,
                     get_formatted_string("unrecognized sampling rule: %s",
                                          sampling_rule_str));
  }
}

void config_load_threshold(Config *config, const char *threshold_str,
                           ErrorStack *error_stack) {
  if (has_iprefix(threshold_str, "none")) {
    config->threshold = BAI_THRESHOLD_NONE;
  } else if (has_iprefix(threshold_str, "gk16")) {
    config->threshold = BAI_THRESHOLD_GK16;
  } else {
    error_stack_push(
        error_stack, ERROR_STATUS_CONFIG_LOAD_MALFORMED_THRESHOLD,
        get_formatted_string("unrecognized threshold type: %s", threshold_str));
  }
}

bool lex_lex_compat(const char *p1_lexicon_name, const char *p2_lexicon_name,
                    ErrorStack *error_stack) {
  if (!p1_lexicon_name && !p2_lexicon_name) {
    return true;
  }
  if (!p1_lexicon_name || !p2_lexicon_name) {
    return false;
  }
  ld_t p1_ld_type = ld_get_type_from_lex_name(p1_lexicon_name, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return false;
  }
  ld_t p2_ld_type = ld_get_type_from_lex_name(p2_lexicon_name, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return false;
  }
  return ld_types_compat(p1_ld_type, p2_ld_type);
}

bool lex_ld_compat(const char *lexicon_name, const char *ld_name,
                   ErrorStack *error_stack) {
  if (!lexicon_name && !ld_name) {
    return true;
  }
  if (!lexicon_name || !ld_name) {
    return false;
  }
  ld_t lexicon_ld_type = ld_get_type_from_lex_name(lexicon_name, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return false;
  }
  ld_t ld_ld_type = ld_get_type_from_ld_name(ld_name, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return false;
  }
  return ld_types_compat(lexicon_ld_type, ld_ld_type);
}

bool lexicons_and_leaves_compat(const char *updated_p1_lexicon_name,
                                const char *updated_p1_leaves_name,
                                const char *updated_p2_lexicon_name,
                                const char *updated_p2_leaves_name,
                                ErrorStack *error_stack) {
  const char *first_lex_compat_name = updated_p1_leaves_name;
  const char *second_lex_compat_name = updated_p2_leaves_name;
  const bool leaves_are_compatible = lex_lex_compat(
      first_lex_compat_name, second_lex_compat_name, error_stack);
  if (!error_stack_is_empty(error_stack) || !leaves_are_compatible) {
    return false;
  }

  const bool lex1_and_leaves1_are_compatible = lex_lex_compat(
      updated_p1_lexicon_name, updated_p1_leaves_name, error_stack);
  if (!error_stack_is_empty(error_stack) || !lex1_and_leaves1_are_compatible) {
    return false;
  }

  first_lex_compat_name = updated_p2_lexicon_name;
  second_lex_compat_name = updated_p2_leaves_name;
  const bool lex2_and_leaves2_are_compatible = lex_lex_compat(
      first_lex_compat_name, second_lex_compat_name, error_stack);
  if (!error_stack_is_empty(error_stack) || !lex2_and_leaves2_are_compatible) {
    return false;
  }
  return true;
}

char *get_default_klv_name(const char *lexicon_name) {
  return string_duplicate(lexicon_name);
}

// Exec mode

exec_mode_t get_exec_mode_type_from_name(const char *exec_mode_str) {
  exec_mode_t exec_mode = EXEC_MODE_UNKNOWN;
  if (has_iprefix(exec_mode_str, "sync")) {
    exec_mode = EXEC_MODE_SYNC;
  } else if (has_iprefix(exec_mode_str, "async")) {
    exec_mode = EXEC_MODE_ASYNC;
  }
  return exec_mode;
}

void config_load_lexicon_dependent_data(Config *config,
                                        ErrorStack *error_stack) {
  // Lexical player data

  // For both the kwg and klv, we disallow any non-NULL -> NULL transitions.
  // Once the kwg and klv are set for both players, they can change to new
  // lexica or leave values, but they can never change to NULL. Therefore, if
  // new names are NULL, it means they weren't specified for this command and
  // the existing kwg and klv types should persist.
  const char *new_lexicon_name =
      config_get_parg_value(config, ARG_TOKEN_LEXICON, 0);

  const char *new_p1_lexicon_name = new_lexicon_name;
  const char *new_p2_lexicon_name = new_lexicon_name;

  // The "l1" and "l2" args override the "lex" arg
  if (config_get_parg_num_set_values(config, ARG_TOKEN_P1_LEXICON) > 0) {
    new_p1_lexicon_name =
        config_get_parg_value(config, ARG_TOKEN_P1_LEXICON, 0);
  }

  if (config_get_parg_num_set_values(config, ARG_TOKEN_P2_LEXICON) > 0) {
    new_p2_lexicon_name =
        config_get_parg_value(config, ARG_TOKEN_P2_LEXICON, 0);
  }

  const char *new_leaves_name =
      config_get_parg_value(config, ARG_TOKEN_LEAVES, 0);

  const char *new_p1_leaves_name = new_leaves_name;
  const char *new_p2_leaves_name = new_leaves_name;

  // The "k1" and "k2" args override the "leaves" arg
  if (config_get_parg_num_set_values(config, ARG_TOKEN_P1_LEAVES) > 0) {
    new_p1_leaves_name = config_get_parg_value(config, ARG_TOKEN_P1_LEAVES, 0);
  }

  if (config_get_parg_num_set_values(config, ARG_TOKEN_P2_LEAVES) > 0) {
    new_p2_leaves_name = config_get_parg_value(config, ARG_TOKEN_P2_LEAVES, 0);
  }

  const char *new_ld_name =
      config_get_parg_value(config, ARG_TOKEN_LETTER_DISTRIBUTION, 0);

  // Load the lexicons
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
  if (!updated_p1_lexicon_name && updated_p2_lexicon_name) {
    error_stack_push(error_stack, ERROR_STATUS_CONFIG_LOAD_LEXICON_MISSING,
                     string_duplicate("missing lexicon for player 1"));
    return;
  }
  if (updated_p1_lexicon_name && !updated_p2_lexicon_name) {
    error_stack_push(error_stack, ERROR_STATUS_CONFIG_LOAD_LEXICON_MISSING,
                     string_duplicate("missing lexicon for player 2"));
    return;
  }

  // Determine the status of the wmp for both players
  // Start by assuming we are just using whatever the existing wmp settings are
  bool p1_wmp_use_when_available = players_data_get_use_when_available(
      config->players_data, PLAYERS_DATA_TYPE_WMP, 0);
  bool p2_wmp_use_when_available = players_data_get_use_when_available(
      config->players_data, PLAYERS_DATA_TYPE_WMP, 1);

  if (config_get_parg_value(config, ARG_TOKEN_USE_WMP, 0)) {
    config_load_bool(config, ARG_TOKEN_USE_WMP, &p1_wmp_use_when_available,
                     error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
    p2_wmp_use_when_available = p1_wmp_use_when_available;
  }

  // The "w1" and "w2" args override the "use_wmp" arg
  if (config_get_parg_value(config, ARG_TOKEN_P1_USE_WMP, 0)) {
    config_load_bool(config, ARG_TOKEN_P1_USE_WMP, &p1_wmp_use_when_available,
                     error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
  }

  if (config_get_parg_value(config, ARG_TOKEN_P2_USE_WMP, 0)) {
    config_load_bool(config, ARG_TOKEN_P2_USE_WMP, &p2_wmp_use_when_available,
                     error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
  }

  players_data_set_use_when_available(config->players_data,
                                      PLAYERS_DATA_TYPE_WMP, 0,
                                      p1_wmp_use_when_available);
  players_data_set_use_when_available(config->players_data,
                                      PLAYERS_DATA_TYPE_WMP, 1,
                                      p2_wmp_use_when_available);

  // Both lexicons are not specified, so we don't
  // load any of the lexicon dependent data
  if (!updated_p1_lexicon_name && !updated_p2_lexicon_name) {
    // We can use the new_* variables here since if lexicons
    // are null, it is guaranteed that there are no leaves or ld
    // since they are all set after this if check.
    if (new_p1_leaves_name || new_p2_leaves_name || new_ld_name) {
      error_stack_push(
          error_stack, ERROR_STATUS_CONFIG_LOAD_LEXICON_MISSING,
          string_duplicate(
              "cannot set leaves or letter distribution without a lexicon"));
    }
    return;
  }

  const bool lex_lex_is_compat = lex_lex_compat(
      updated_p1_lexicon_name, updated_p2_lexicon_name, error_stack);

  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  if (!lex_lex_is_compat) {
    error_stack_push(
        error_stack, ERROR_STATUS_CONFIG_LOAD_INCOMPATIBLE_LEXICONS,
        get_formatted_string("lexicons are incompatible: %s, %s",
                             updated_p1_lexicon_name, updated_p2_lexicon_name));
    return;
  }

  // Set the use_default bool here because the 'existing_p1_lexicon_name'
  // variable might be free'd in players_data_set.
  const bool use_default = !lex_lex_compat(
      updated_p1_lexicon_name, existing_p1_lexicon_name, error_stack);

  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  // Load lexica
  players_data_set(config->players_data, PLAYERS_DATA_TYPE_KWG,
                   config->data_paths, updated_p1_lexicon_name,
                   updated_p2_lexicon_name, error_stack);

  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  // Load lexica (in WMP format)

  // For the wmp, we allow non-NULL -> NULL transitions.

  const char *p1_wmp_name = NULL;
  if (p1_wmp_use_when_available) {
    p1_wmp_name = updated_p1_lexicon_name;
  }

  const char *p2_wmp_name = NULL;
  if (p2_wmp_use_when_available) {
    p2_wmp_name = updated_p2_lexicon_name;
  }

  players_data_set(config->players_data, PLAYERS_DATA_TYPE_WMP,
                   config->data_paths, p1_wmp_name, p2_wmp_name, error_stack);

  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  // Load the leaves

  const char *existing_p1_leaves_name = players_data_get_data_name(
      config->players_data, PLAYERS_DATA_TYPE_KLV, 0);
  char *updated_p1_leaves_name = NULL;
  if (new_p1_leaves_name) {
    updated_p1_leaves_name = string_duplicate(new_p1_leaves_name);
  } else if (use_default || !existing_p1_leaves_name) {
    updated_p1_leaves_name = get_default_klv_name(updated_p1_lexicon_name);
  } else {
    updated_p1_leaves_name = string_duplicate(existing_p1_leaves_name);
  }

  const char *existing_p2_leaves_name = players_data_get_data_name(
      config->players_data, PLAYERS_DATA_TYPE_KLV, 1);
  char *updated_p2_leaves_name = NULL;
  if (new_p2_leaves_name) {
    updated_p2_leaves_name = string_duplicate(new_p2_leaves_name);
  } else if (use_default || !existing_p2_leaves_name) {
    updated_p2_leaves_name = get_default_klv_name(updated_p2_lexicon_name);
  } else {
    updated_p2_leaves_name = string_duplicate(existing_p2_leaves_name);
  }

  const bool leaves_and_lexicons_are_compatible = lexicons_and_leaves_compat(
      updated_p1_lexicon_name, updated_p1_leaves_name, updated_p2_lexicon_name,
      updated_p2_leaves_name, error_stack);

  if (error_stack_is_empty(error_stack)) {
    if (!leaves_and_lexicons_are_compatible) {
      error_stack_push(error_stack,
                       ERROR_STATUS_CONFIG_LOAD_INCOMPATIBLE_LEXICONS,
                       get_formatted_string(
                           "one or more of the leaves are incompatible with "
                           "the current lexicons or each other: %s, %s",
                           updated_p1_leaves_name, updated_p2_leaves_name));
    } else {
      players_data_set(config->players_data, PLAYERS_DATA_TYPE_KLV,
                       config->data_paths, updated_p1_leaves_name,
                       updated_p2_leaves_name, error_stack);
      autoplay_results_set_klv(config->autoplay_results,
                               players_data_get_data(config->players_data,
                                                     PLAYERS_DATA_TYPE_KLV, 0));
    }
  }

  free(updated_p1_leaves_name);
  free(updated_p2_leaves_name);

  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  // Load letter distribution

  const char *existing_ld_name = NULL;
  if (config->ld) {
    existing_ld_name = ld_get_name(config->ld);
  }
  char *updated_ld_name = NULL;
  if (new_ld_name) {
    updated_ld_name = string_duplicate(new_ld_name);
  } else if (use_default || !existing_ld_name) {
    updated_ld_name = ld_get_default_name_from_lexicon_name(
        updated_p1_lexicon_name, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
  } else {
    updated_ld_name = string_duplicate(existing_ld_name);
  }

  const bool lex_ld_is_compat =
      lex_ld_compat(updated_p1_lexicon_name, updated_ld_name, error_stack);
  if (error_stack_is_empty(error_stack)) {
    if (!lex_ld_is_compat) {
      error_stack_push(
          error_stack,
          ERROR_STATUS_CONFIG_LOAD_INCOMPATIBLE_LETTER_DISTRIBUTION,
          get_formatted_string(
              "lexicon %s is incompatible with letter distribution %s",
              updated_p1_lexicon_name, updated_ld_name));
    } else {
      // If the letter distribution name has changed, update it
      config->ld_changed = false;
      if (!strings_equal(updated_ld_name, existing_ld_name)) {
        ld_destroy(config->ld);
        config->ld =
            ld_create(config->data_paths, updated_ld_name, error_stack);
        if (error_stack_is_empty(error_stack)) {
          config->ld_changed = true;
        }
      }
      autoplay_results_set_ld(config->autoplay_results, config->ld);
    }
  }
  free(updated_ld_name);
}

// Assumes all args are parsed and correctly set in pargs.
void config_load_data(Config *config, ErrorStack *error_stack) {
  const char *new_path = config_get_parg_value(config, ARG_TOKEN_DATA_PATH, 0);
  if (new_path) {
    free(config->data_paths);
    config->data_paths = string_duplicate(new_path);
  }
  autoplay_results_set_data_paths(config->autoplay_results, config->data_paths);
  // Exec Mode

  const char *new_exec_mode_str =
      config_get_parg_value(config, ARG_TOKEN_EXEC_MODE, 0);
  if (new_exec_mode_str) {
    exec_mode_t new_exec_mode = get_exec_mode_type_from_name(new_exec_mode_str);
    if (new_exec_mode == EXEC_MODE_UNKNOWN) {
      error_stack_push(error_stack,
                       ERROR_STATUS_CONFIG_LOAD_UNRECOGNIZED_EXEC_MODE,
                       get_formatted_string("unrecognized exec mode: %s",
                                            new_exec_mode_str));
      return;
    }
    config->exec_mode = new_exec_mode;
  }

  // Int values

  config_load_int(config, ARG_TOKEN_BINGO_BONUS, INT_MIN, INT_MAX,
                  &config->bingo_bonus, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  config_load_int(config, ARG_TOKEN_CHALLENGE_BONUS, 0, INT_MAX,
                  &config->challenge_bonus, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  config_load_int(config, ARG_TOKEN_PLIES, 1, MAX_PLIES, &config->plies,
                  error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  config_load_int(config, ARG_TOKEN_ENDGAME_PLIES, 1, MAX_VARIANT_LENGTH,
                  &config->endgame_plies, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  config_load_int(config, ARG_TOKEN_NUMBER_OF_PLAYS, 1, INT_MAX,
                  &config->num_plays, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  config_load_int(config, ARG_TOKEN_NUM_LEAVES, 0, INT_MAX,
                  &config->num_leaves, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  config_load_int(config, ARG_TOKEN_MAX_ITERATIONS, 1, INT_MAX,
                  &config->max_iterations, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  config_load_int(config, ARG_TOKEN_MIN_PLAY_ITERATIONS, 2, INT_MAX,
                  &config->min_play_iterations, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  config_load_int(config, ARG_TOKEN_NUMBER_OF_THREADS, 1, MAX_THREADS,
                  &config->num_threads, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  config_load_int(config, ARG_TOKEN_PRINT_INFO_INTERVAL, 0, INT_MAX,
                  &config->print_interval, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  config_load_int(config, ARG_TOKEN_TIME_LIMIT, 0, INT_MAX,
                  &config->time_limit_seconds, error_stack);

  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  // Double values

  const char *new_stop_cond_str =
      config_get_parg_value(config, ARG_TOKEN_STOP_COND_PCT, 0);
  if (new_stop_cond_str && has_iprefix(new_stop_cond_str, "none")) {
    config->stop_cond_pct = 1000;
  } else {
    config_load_double(config, ARG_TOKEN_STOP_COND_PCT, 1e-10, 100 - 1e-10,
                       &config->stop_cond_pct, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
  }

  const char *new_eq_margin_inference_double =
      config_get_parg_value(config, ARG_TOKEN_EQ_MARGIN_INFERENCE, 0);
  if (new_eq_margin_inference_double) {
    double eq_margin_inference_double = 0;
    config_load_double(config, ARG_TOKEN_EQ_MARGIN_INFERENCE, 0,
                       EQUITY_MAX_DOUBLE, &eq_margin_inference_double,
                       error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
    assert(!isnan(eq_margin_inference_double));
    config->eq_margin_inference = double_to_equity(eq_margin_inference_double);
  }

  const char *new_eq_margin_movegen =
      config_get_parg_value(config, ARG_TOKEN_EQ_MARGIN_MOVEGEN, 0);
  if (new_eq_margin_movegen) {
    double eq_margin_movegen = NAN;
    config_load_double(config, ARG_TOKEN_EQ_MARGIN_MOVEGEN, 0,
                       EQUITY_MAX_DOUBLE, &eq_margin_movegen, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
    assert(!isnan(eq_margin_movegen));
    config->eq_margin_movegen = double_to_equity(eq_margin_movegen);
  }

  config_load_double(config, ARG_TOKEN_TT_FRACTION_OF_MEM, 0, 1,
                     &config->tt_fraction_of_mem, error_stack);

  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  // Game variant

  const char *new_game_variant_str =
      config_get_parg_value(config, ARG_TOKEN_GAME_VARIANT, 0);
  if (new_game_variant_str) {
    game_variant_t new_game_variant =
        get_game_variant_type_from_name(new_game_variant_str);
    if (new_game_variant == GAME_VARIANT_UNKNOWN) {
      error_stack_push(error_stack,
                       ERROR_STATUS_CONFIG_LOAD_UNRECOGNIZED_GAME_VARIANT,
                       get_formatted_string("unrecognized game variant: %s",
                                            new_game_variant_str));
      return;
    }
    config->game_variant = new_game_variant;
  }

  // Game pairs

  config_load_bool(config, ARG_TOKEN_USE_GAME_PAIRS, &config->use_game_pairs,
                   error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  config_load_bool(config, ARG_TOKEN_USE_SMALL_PLAYS, &config->use_small_plays,
                   error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  // Sim with inference

  config_load_bool(config, ARG_TOKEN_SIM_WITH_INFERENCE,
                   &config->sim_with_inference, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  // Human readable

  config_load_bool(config, ARG_TOKEN_HUMAN_READABLE, &config->human_readable,
                   error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  // Print boards

  config_load_bool(config, ARG_TOKEN_PRINT_BOARDS, &config->print_boards,
                   error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  // Print on finish

  config_load_bool(config, ARG_TOKEN_PRINT_ON_FINISH, &config->print_on_finish,
                   error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  // Show prompt

  config_load_bool(config, ARG_TOKEN_SHOW_PROMPT, &config->show_prompt,
                   error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  // Board color

  const char *board_color_str =
      config_get_parg_value(config, ARG_TOKEN_BOARD_COLOR, 0);
  if (board_color_str) {
    if (strings_iequal(board_color_str, "none")) {
      config->game_string_options->board_color = GAME_STRING_BOARD_COLOR_NONE;
    } else if (strings_iequal(board_color_str, "ansi")) {
      config->game_string_options->board_color = GAME_STRING_BOARD_COLOR_ANSI;
    } else if (strings_iequal(board_color_str, "xterm256")) {
      config->game_string_options->board_color =
          GAME_STRING_BOARD_COLOR_XTERM_256;
    } else if (strings_iequal(board_color_str, "truecolor")) {
      config->game_string_options->board_color =
          GAME_STRING_BOARD_COLOR_TRUECOLOR;
    } else {
      error_stack_push(
          error_stack, ERROR_STATUS_CONFIG_LOAD_UNRECOGNIZED_BOARD_COLOR,
          get_formatted_string("invalid board color: %s", board_color_str));
      return;
    }
  }

  // Board tile glyphs

  const char *board_tiles_str =
      config_get_parg_value(config, ARG_TOKEN_BOARD_TILE_GLYPHS, 0);
  if (board_tiles_str) {
    if (strings_iequal(board_tiles_str, "primary")) {
      config->game_string_options->board_tile_glyphs =
          GAME_STRING_BOARD_TILE_GLYPHS_PRIMARY;
    } else if (strings_iequal(board_tiles_str, "alt")) {
      config->game_string_options->board_tile_glyphs =
          GAME_STRING_BOARD_TILE_GLYPHS_ALT;
    } else {
      error_stack_push(
          error_stack, ERROR_STATUS_CONFIG_LOAD_UNRECOGNIZED_BOARD_TILES,
          get_formatted_string("invalid board tiles: %s", board_tiles_str));
      return;
    }
  }

  // Board border

  const char *board_border_str =
      config_get_parg_value(config, ARG_TOKEN_BOARD_BORDER, 0);
  if (board_border_str) {
    if (strings_iequal(board_border_str, "ascii")) {
      config->game_string_options->board_border =
          GAME_STRING_BOARD_BORDER_ASCII;
    } else if (strings_iequal(board_border_str, "box")) {
      config->game_string_options->board_border =
          GAME_STRING_BOARD_BORDER_BOX_DRAWING;
    } else {
      error_stack_push(
          error_stack, ERROR_STATUS_CONFIG_LOAD_UNRECOGNIZED_BOARD_BORDER,
          get_formatted_string("invalid board border: %s", board_border_str));
      return;
    }
  }

  // Board column labels

  const char *board_columns_str =
      config_get_parg_value(config, ARG_TOKEN_BOARD_COLUMN_LABEL, 0);
  if (board_columns_str) {
    if (strings_iequal(board_columns_str, "ascii")) {
      config->game_string_options->board_column_label =
          GAME_STRING_BOARD_COLUMN_LABEL_ASCII;
    } else if (strings_iequal(board_columns_str, "fullwidth")) {
      config->game_string_options->board_column_label =
          GAME_STRING_BOARD_COLUMN_LABEL_FULLWIDTH;
    } else {
      error_stack_push(
          error_stack, ERROR_STATUS_CONFIG_LOAD_UNRECOGNIZED_BOARD_COLUMNS,
          get_formatted_string("invalid board columns: %s", board_columns_str));
      return;
    }
  }

  // On-turn marker

  const char *on_turn_marker_str =
      config_get_parg_value(config, ARG_TOKEN_ON_TURN_MARKER, 0);
  if (on_turn_marker_str) {
    if (strings_iequal(on_turn_marker_str, "ascii")) {
      config->game_string_options->on_turn_marker =
          GAME_STRING_ON_TURN_MARKER_ASCII;
    } else if (strings_iequal(on_turn_marker_str, "arrowhead")) {
      config->game_string_options->on_turn_marker =
          GAME_STRING_ON_TURN_MARKER_ARROWHEAD;
    } else {
      error_stack_push(error_stack,
                       ERROR_STATUS_CONFIG_LOAD_UNRECOGNIZED_ON_TURN_MARKER,
                       get_formatted_string("invalid on-turn marker: %s",
                                            on_turn_marker_str));
      return;
    }
  }

  // On-turn color

  const char *on_turn_color_str =
      config_get_parg_value(config, ARG_TOKEN_ON_TURN_COLOR, 0);
  if (on_turn_color_str) {
    if (strings_iequal(on_turn_color_str, "none")) {
      config->game_string_options->on_turn_color =
          GAME_STRING_ON_TURN_COLOR_NONE;
    } else if (strings_iequal(on_turn_color_str, "green")) {
      config->game_string_options->on_turn_color =
          GAME_STRING_ON_TURN_COLOR_ANSI_GREEN;
    } else {
      error_stack_push(
          error_stack, ERROR_STATUS_CONFIG_LOAD_UNRECOGNIZED_ON_TURN_COLOR,
          get_formatted_string("invalid on-turn color: %s", on_turn_color_str));
      return;
    }
  }

  // On-turn score style

  const char *on_turn_score_str =
      config_get_parg_value(config, ARG_TOKEN_ON_TURN_SCORE_STYLE, 0);
  if (on_turn_score_str) {
    if (strings_iequal(on_turn_score_str, "normal")) {
      config->game_string_options->on_turn_score_style =
          GAME_STRING_ON_TURN_SCORE_NORMAL;
    } else if (strings_iequal(on_turn_score_str, "bold")) {
      config->game_string_options->on_turn_score_style =
          GAME_STRING_ON_TURN_SCORE_BOLD;
    } else {
      error_stack_push(
          error_stack,
          ERROR_STATUS_CONFIG_LOAD_UNRECOGNIZED_ON_TURN_SCORE_STYLE,
          get_formatted_string("invalid on-turn score style: %s",
                               on_turn_score_str));
      return;
    }
  }

  // Pretty mode - sets multiple board options at once

  bool pretty_mode = false;
  config_load_bool(config, ARG_TOKEN_PRETTY, &pretty_mode, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
  if (pretty_mode) {
    config->print_boards = true;
    config->game_string_options->board_color = GAME_STRING_BOARD_COLOR_ANSI;
    config->game_string_options->board_tile_glyphs =
        GAME_STRING_BOARD_TILE_GLYPHS_ALT;
    config->game_string_options->board_border =
        GAME_STRING_BOARD_BORDER_BOX_DRAWING;
    config->game_string_options->board_column_label =
        GAME_STRING_BOARD_COLUMN_LABEL_FULLWIDTH;
    config->game_string_options->on_turn_marker =
        GAME_STRING_ON_TURN_MARKER_ARROWHEAD;
    config->game_string_options->on_turn_color =
        GAME_STRING_ON_TURN_COLOR_ANSI_GREEN;
  }

  const char *write_buffer_size_str =
      config_get_parg_value(config, ARG_TOKEN_WRITE_BUFFER_SIZE, 0);
  if (write_buffer_size_str) {
    int write_buffer_size = 0;
    config_load_int(config, ARG_TOKEN_WRITE_BUFFER_SIZE, 1, INT_MAX,
                    &write_buffer_size, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
    autoplay_results_set_write_buffer_size(config->autoplay_results,
                                           (size_t)write_buffer_size);
  }

  // Seed

  config_load_uint64(config, ARG_TOKEN_RANDOM_SEED, &config->seed, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  // Board layout
  const char *new_board_layout_name =
      config_get_parg_value(config, ARG_TOKEN_BOARD_LAYOUT, 0);
  if (new_board_layout_name &&
      !strings_equal(board_layout_get_name(config->board_layout),
                     new_board_layout_name)) {
    board_layout_load(config->board_layout, config->data_paths,
                      new_board_layout_name, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      error_stack_push(
          error_stack, ERROR_STATUS_CONFIG_LOAD_BOARD_LAYOUT_ERROR,
          string_duplicate("encountered an error loading the board layout"));
      return;
    }
  }

  const char *sampling_rule =
      config_get_parg_value(config, ARG_TOKEN_SAMPLING_RULE, 0);
  if (sampling_rule) {
    config_load_sampling_rule(config, sampling_rule, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
  }

  const char *threshold = config_get_parg_value(config, ARG_TOKEN_THRESHOLD, 0);
  if (threshold) {
    config_load_threshold(config, threshold, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
  }

  // Non-lexical player data

  const arg_token_t sort_type_args[2] = {ARG_TOKEN_P1_MOVE_SORT_TYPE,
                                         ARG_TOKEN_P2_MOVE_SORT_TYPE};
  const arg_token_t record_type_args[2] = {ARG_TOKEN_P1_MOVE_RECORD_TYPE,
                                           ARG_TOKEN_P2_MOVE_RECORD_TYPE};
  const arg_token_t pname_args[2] = {ARG_TOKEN_P1_NAME, ARG_TOKEN_P2_NAME};

  for (int player_index = 0; player_index < 2; player_index++) {
    const char *new_player_sort_type_str =
        config_get_parg_value(config, sort_type_args[player_index], 0);
    if (new_player_sort_type_str) {
      config_load_sort_type(config, new_player_sort_type_str, player_index,
                            error_stack);
      if (!error_stack_is_empty(error_stack)) {
        return;
      }
    }

    const char *new_player_record_type_str =
        config_get_parg_value(config, record_type_args[player_index], 0);
    if (new_player_record_type_str) {
      config_load_record_type(config, new_player_record_type_str, player_index,
                              error_stack);
      if (!error_stack_is_empty(error_stack)) {
        return;
      }
    }

    const char *new_player_name =
        config_get_parg_value(config, pname_args[player_index], 0);
    if (new_player_name) {
      players_data_set_name(config->players_data, player_index,
                            new_player_name);
    }
  }

  config_load_lexicon_dependent_data(config, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  // Update the game history
  update_game_history_with_config(config);

  // Set win pct

  const char *new_win_pct_name =
      config_get_parg_value(config, ARG_TOKEN_WIN_PCT, 0);
  if (new_win_pct_name &&
      !strings_equal(win_pct_get_name(config->win_pcts), new_win_pct_name)) {
    win_pct_destroy(config->win_pcts);
    config->win_pcts =
        win_pct_create(config->data_paths, new_win_pct_name, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
  }

  // Load per-player sim params (default to 0 plies = no sims)
  config->p1_sim_plies = 0;
  config->p2_sim_plies = 0;

  if (config_get_parg_value(config, ARG_TOKEN_P1_SIM_PLIES, 0)) {
    config_load_int(config, ARG_TOKEN_P1_SIM_PLIES, 0, MAX_PLIES,
                    &config->p1_sim_plies, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
  }

  if (config_get_parg_value(config, ARG_TOKEN_P2_SIM_PLIES, 0)) {
    config_load_int(config, ARG_TOKEN_P2_SIM_PLIES, 0, MAX_PLIES,
                    &config->p2_sim_plies, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
  }

  // Default per-player num_plays to global value
  config->p1_sim_num_plays = config->num_plays;
  config->p2_sim_num_plays = config->num_plays;

  if (config_get_parg_value(config, ARG_TOKEN_P1_SIM_NUM_PLAYS, 0)) {
    config_load_int(config, ARG_TOKEN_P1_SIM_NUM_PLAYS, 1, INT_MAX,
                    &config->p1_sim_num_plays, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
  }

  if (config_get_parg_value(config, ARG_TOKEN_P2_SIM_NUM_PLAYS, 0)) {
    config_load_int(config, ARG_TOKEN_P2_SIM_NUM_PLAYS, 1, INT_MAX,
                    &config->p2_sim_num_plays, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
  }

  // Default per-player max_iterations to global value
  config->p1_sim_max_iterations = config->max_iterations;
  config->p2_sim_max_iterations = config->max_iterations;

  if (config_get_parg_value(config, ARG_TOKEN_P1_SIM_MAX_ITERATIONS, 0)) {
    config_load_int(config, ARG_TOKEN_P1_SIM_MAX_ITERATIONS, 1, INT_MAX,
                    &config->p1_sim_max_iterations, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
  }

  if (config_get_parg_value(config, ARG_TOKEN_P2_SIM_MAX_ITERATIONS, 0)) {
    config_load_int(config, ARG_TOKEN_P2_SIM_MAX_ITERATIONS, 1, INT_MAX,
                    &config->p2_sim_max_iterations, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
  }

  // Default per-player min_play_iterations to global value
  config->p1_sim_min_play_iterations = config->min_play_iterations;
  config->p2_sim_min_play_iterations = config->min_play_iterations;

  if (config_get_parg_value(config, ARG_TOKEN_P1_SIM_MIN_PLAY_ITERATIONS, 0)) {
    config_load_int(config, ARG_TOKEN_P1_SIM_MIN_PLAY_ITERATIONS, 2, INT_MAX,
                    &config->p1_sim_min_play_iterations, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
  }

  if (config_get_parg_value(config, ARG_TOKEN_P2_SIM_MIN_PLAY_ITERATIONS, 0)) {
    config_load_int(config, ARG_TOKEN_P2_SIM_MIN_PLAY_ITERATIONS, 2, INT_MAX,
                    &config->p2_sim_min_play_iterations, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
  }

  // Cap per-player min_play_iterations to ensure BAI can run
  if (config->p1_sim_num_plays > 0) {
    int max_min_play_iters =
        config->p1_sim_max_iterations / config->p1_sim_num_plays;
    if (config->p1_sim_min_play_iterations > max_min_play_iters) {
      config->p1_sim_min_play_iterations =
          max_min_play_iters < 2 ? 2 : max_min_play_iters;
    }
  }
  if (config->p2_sim_num_plays > 0) {
    int max_min_play_iters =
        config->p2_sim_max_iterations / config->p2_sim_num_plays;
    if (config->p2_sim_min_play_iterations > max_min_play_iters) {
      config->p2_sim_min_play_iterations =
          max_min_play_iters < 2 ? 2 : max_min_play_iters;
    }
  }

  // Default per-player stop_cond_pct to global value
  config->p1_sim_stop_cond_pct = config->stop_cond_pct;
  config->p2_sim_stop_cond_pct = config->stop_cond_pct;

  const char *new_p1_stop_cond_str =
      config_get_parg_value(config, ARG_TOKEN_P1_SIM_STOP_COND_PCT, 0);
  if (new_p1_stop_cond_str) {
    if (has_iprefix(new_p1_stop_cond_str, "none")) {
      config->p1_sim_stop_cond_pct = 1000;
    } else {
      config_load_double(config, ARG_TOKEN_P1_SIM_STOP_COND_PCT, 1e-10,
                         100 - 1e-10, &config->p1_sim_stop_cond_pct,
                         error_stack);
      if (!error_stack_is_empty(error_stack)) {
        return;
      }
    }
  }

  const char *new_p2_stop_cond_str =
      config_get_parg_value(config, ARG_TOKEN_P2_SIM_STOP_COND_PCT, 0);
  if (new_p2_stop_cond_str) {
    if (has_iprefix(new_p2_stop_cond_str, "none")) {
      config->p2_sim_stop_cond_pct = 1000;
    } else {
      config_load_double(config, ARG_TOKEN_P2_SIM_STOP_COND_PCT, 1e-10,
                         100 - 1e-10, &config->p2_sim_stop_cond_pct,
                         error_stack);
      if (!error_stack_is_empty(error_stack)) {
        return;
      }
    }
  }

  // Default per-player use_inference to global sim_with_inference value
  config->p1_sim_use_inference = config->sim_with_inference;
  config->p2_sim_use_inference = config->sim_with_inference;

  if (config_get_parg_value(config, ARG_TOKEN_P1_SIM_USE_INFERENCE, 0)) {
    config_load_bool(config, ARG_TOKEN_P1_SIM_USE_INFERENCE,
                     &config->p1_sim_use_inference, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
  }

  if (config_get_parg_value(config, ARG_TOKEN_P2_SIM_USE_INFERENCE, 0)) {
    config_load_bool(config, ARG_TOKEN_P2_SIM_USE_INFERENCE,
                     &config->p2_sim_use_inference, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
  }

  // Set sim params for each player
  SimParams p1_params = {
      .plies = config->p1_sim_plies,
      .num_plays = config->p1_sim_num_plays,
      .max_iterations = config->p1_sim_max_iterations,
      .min_play_iterations = config->p1_sim_min_play_iterations,
      .stop_cond_pct = config->p1_sim_stop_cond_pct,
      .use_inference = config->p1_sim_use_inference,
  };
  SimParams p2_params = {
      .plies = config->p2_sim_plies,
      .num_plays = config->p2_sim_num_plays,
      .max_iterations = config->p2_sim_max_iterations,
      .min_play_iterations = config->p2_sim_min_play_iterations,
      .stop_cond_pct = config->p2_sim_stop_cond_pct,
      .use_inference = config->p2_sim_use_inference,
  };
  players_data_set_sim_params(config->players_data, 0, &p1_params);
  players_data_set_sim_params(config->players_data, 1, &p2_params);
}

// Parses the arguments given by the cmd string and updates the state of
// the config data values, but does not execute the command.
void config_load_command(Config *config, const char *cmd,
                         ErrorStack *error_stack) {
  // If the command is empty, consider this a set options
  // command where zero options are set and return without error.
  if (is_string_empty_or_whitespace(cmd)) {
    return;
  }

  StringSplitter *cmd_split_string = split_string_by_whitespace(cmd, true);
  // CGP data can have semicolons at the end, so
  // we trim these off to make loading easier.
  string_splitter_trim_char(cmd_split_string, ';');
  config_load_parsed_args(config, cmd_split_string, error_stack);

  if (error_stack_is_empty(error_stack)) {
    config_load_data(config, error_stack);
  }

  string_splitter_destroy(cmd_split_string);
}

void config_execute_command(Config *config, ErrorStack *error_stack) {
  if (config_exec_parg_is_set(config)) {
    config_get_parg_exec_func(config, config->exec_parg_token)(config,
                                                               error_stack);
    if (config->print_on_finish) {
      char *finished_msg = get_status_finished_str(config);
      thread_control_print(config_get_thread_control(config), finished_msg);
      free(finished_msg);
    }
  }
}

bool config_run_str_api_command(Config *config, ErrorStack *error_stack,
                                char **output) {
  if (!config_exec_parg_is_set(config)) {
    return false;
  }
  *output = config_get_parg_api_func(config, config->exec_parg_token)(
      config, error_stack);
  thread_control_set_status(config_get_thread_control(config),
                            THREAD_CONTROL_STATUS_FINISHED);
  return true;
}

char *config_get_execute_status(Config *config) {
  char *status = NULL;
  if (config_exec_parg_is_set(config)) {
    status =
        config_get_parg_status_func(config, config->exec_parg_token)(config);
  }
  return status;
}

// API surface
// -------------------------------------
// There are two sets of API functions:
// 1. execute_*, called by the magpie console and cli
//    - prints output, if any, to stdout
// 2. str_api_*, meant to be called by programs embedding magpie as a library
//    - all functions are char* func(Config*, ErrorStack*, char* cmd)
//    - uses the same parser as the execute commands, but returns output as a
//      string

void execute_load_cgp(Config *config, ErrorStack *error_stack) {
  impl_load_cgp(config, error_stack);
}

char *str_api_load_cgp(Config *config, ErrorStack *error_stack) {
  impl_load_cgp(config, error_stack);
  return empty_string();
}

void execute_add_moves(Config *config, ErrorStack *error_stack) {
  impl_add_moves(config, error_stack);
}

char *str_api_add_moves(Config *config, ErrorStack *error_stack) {
  impl_add_moves(config, error_stack);
  return empty_string();
}

void execute_set_rack(Config *config, ErrorStack *error_stack) {
  impl_set_rack(config, error_stack);
}

char *str_api_set_rack(Config *config, ErrorStack *error_stack) {
  impl_set_rack(config, error_stack);
  return empty_string();
}

void execute_move_gen(Config *config, ErrorStack *error_stack) {
  impl_move_gen(config, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
  print_ucgi_static_moves(config->game, config->move_list,
                          config->thread_control);
}

char *str_api_move_gen(Config *config, ErrorStack *error_stack) {
  impl_move_gen(config, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return empty_string();
  }
  return ucgi_static_moves(config->game, config->move_list);
}

void execute_sim(Config *config, ErrorStack *error_stack) {
  impl_sim(config, error_stack);
}

char *str_api_sim(Config *config, ErrorStack *error_stack) {
  impl_sim(config, error_stack);
  return empty_string();
}

void execute_infer(Config *config, ErrorStack *error_stack) {
  impl_infer(config, error_stack);
  gen_destroy_cache();
}

char *str_api_infer(Config *config, ErrorStack *error_stack) {
  impl_infer(config, error_stack);
  gen_destroy_cache();
  return empty_string();
}

void execute_endgame(Config *config, ErrorStack *error_stack) {
  impl_endgame(config, error_stack);
}

char *str_api_endgame(Config *config, ErrorStack *error_stack) {
  impl_endgame(config, error_stack);
  return empty_string();
}

void execute_autoplay(Config *config, ErrorStack *error_stack) {
  impl_autoplay(config, error_stack);
}

char *str_api_autoplay(Config *config, ErrorStack *error_stack) {
  impl_autoplay(config, error_stack);
  return empty_string();
}

void execute_convert(Config *config, ErrorStack *error_stack) {
  impl_convert(config, error_stack);
}

char *str_api_convert(Config *config, ErrorStack *error_stack) {
  impl_convert(config, error_stack);
  return empty_string();
}

void execute_leave_gen(Config *config, ErrorStack *error_stack) {
  impl_leave_gen(config, error_stack);
}

char *str_api_leave_gen(Config *config, ErrorStack *error_stack) {
  impl_leave_gen(config, error_stack);
  return empty_string();
}

void execute_create_data(Config *config, ErrorStack *error_stack) {
  impl_create_data(config, error_stack);
}

char *str_api_create_data(Config *config, ErrorStack *error_stack) {
  impl_create_data(config, error_stack);
  return empty_string();
}

void config_create_default_internal(Config *config, ErrorStack *error_stack,
                                    const char *data_paths) {
  // Attempt to load fields that might fail first
  config->data_paths = string_duplicate(data_paths);
  config->board_layout =
      board_layout_create_default(config->data_paths, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_push(
        error_stack, ERROR_STATUS_CONFIG_LOAD_BOARD_LAYOUT_ERROR,
        string_duplicate(
            "encountered an error loading the default board layout"));
    return;
  }

  config->win_pcts =
      win_pct_create(config->data_paths, DEFAULT_WIN_PCT, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_push(
        error_stack, ERROR_STATUS_CONFIG_LOAD_WIN_PCT_ERROR,
        string_duplicate(
            "encountered an error loading the default win percentage file"));
    return;
  }

  // Command parsed from string input
#define cmd(token, name, n_req, n_val, func, stat)                             \
  parsed_arg_create(config, token, name, n_req, n_val, execute_##func,         \
                    str_api_##func, status_##stat)

  // Non-command arg
#define arg(token, name, n_req, n_val)                                         \
  parsed_arg_create(config, token, name, n_req, n_val, execute_fatal,          \
                    str_api_fatal, status_generic)

  cmd(ARG_TOKEN_SET, "setoptions", 0, 0, noop, generic);
  cmd(ARG_TOKEN_CGP, "cgp", 4, 4, load_cgp, generic);
  cmd(ARG_TOKEN_LOAD, "load", 1, 1, load_gcg, generic);
  cmd(ARG_TOKEN_NEW_GAME, "newgame", 0, 2, new_game, generic);
  cmd(ARG_TOKEN_EXPORT, "export", 0, 1, export, generic);
  cmd(ARG_TOKEN_COMMIT, "commit", 1, 3, commit, generic);
  cmd(ARG_TOKEN_CHALLENGE, "challenge", 0, 1, challenge, generic);
  cmd(ARG_TOKEN_UNCHALLENGE, "unchallenge", 0, 1, unchallenge, generic);
  cmd(ARG_TOKEN_OVERTIME, "overtimepenalty", 2, 2, overtime, generic);
  cmd(ARG_TOKEN_SWITCH_NAMES, "switchnames", 0, 0, switch_names, generic);
  cmd(ARG_TOKEN_SHOW, "show", 0, 0, show, generic);
  cmd(ARG_TOKEN_MOVES, "addmoves", 1, 1, add_moves, generic);
  cmd(ARG_TOKEN_RACK, "rack", 1, 1, set_rack, generic);
  cmd(ARG_TOKEN_GEN, "generate", 0, 0, move_gen, generic);
  cmd(ARG_TOKEN_SIM, "simulate", 0, 1, sim, sim);
  cmd(ARG_TOKEN_INFER, "infer", 0, 5, infer, generic);
  cmd(ARG_TOKEN_ENDGAME, "endgame", 0, 0, endgame, generic);
  cmd(ARG_TOKEN_AUTOPLAY, "autoplay", 2, 2, autoplay, generic);
  cmd(ARG_TOKEN_CONVERT, "convert", 2, 3, convert, generic);
  cmd(ARG_TOKEN_LEAVE_GEN, "leavegen", 2, 2, leave_gen, generic);
  cmd(ARG_TOKEN_CREATE_DATA, "createdata", 2, 3, create_data, generic);
  cmd(ARG_TOKEN_NEXT, "next", 0, 0, next, generic);
  cmd(ARG_TOKEN_PREVIOUS, "previous", 0, 0, previous, generic);
  cmd(ARG_TOKEN_GOTO, "goto", 1, 1, goto, generic);

  arg(ARG_TOKEN_DATA_PATH, "path", 1, 1);
  arg(ARG_TOKEN_BINGO_BONUS, "bb", 1, 1);
  arg(ARG_TOKEN_CHALLENGE_BONUS, "cb", 1, 1);
  arg(ARG_TOKEN_BOARD_LAYOUT, "bdn", 1, 1);
  arg(ARG_TOKEN_GAME_VARIANT, "var", 1, 1);
  arg(ARG_TOKEN_LETTER_DISTRIBUTION, "ld", 1, 1);
  arg(ARG_TOKEN_LEXICON, "lex", 1, 1);
  arg(ARG_TOKEN_USE_WMP, "wmp", 1, 1);
  arg(ARG_TOKEN_LEAVES, "leaves", 1, 1);
  arg(ARG_TOKEN_P1_NAME, "p1", 1, 1);
  arg(ARG_TOKEN_P1_LEXICON, "l1", 1, 1);
  arg(ARG_TOKEN_P1_USE_WMP, "w1", 1, 1);
  arg(ARG_TOKEN_P1_LEAVES, "k1", 1, 1);
  arg(ARG_TOKEN_P1_MOVE_SORT_TYPE, "s1", 1, 1);
  arg(ARG_TOKEN_P1_MOVE_RECORD_TYPE, "r1", 1, 1);
  arg(ARG_TOKEN_P1_SIM_PLIES, "sp1", 1, 1);
  arg(ARG_TOKEN_P1_SIM_NUM_PLAYS, "np1", 1, 1);
  arg(ARG_TOKEN_P1_SIM_MAX_ITERATIONS, "ip1", 1, 1);
  arg(ARG_TOKEN_P1_SIM_STOP_COND_PCT, "scp1", 1, 1);
  arg(ARG_TOKEN_P1_SIM_MIN_PLAY_ITERATIONS, "mpi1", 1, 1);
  arg(ARG_TOKEN_P1_SIM_USE_INFERENCE, "is1", 1, 1);
  arg(ARG_TOKEN_P2_NAME, "p2", 1, 1);
  arg(ARG_TOKEN_P2_LEXICON, "l2", 1, 1);
  arg(ARG_TOKEN_P2_USE_WMP, "w2", 1, 1);
  arg(ARG_TOKEN_P2_LEAVES, "k2", 1, 1);
  arg(ARG_TOKEN_P2_MOVE_SORT_TYPE, "s2", 1, 1);
  arg(ARG_TOKEN_P2_MOVE_RECORD_TYPE, "r2", 1, 1);
  arg(ARG_TOKEN_P2_SIM_PLIES, "sp2", 1, 1);
  arg(ARG_TOKEN_P2_SIM_NUM_PLAYS, "np2", 1, 1);
  arg(ARG_TOKEN_P2_SIM_MAX_ITERATIONS, "ip2", 1, 1);
  arg(ARG_TOKEN_P2_SIM_STOP_COND_PCT, "scp2", 1, 1);
  arg(ARG_TOKEN_P2_SIM_MIN_PLAY_ITERATIONS, "mpi2", 1, 1);
  arg(ARG_TOKEN_P2_SIM_USE_INFERENCE, "is2", 1, 1);
  arg(ARG_TOKEN_WIN_PCT, "winpct", 1, 1);
  arg(ARG_TOKEN_PLIES, "plies", 1, 1);
  arg(ARG_TOKEN_ENDGAME_PLIES, "eplies", 1, 1);
  arg(ARG_TOKEN_NUMBER_OF_PLAYS, "numplays", 1, 1);
  arg(ARG_TOKEN_NUMBER_OF_SMALL_PLAYS, "numsmallplays", 1, 1);
  arg(ARG_TOKEN_NUM_LEAVES, "numleaves", 1, 1);
  arg(ARG_TOKEN_MAX_ITERATIONS, "iterations", 1, 1);
  arg(ARG_TOKEN_MIN_PLAY_ITERATIONS, "minplayiterations", 1, 1);
  arg(ARG_TOKEN_STOP_COND_PCT, "scondition", 1, 1);
  arg(ARG_TOKEN_EQ_MARGIN_INFERENCE, "equitymargin", 1, 1);
  arg(ARG_TOKEN_EQ_MARGIN_MOVEGEN, "maxequitydifference", 1, 1);
  arg(ARG_TOKEN_USE_GAME_PAIRS, "gp", 1, 1);
  arg(ARG_TOKEN_USE_SMALL_PLAYS, "sp", 1, 1);
  arg(ARG_TOKEN_SIM_WITH_INFERENCE, "sinfer", 1, 1);
  arg(ARG_TOKEN_HUMAN_READABLE, "hr", 1, 1);
  arg(ARG_TOKEN_WRITE_BUFFER_SIZE, "wb", 1, 1);
  arg(ARG_TOKEN_RANDOM_SEED, "seed", 1, 1);
  arg(ARG_TOKEN_NUMBER_OF_THREADS, "threads", 1, 1);
  arg(ARG_TOKEN_MULTI_THREADED_SIMS, "mts", 1, 1);
  arg(ARG_TOKEN_PRINT_INFO_INTERVAL, "pfrequency", 1, 1);
  arg(ARG_TOKEN_EXEC_MODE, "mode", 1, 1);
  arg(ARG_TOKEN_TT_FRACTION_OF_MEM, "ttfraction", 1, 1);
  arg(ARG_TOKEN_TIME_LIMIT, "tlim", 1, 1);
  arg(ARG_TOKEN_SAMPLING_RULE, "sr", 1, 1);
  arg(ARG_TOKEN_THRESHOLD, "threshold", 1, 1);
  arg(ARG_TOKEN_PRINT_BOARDS, "printboards", 1, 1);
  arg(ARG_TOKEN_BOARD_COLOR, "boardcolor", 1, 1);
  arg(ARG_TOKEN_BOARD_TILE_GLYPHS, "boardtiles", 1, 1);
  arg(ARG_TOKEN_BOARD_BORDER, "boardborder", 1, 1);
  arg(ARG_TOKEN_BOARD_COLUMN_LABEL, "boardcolumns", 1, 1);
  arg(ARG_TOKEN_ON_TURN_MARKER, "onturnmarker", 1, 1);
  arg(ARG_TOKEN_ON_TURN_COLOR, "onturncolor", 1, 1);
  arg(ARG_TOKEN_ON_TURN_SCORE_STYLE, "onturnscore", 1, 1);
  arg(ARG_TOKEN_PRETTY, "pretty", 1, 1);
  arg(ARG_TOKEN_PRINT_ON_FINISH, "printonfinish", 1, 1);
  arg(ARG_TOKEN_SHOW_PROMPT, "shprompt", 1, 1);

#undef cmd
#undef arg
  config->exec_parg_token = NUMBER_OF_ARG_TOKENS;
  config->ld_changed = false;
  config->exec_mode = EXEC_MODE_SYNC;
  config->bingo_bonus = DEFAULT_BINGO_BONUS;
  config->challenge_bonus = DEFAULT_CHALLENGE_BONUS;
  config->num_plays = DEFAULT_MOVE_LIST_CAPACITY;
  config->num_small_plays = DEFAULT_SMALL_MOVE_LIST_CAPACITY;
  config->num_leaves = DEFAULT_MOVE_LIST_CAPACITY;
  config->plies = 2;
  config->endgame_plies = 6;
  config->eq_margin_inference = 0;
  config->eq_margin_movegen = int_to_equity(10);
  config->min_play_iterations = 100;
  config->max_iterations = 5000;
  config->stop_cond_pct = 99;
  config->time_limit_seconds = 0;
  config->num_threads = 1;
  config->print_interval = 0;
  config->seed = ctime_get_current_time();
  config->sampling_rule = BAI_SAMPLING_RULE_TOP_TWO_IDS;
  config->threshold = BAI_THRESHOLD_GK16;
  config->use_game_pairs = false;
  config->use_small_plays = false;
  config->human_readable = false;
  config->sim_with_inference = false;
  config->print_boards = false;
  config->print_on_finish = false;
  config->show_prompt = true;
  config->game_variant = DEFAULT_GAME_VARIANT;
  config->ld = NULL;
  config->players_data = players_data_create();
  config->thread_control = thread_control_create();
  config->game = NULL;
  config->game_backup = NULL;
  config->move_list = NULL;
  config->game_history = game_history_create();
  config->game_history_backup = NULL;
  config->endgame_solver = endgame_solver_create();
  config->sim_results = sim_results_create();
  config->inference_results = inference_results_create(NULL);
  config->endgame_results = endgame_results_create();
  config->autoplay_results = autoplay_results_create();
  config->conversion_results = conversion_results_create();
  config->tt_fraction_of_mem = 0.25;
  config->game_string_options = game_string_options_create_default();

  autoplay_results_set_players_data(config->autoplay_results,
                                    config->players_data);
}

Config *config_create_default_with_data_paths(ErrorStack *error_stack,
                                              const char *data_paths) {
  Config *config = calloc_or_die(1, sizeof(Config));
  config_create_default_internal(config, error_stack, data_paths);
  return config;
}

void config_destroy(Config *config) {
  if (!config) {
    return;
  }
  for (int i = 0; i < NUMBER_OF_ARG_TOKENS; i++) {
    parsed_arg_destroy(config->pargs[i]);
  }
  win_pct_destroy(config->win_pcts);
  board_layout_destroy(config->board_layout);
  ld_destroy(config->ld);
  players_data_destroy(config->players_data);
  thread_control_destroy(config->thread_control);
  game_destroy(config->game);
  game_destroy(config->game_backup);
  game_history_destroy(config->game_history);
  game_history_destroy(config->game_history_backup);
  endgame_solver_destroy(config->endgame_solver);
  move_list_destroy(config->move_list);
  sim_results_destroy(config->sim_results);
  inference_results_destroy(config->inference_results);
  endgame_results_destroy(config->endgame_results);
  autoplay_results_destroy(config->autoplay_results);
  conversion_results_destroy(config->conversion_results);
  game_string_options_destroy(config->game_string_options);
  free(config->data_paths);
  free(config);
}
