#include "config.h"

#include "../compat/ctime.h"
#include "../compat/memory_info.h"
#include "../def/autoplay_defs.h"
#include "../def/bai_defs.h"
#include "../def/config_defs.h"
#include "../def/equity_defs.h"
#include "../def/exec_defs.h"
#include "../def/game_defs.h"
#include "../def/game_history_defs.h"
#include "../def/move_defs.h"
#include "../def/players_data_defs.h"
#include "../def/rack_defs.h"
#include "../def/sim_defs.h"
#include "../def/thread_control_defs.h"
#include "../def/validated_move_defs.h"
#include "../ent/autoplay_results.h"
#include "../ent/bag.h"
#include "../ent/board.h"
#include "../ent/board_layout.h"
#include "../ent/conversion_results.h"
#include "../ent/endgame_results.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/game_history.h"
#include "../ent/heat_map.h"
#include "../ent/inference_args.h"
#include "../ent/inference_results.h"
#include "../ent/klv.h"
#include "../ent/klv_csv.h"
#include "../ent/letter_distribution.h"
#include "../ent/move.h"
#include "../ent/player.h"
#include "../ent/players_data.h"
#include "../ent/rack.h"
#include "../ent/sim_args.h"
#include "../ent/sim_results.h"
#include "../ent/thread_control.h"
#include "../ent/trie.h"
#include "../ent/validated_move.h"
#include "../ent/win_pct.h"
#include "../str/endgame_string.h"
#include "../str/game_string.h"
#include "../str/inference_string.h"
#include "../str/move_string.h"
#include "../str/rack_string.h"
#include "../str/sim_string.h"
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

enum {
  HELP_INDENT = 10,
};

typedef enum {
  ARG_TOKEN_HELP,
  ARG_TOKEN_SET,
  ARG_TOKEN_CGP,
  ARG_TOKEN_MOVES,
  ARG_TOKEN_RACK,
  ARG_TOKEN_RANDOM_RACK,
  ARG_TOKEN_GEN,
  ARG_TOKEN_SIM,
  ARG_TOKEN_GEN_AND_SIM,
  ARG_TOKEN_RACK_AND_GEN_AND_SIM,
  ARG_TOKEN_INFER,
  ARG_TOKEN_ENDGAME,
  ARG_TOKEN_AUTOPLAY,
  ARG_TOKEN_CONVERT,
  ARG_TOKEN_P1_NAME,
  ARG_TOKEN_P2_NAME,
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
  ARG_TOKEN_P1_LEXICON,
  ARG_TOKEN_P1_USE_WMP,
  ARG_TOKEN_P1_LEAVES,
  ARG_TOKEN_P1_MOVE_SORT_TYPE,
  ARG_TOKEN_P1_MOVE_RECORD_TYPE,
  ARG_TOKEN_P2_LEXICON,
  ARG_TOKEN_P2_USE_WMP,
  ARG_TOKEN_P2_LEAVES,
  ARG_TOKEN_P2_MOVE_SORT_TYPE,
  ARG_TOKEN_P2_MOVE_RECORD_TYPE,
  ARG_TOKEN_WIN_PCT,
  ARG_TOKEN_PLIES,
  ARG_TOKEN_ENDGAME_PLIES,
  ARG_TOKEN_NUMBER_OF_PLAYS,
  ARG_TOKEN_MAX_NUMBER_OF_DISPLAY_PLAYS,
  ARG_TOKEN_NUMBER_OF_SMALL_PLAYS,
  ARG_TOKEN_MAX_ITERATIONS,
  ARG_TOKEN_STOP_COND_PCT,
  ARG_TOKEN_INFERENCE_MARGIN,
  ARG_TOKEN_MOVEGEN_MARGIN,
  ARG_TOKEN_MIN_PLAY_ITERATIONS,
  ARG_TOKEN_USE_GAME_PAIRS,
  ARG_TOKEN_USE_SMALL_PLAYS,
  ARG_TOKEN_SIM_WITH_INFERENCE,
  ARG_TOKEN_USE_HEAT_MAP,
  ARG_TOKEN_WRITE_BUFFER_SIZE,
  ARG_TOKEN_HUMAN_READABLE,
  ARG_TOKEN_RANDOM_SEED,
  ARG_TOKEN_NUMBER_OF_THREADS,
  ARG_TOKEN_PRINT_INTERVAL,
  ARG_TOKEN_EXEC_MODE,
  ARG_TOKEN_TT_FRACTION_OF_MEM,
  ARG_TOKEN_TIME_LIMIT,
  ARG_TOKEN_SAMPLING_RULE,
  ARG_TOKEN_THRESHOLD,
  ARG_TOKEN_CUTOFF,
  ARG_TOKEN_LOAD,
  ARG_TOKEN_NEW_GAME,
  ARG_TOKEN_EXPORT,
  ARG_TOKEN_COMMIT,
  ARG_TOKEN_TOP_COMMIT,
  ARG_TOKEN_CHALLENGE,
  ARG_TOKEN_UNCHALLENGE,
  ARG_TOKEN_OVERTIME,
  ARG_TOKEN_SWITCH_NAMES,
  ARG_TOKEN_SHOW_GAME,
  ARG_TOKEN_SHOW_MOVES,
  ARG_TOKEN_SHOW_INFERENCE,
  ARG_TOKEN_SHOW_ENDGAME,
  ARG_TOKEN_SHOW_HEAT_MAP,
  ARG_TOKEN_NEXT,
  ARG_TOKEN_PREVIOUS,
  ARG_TOKEN_GOTO,
  ARG_TOKEN_NOTE,
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
  ARG_TOKEN_SAVE_SETTINGS,
  ARG_TOKEN_AUTOSAVE_GCG,
  // This must always be the last
  // token for the count to be accurate
  NUMBER_OF_ARG_TOKENS
} arg_token_t;

typedef void (*command_exec_func_t)(Config *, ErrorStack *);
typedef char *(*command_api_func_t)(Config *, ErrorStack *);
typedef char *(*command_status_func_t)(Config *);

typedef struct ParsedArg {
  char *name;
  char *shortest_unambiguous_name;
  char **values;
  int num_req_values;
  int num_values;
  int num_set_values;
  command_exec_func_t exec_func;
  command_api_func_t api_func;
  command_status_func_t status_func;
  bool is_hotkey;
  bool is_command;
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
  int max_num_display_plays;
  int num_small_plays;
  int plies;
  int endgame_plies;
  uint64_t max_iterations;
  uint64_t min_play_iterations;
  double stop_cond_pct;
  double cutoff;
  Equity eq_margin_inference;
  Equity eq_margin_movegen;
  bool use_game_pairs;
  bool human_readable;
  bool use_small_plays;
  bool sim_with_inference;
  bool use_heat_map;
  bool print_boards;
  bool print_on_finish;
  bool show_prompt;
  bool save_settings;
  bool autosave_gcg;
  bool loaded_settings;
  char *record_filepath;
  char *settings_filename;
  double tt_fraction_of_mem;
  uint64_t time_limit_seconds;
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
                       command_status_func_t command_status_func,
                       const bool is_hotkey, const bool is_command) {
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
  parsed_arg->is_hotkey = is_hotkey;
  parsed_arg->is_command = is_command;

  config->pargs[arg_token] = parsed_arg;
}

void parsed_arg_destroy(ParsedArg *parsed_arg) {
  if (!parsed_arg) {
    return;
  }

  free(parsed_arg->name);
  free(parsed_arg->shortest_unambiguous_name);

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

int config_get_num_small_plays(const Config *config) {
  return config->num_small_plays;
}
int config_get_plies(const Config *config) { return config->plies; }

int config_get_endgame_plies(const Config *config) {
  return config->endgame_plies;
}

uint64_t config_get_max_iterations(const Config *config) {
  return config->max_iterations;
}

double config_get_stop_cond_pct(const Config *config) {
  return config->stop_cond_pct;
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

bool config_get_save_settings(const Config *config) {
  return config->save_settings;
}

bool config_get_loaded_settings(const Config *config) {
  return config->loaded_settings;
}

void config_set_loaded_settings(Config *config, const bool value) {
  config->loaded_settings = value;
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

const char *config_get_settings_filename(const Config *config) {
  return config->settings_filename;
}

int config_get_num_threads(const Config *config) { return config->num_threads; }

int config_get_print_interval(const Config *config) {
  return config->print_interval;
}

Equity config_get_eq_margin_inference(const Config *config) {
  return config->eq_margin_inference;
}

bool config_exec_parg_is_set(const Config *config) {
  return config->exec_parg_token != NUMBER_OF_ARG_TOKENS;
}

bool config_continue_on_coldstart(const Config *config) {
  // Each of these conditions indicates that the user wants to continue running
  // commands
  return config->exec_parg_token == ARG_TOKEN_SET ||
         config->exec_parg_token == ARG_TOKEN_CGP ||
         !config_exec_parg_is_set(config) ||
         config_get_parg_num_set_values(config, ARG_TOKEN_EXEC_MODE) > 0;
}

// Config command execution helper functions

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
  if (!config->game) {
    config->game = config_game_create(config);
  } else {
    config_game_update(config, config->game);
  }
}

void config_reset_move_list_and_invalidate_sim_results(Config *config) {
  if (config->move_list) {
    move_list_reset(config->move_list);
    Rack new_move_list_rack;
    rack_set_dist_size_and_reset(&new_move_list_rack, 0);
    if (config->game) {
      const Rack *player_on_turn_rack = player_get_rack(game_get_player(
          config->game, game_get_player_on_turn_index(config->game)));
      new_move_list_rack = *player_on_turn_rack;
    }
    move_list_set_rack(config->move_list, &new_move_list_rack);
  }
  sim_results_set_valid_for_current_game_state(config->sim_results, false);
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
      config_reset_move_list_and_invalidate_sim_results(config);
    } else {
      move_list_destroy(config->move_list);
      config->move_list = move_list_create(capacity);
    }
  } else if (list_type == MOVE_LIST_TYPE_SMALL) {
    if (!config->move_list) {
      config->move_list = move_list_create_small(capacity);
      config_reset_move_list_and_invalidate_sim_results(config);
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
  const int num_mls = rack_set_to_string_unblanked(ld, rack, rack_str);
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
        get_formatted_string("integer value for %s must be between %d and %d "
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
            "decimal value for %s must be between %f and %f inclusive, but "
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
                        uint64_t min, uint64_t *value,
                        ErrorStack *error_stack) {
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
  } else if (new_value < min) {
    error_stack_push(
        error_stack, ERROR_STATUS_CONFIG_LOAD_INT_ARG_OUT_OF_BOUNDS,
        get_formatted_string(
            "integer value for %s must be at least %lu, but got %s",
            config_get_parg_name(config, arg_token), min, int_str));
    return;
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

// Returns NUMBER_OF_ARG_TOKENS if there was an error or no token was found
arg_token_t get_token_from_string(Config *config, const char *arg_name,
                                  ErrorStack *error_stack) {
  arg_token_t current_arg_token = NUMBER_OF_ARG_TOKENS;
  bool parg_has_prefix[NUMBER_OF_ARG_TOKENS] = {false};
  bool is_ambiguous = false;
  for (int k = 0; k < NUMBER_OF_ARG_TOKENS; k++) {
    if (string_length(arg_name) == 1 && config->pargs[k]->is_hotkey &&
        arg_name[0] == config->pargs[k]->name[0]) {
      return k;
    }
    if (has_prefix(arg_name, config->pargs[k]->name)) {
      parg_has_prefix[k] = true;
      if (current_arg_token != NUMBER_OF_ARG_TOKENS) {
        is_ambiguous = true;
      } else {
        current_arg_token = k;
      }
    }
  }

  if (current_arg_token == NUMBER_OF_ARG_TOKENS) {
    error_stack_push(error_stack, ERROR_STATUS_CONFIG_LOAD_UNRECOGNIZED_ARG,
                     get_formatted_string(
                         "unrecognized command or argument '%s'", arg_name));
    return current_arg_token;
  }

  if (!is_ambiguous) {
    return current_arg_token;
  }

  StringBuilder *sb = string_builder_create();
  string_builder_add_formatted_string(
      sb, "ambiguous command '%s' could be:", arg_name);
  for (int k = 0; k < NUMBER_OF_ARG_TOKENS; k++) {
    if (parg_has_prefix[k]) {
      string_builder_add_formatted_string(sb, " %s,", config->pargs[k]->name);
    }
  }
  // Remove the trailing comma
  string_builder_truncate(sb, string_builder_length(sb) - 1);
  error_stack_push(error_stack, ERROR_STATUS_CONFIG_LOAD_AMBIGUOUS_COMMAND,
                   string_builder_dump(sb, NULL));
  string_builder_destroy(sb);
  return NUMBER_OF_ARG_TOKENS;
}

// Help
void add_help_arg_to_string_builder(const Config *config, int token,
                                    StringBuilder *sb,
                                    const bool show_async_commands) {
  const char *examples[10] = {NULL};
  const char *usages[10] = {NULL};
  const char *text = NULL;
  bool is_command = false;
  bool is_hotkey = false;
  const char *name = NULL;
  const char *shortest_unambiguous_name = NULL;
  if (show_async_commands) {
    async_token_t async_token = token;
    switch (async_token) {
    case ASYNC_STOP_COMMAND_TOKEN:
      usages[0] = "";
      text = "Stops the currently running command.";
      name = ASYNC_STOP_COMMAND_STRING;
      shortest_unambiguous_name = ASYNC_STOP_COMMAND_STRING_SHORT;
      break;
    case ASYNC_STATUS_COMMAND_TOKEN:;
      usages[0] = "";
      text = "Prints the status of the currently running command.";
      name = ASYNC_STATUS_COMMAND_STRING;
      shortest_unambiguous_name = ASYNC_STATUS_COMMAND_STRING_SHORT;
      break;
    case NUMBER_OF_ASYNC_COMMAND_TOKENS:
      log_fatal("encountered unexpected async command token in help command");
      break;
    }
  } else {
    arg_token_t arg_token = token;
    const ParsedArg *parg = config_get_parg(config, arg_token);
    is_command = parg->is_command;
    is_hotkey = parg->is_hotkey;
    name = parg->name;
    shortest_unambiguous_name = parg->shortest_unambiguous_name;
    switch (arg_token) {
    case ARG_TOKEN_HELP:
      usages[0] = "[<command_or_arg>]";
      examples[0] = "";
      examples[1] = "infer";
      text = "Prints a help message for the given command or argument. If no "
             "argument is "
             "given, prints help messages for all commands and arguments.";
      break;
    case ARG_TOKEN_SET:
      usages[0] = "<option1> <value1> <option2> <value2> ...";
      examples[0] = "-numplays 15 -minp 100";
      text = "Sets any number of specified options.";
      break;
    case ARG_TOKEN_CGP:
      usages[0] = "<board> <racks> <scores> <consecutive_zeros>";
      examples[0] = "15/15/15/15/14V/14A/14N/6GUM5N/7PEW4E/9EF3R/9BEVELS/15/15/"
                    "15/15 AEEIILZ/CDGKNOR 56/117 0";
      text = "Loads the specified CGP into the game.";
      break;
    case ARG_TOKEN_MOVES:
      usages[0] = "<move>";
      usages[1] = "<move>,<move>,...";
      examples[0] = " 8F.LIN";
      examples[1] = " 8F.LIN,8D.ZILLION,8F.ZILLION";
      text = "Adds the CGP moves to the move list. Multiple moves must be "
             "delimited by commas as opposed to spaces.";
      break;
    case ARG_TOKEN_RACK:
      usages[0] = "<rack>";
      text = "Sets the rack for the player on turn.";
      break;
    case ARG_TOKEN_RANDOM_RACK:
      usages[0] = "";
      text = "Sets a random rack for the player on turn.";
      break;
    case ARG_TOKEN_GEN:
      usages[0] = "";
      text = "Generates moves for the current position.";
      break;
    case ARG_TOKEN_SIM:
      usages[0] = "[<opponent_known_rack>]";
      examples[0] = "";
      examples[1] = "ABCD";
      examples[2] = "-";
      text = "Runs a Monte Carlo simulation for the current position using the "
             "specified opponent rack. If no rack is specified, the opponent "
             "known rack from the game history is used. If the game history "
             "has a known rack for the opponent, you can use '-' to force the "
             "sim to use a completely random rack.";
      break;
    case ARG_TOKEN_GEN_AND_SIM:
      usages[0] = "[<opponent_known_rack>]";
      examples[0] = "";
      examples[1] = "ABCD";
      examples[2] = "-";
      text = "Generates moves for the current position and runs a simulation.";
      break;
    case ARG_TOKEN_RACK_AND_GEN_AND_SIM:
      usages[0] = "<player_rack> [<opponent_known_rack>]";
      examples[0] = "ABCD";
      examples[1] = "ABCD EFG";
      examples[2] = "ABCD -";
      text = "Sets the current player rack, generates moves for the current "
             "position, and runs a simulation.";
      break;
    case ARG_TOKEN_INFER:
      usages[0] = "";
      usages[1] = "<target_nickname> <target_played_tiles> <target_score> "
                  "[<target_known_rack>] [<nontarget_known_rack>]";
      usages[2] = "<target_nickname> <target_num_exchanged> "
                  "[<target_known_rack>] [<nontarget_known_rack>]";
      examples[0] = "";
      examples[1] = "josh ABCDE 13";
      examples[2] = "josh ABCDE 13 ABCD";
      examples[3] = "josh ABCDE 13 ABCD EFG";
      examples[4] = "josh ABCDE 13 - EFG";
      examples[5] = "josh 3 ABCDE";
      examples[6] = "josh 3 ABCDE EFG";
      examples[7] = "josh 3 - EFG";
      text =
          "Runs an exhaustive inference for what the opponent could have kept "
          "based on their previous play. If no arguments are specified, the "
          "inference will use the previous play in the game history.";
      break;
    case ARG_TOKEN_ENDGAME:
      usages[0] = "";
      text = "Runs the endgame solver.";
      break;
    case ARG_TOKEN_AUTOPLAY:
      usages[0] = "<type1> <num_games>";
      usages[1] = "<type1>,<type2>,... <num_games>";
      examples[0] = "games 100";
      examples[1] = "games,winpct 1000";
      examples[2] = "leave,winpct 2000";
      text = "Runs the autoplay command with the specified recorder(s). If the "
             "game pairs option is set to true, autoplay will run <num_games> "
             "game pairs resulting in a total of 2 * <num_games> games.";
      break;
    case ARG_TOKEN_CONVERT:
      usages[0] = "<type> <input_name_without_extension> "
                  "[<output_name_without_extension>]";
      examples[0] = "klv2csv CSW21";
      examples[1] = "klv2csv CSW21 CSW21_new";
      examples[2] = "text2wordmap NWL20";
      text =
          "Runs the convert command for the specified type with the given "
          "input and output names. If no output name is specified, the input "
          "name will be used. Note that this will not overwrite the input "
          "since the output filename will have a different extension.";
      break;
    case ARG_TOKEN_LEAVE_GEN:
      usages[0] = "<gen1_min_rack_target>,<gen1_min_rack_target>,... "
                  "[<games_before_force_draw>]";
      examples[0] = "100,200,500,1000,1000,1000 100000000";
      text =
          "Generates leaves for the current lexicon. The minimum rack targets "
          "specify the required minimum number of rack occurrences for all "
          "racks before the next generation can start. The games before force "
          "draw argument specifies the number of 'normal' games to play before "
          "rare racks are 'forced' to occur so that they reach the minimum. "
          "The "
          "example shows the currently recommended values. Emperical testing "
          "has "
          "show that there is little improvement after the 5th generation. The "
          "files produced by each generation are labeled *_gen_<gen_number>, "
          "so "
          "if the command is stopped early, reinvoke it with using leaves of "
          "the "
          "last generation, otherwise the leavegen command will start over at "
          "the first generation. It is recommended to use the autoplay command "
          "with game pairs to evaluate the resulting leaves. Depending on your "
          "hardware, this command could take days or weeks.";
      break;
    case ARG_TOKEN_CREATE_DATA:
      usages[0] = "<type> <output_name> [<letter_distribution>]";
      examples[0] = "klv CSW50";
      examples[1] = "klv CSW50 english";
      text =
          "Creates a zeroed or empty data file of the specified type. If no "
          "letter distribution is specified, the current letter distribution "
          "is used. Currently, only the 'klv' type is supported.";
      break;
    case ARG_TOKEN_LOAD:
      usages[0] = "<source_identifier>";
      examples[0] = "54938";
      examples[1] = "https://cross-tables.com/annotated.php?u=54938";
      examples[2] = "XuoAntzD";
      examples[3] = "https://woogles.io/game/XuoAntzD";
      examples[4] =
          "https://www.cross-tables.com/annotated/selfgcg/556/anno55690.gcg";
      examples[5] = "testdata/gcgs/success_six_pass.gcg";
      examples[6] = "some_game.gcg";
      text =
          "Loads the game using the specified GCG. The source identifier can "
          "be a cross-tables ID or game URL, a woogles.io ID or game URL, a "
          "URL "
          "to the GCG file, or a local GCG file.";
      break;
    case ARG_TOKEN_NEW_GAME:
      usages[0] = "[<gcg_filename>]";
      examples[0] = "";
      examples[1] = "alice_vs_bob";
      examples[2] = "alice_vs_bob.gcg";
      text = "Starts a new game, resetting the previous game and game history. "
             "The GCG filename can be optionally specified.";
      break;
    case ARG_TOKEN_EXPORT:
      usages[0] = "[<output_gcg_filename>]";
      examples[0] = "";
      examples[1] = "my_game";
      examples[2] = "other_game.gcg";
      text = "Saves the current game history to the specified GCG file. If no "
             "GCG file is specified, a default name will be used. If no '.gcg' "
             "extension is specified, it will be added to the filename "
             "automatically.";
      break;
    case ARG_TOKEN_COMMIT:
      usages[0] = "<move_index>";
      usages[1] = "<position> <tiles>";
      usages[2] = "ex <exchanged_tiles> [<rack_after_exchange>]";
      usages[3] = "pass";
      examples[0] = "2";
      examples[1] = "11d ANTIQuES";
      examples[2] = "ex ABC";
      examples[3] = "ex ABC HIJKLMN";
      examples[4] = "pass";
      text =
          "Commits the move to the game history. When committing an exchange, "
          "the rack after the exchange can be specified so that six passes "
          "can be correctly scored.";
      break;
    case ARG_TOKEN_TOP_COMMIT:
      usages[0] = "[<rack>]";
      examples[0] = "";
      examples[1] = "RETINAS";
      text = "Commits the static best move if a rack is specified or no sim "
             "results are available. Otherwise, commits the best move by win "
             "percentage according to the sim results.";
      break;
    case ARG_TOKEN_CHALLENGE:
      usages[0] = "[<challenge_bonus_points>]";
      examples[0] = "";
      examples[1] = "7";
      text = "Adds a challenge bonus to the previous tile placement move. "
             "Challenges can be added mid-game without truncating the game "
             "history. If no challenge bonus is specified, the current value "
             "will be used.";
      break;
    case ARG_TOKEN_UNCHALLENGE:
      usages[0] = "";
      text =
          "Removes the challenge bonus from the previous tile placement move. "
          "Challenges can be removed mid-game without truncating the game "
          "history.";
      break;
    case ARG_TOKEN_OVERTIME:
      usages[0] = "<player_nickname> <overtime_penalty>";
      examples[0] = "josh 5";
      examples[1] = "josh 9";
      text =
          "Adds an overtime penalty for the given player. Overtime penalties "
          "can only be applied after the game is over.";
      break;
    case ARG_TOKEN_SWITCH_NAMES:
      usages[0] = "";
      text = "Switches the names of the players.";
      break;
    case ARG_TOKEN_SHOW_GAME:
      usages[0] = "";
      text = "Shows the current game.";
      break;
    case ARG_TOKEN_SHOW_MOVES:
      usages[0] = "";
      text = "Shows the moves or the sim results if available.";
      break;
    case ARG_TOKEN_SHOW_INFERENCE:
      usages[0] = "";
      text = "Shows the inference result.";
      break;
    case ARG_TOKEN_SHOW_ENDGAME:
      usages[0] = "";
      text = "Shows the endgame solver result.";
      break;
    case ARG_TOKEN_SHOW_HEAT_MAP:
      usages[0] = "<play_index> [<ply> <type>]";
      examples[0] = "10";
      examples[1] = "10 a";
      examples[2] = "10 b";
      examples[3] = "10 1";
      examples[4] = "10 3 all";
      examples[5] = "10 3 a";
      examples[6] = "10 4 bingo";
      examples[7] = "10 4 b";
      text = "Shows the heat map for the given play, ply, and type. If no ply "
             "is given, a default of 1 will be used. If no type is given, a "
             "default of 'all' will be used.";
      break;
    case ARG_TOKEN_NEXT:
      usages[0] = "";
      text = "Goes to the next move of the game if possible. Skips over "
             "challenge bonus events.";
      break;
    case ARG_TOKEN_PREVIOUS:
      usages[0] = "";
      text = "Goes to the previous move of the game if possible. Skips over "
             "challenge bonus events.";
      break;
    case ARG_TOKEN_GOTO:
      usages[0] = "<turn_number_or_end_or_start>";
      examples[0] = "1";
      examples[1] = "end";
      examples[2] = "start";
      text =
          "Goes to the game state after the specified turn number. Use 'start' "
          "and 'end' to go to the start and end of the game, respectively.";
      break;
    case ARG_TOKEN_NOTE:
      usages[0] = "<content_with_possible_whitespace>";
      examples[0] = "#knowledgesaddest";
      examples[1] = "this is a valid note with whitespace";
      text = "Specifies the note for the most recently added move. If there is "
             "an existing note it will be overwritten.";
      break;
    case ARG_TOKEN_P1_NAME:
      usages[0] = "<player_name_with_possible_whitespace>";
      examples[0] = "Bob";
      examples[1] = "Bob Smith";
      text = "Specifies the name of the first player.";
      break;
    case ARG_TOKEN_P2_NAME:
      usages[0] = "<player_name_with_possible_whitespace>";
      examples[0] = "Bob";
      examples[1] = "Bob Smith";
      text = "Specifies the name of the second player.";
      break;
    case ARG_TOKEN_DATA_PATH:
      usages[0] = "<data_paths>";
      examples[0] = "./data";
      examples[1] = "./data:./testdata:./other_dir";
      text =
          "Designates the data file directories ordered by search precedence. "
          "Directories listed earlier are preferred when locating files.";
      break;
    case ARG_TOKEN_BINGO_BONUS:
      usages[0] = "<bingo_bonus>";
      examples[0] = "50";
      examples[1] = "30";
      text = "Specifies the number of additional points plays receive when all "
             "tiles on the rack are played.";
      break;
    case ARG_TOKEN_CHALLENGE_BONUS:
      usages[0] = "<challenge_bonus>";
      examples[0] = "5";
      examples[1] = "10";
      text =
          "Specifies the number of additional points plays receive when they "
          "are unsuccessfully challenged.";
      break;
    case ARG_TOKEN_BOARD_LAYOUT:
      usages[0] = "<board_layout>";
      examples[0] = "standard15";
      examples[1] = "standard21";
      text = "Specifies the bonus square layout for the board.";
      break;
    case ARG_TOKEN_GAME_VARIANT:
      usages[0] = "<game_variant>";
      examples[0] = "classic";
      examples[1] = "wordsmog";
      text = "Specifies the game variant.";
      break;
    case ARG_TOKEN_LETTER_DISTRIBUTION:
      usages[0] = "<letter_distribution>";
      examples[0] = "english";
      examples[1] = "french";
      text = "Specifies the letter distribution.";
      break;
    case ARG_TOKEN_LEXICON:
      usages[0] = "<lexicon>";
      examples[0] = "CSW21";
      examples[1] = "NWL20";
      text = "Specifies the lexicon for both players, unless overridden by the "
             "'l1' or 'l2' options.";
      break;
    case ARG_TOKEN_USE_WMP:
      usages[0] = "<true_or_false>";
      examples[0] = "true";
      examples[1] = "false";
      text = "Specifies whether to use word maps as opposed to KWGs when "
             "generating moves. Word maps are much faster but use more memory "
             "and are on by default.";
      break;
    case ARG_TOKEN_LEAVES:
      usages[0] = "<leaves>";
      examples[0] = "CSW21";
      examples[1] = "TWL98";
      text = "Specifies the leaves for both players, unless overridden by the "
             "'k1' or 'k2' options.";
      break;
    case ARG_TOKEN_P1_LEXICON:
    case ARG_TOKEN_P2_LEXICON:
      usages[0] = "<lexicon>";
      examples[0] = "CSW21";
      examples[1] = "TWL98";
      text =
          "Specifies the lexicon for the given player. This can be used with "
          "the autoplay command to compare different lexicons.";
      break;
    case ARG_TOKEN_P1_USE_WMP:
    case ARG_TOKEN_P2_USE_WMP:
      usages[0] = "<true_or_false>";
      examples[0] = "true";
      examples[1] = "false";
      text = "Specifies whether to use word maps as opposed to KWGs when "
             "generating moves for the given player.";
      break;
    case ARG_TOKEN_P1_LEAVES:
    case ARG_TOKEN_P2_LEAVES:
      usages[0] = "<leaves>";
      examples[0] = "CSW21";
      examples[1] = "TWL98";
      text = "Specifies the leaves for the given player, This can can be used "
             "with the autoplay command to compare different leaves.";
      break;
    case ARG_TOKEN_P1_MOVE_SORT_TYPE:
    case ARG_TOKEN_P2_MOVE_SORT_TYPE:
      usages[0] = "<sort_type>";
      examples[0] = "score";
      examples[1] = "equity";
      text = "Specifies how the generated moves for the given player should be "
             "sorted.";
      break;
    case ARG_TOKEN_P1_MOVE_RECORD_TYPE:
    case ARG_TOKEN_P2_MOVE_RECORD_TYPE:
      usages[0] = "<record_type>";
      examples[0] = "best";
      examples[1] = "equity";
      examples[2] = "all";
      text = "Specifies how the generated moves for the given player should be "
             "recorded. The 'best' record type will only record the best move "
             "according the the sort type and is the fastest to compute. This "
             "option is ideal for autoplay. The 'all' record type will record "
             "all moves. The 'equity' record type will record all moves with X "
             "equity of the best move, where X is specified by the 'mmargin' "
             "option.";
      break;
    case ARG_TOKEN_WIN_PCT:
      usages[0] = "<win_percentage>";
      examples[0] = "winpct";
      text = "Specifies which win percentage file to use for simulations.";
      break;
    case ARG_TOKEN_PLIES:
      usages[0] = "<plies>";
      examples[0] = "2";
      examples[1] = "4";
      text = "Specifies the number of plies to use for simulations.";
      break;
    case ARG_TOKEN_ENDGAME_PLIES:
      usages[0] = "<endgame_plies>";
      examples[0] = "4";
      examples[1] = "8";
      text = "Specifies the number of plies to use for solving endgames.";
      break;
    case ARG_TOKEN_NUMBER_OF_PLAYS:
      usages[0] = "<number_of_plays>";
      examples[0] = "150";
      examples[1] = "300";
      text = "Specifies the number of plays generated by the move generation "
             "command.";
      break;
    case ARG_TOKEN_MAX_NUMBER_OF_DISPLAY_PLAYS:
      usages[0] = "<max_number_of_display_plays>";
      examples[0] = "15";
      examples[1] = "30";
      text = "Specifies the maximum number of plays to display.";
      break;
    case ARG_TOKEN_NUMBER_OF_SMALL_PLAYS:
      usages[0] = "<number_of_small_plays>";
      examples[0] = "15";
      examples[1] = "30";
      text = "Specifies the number of plays generated by the move generation "
             "command for endgames.";
      break;
    case ARG_TOKEN_MAX_ITERATIONS:
      usages[0] = "<max_iterations>";
      examples[0] = "100";
      examples[1] = "1000";
      text = "Specifies the maximum total number of iterations across all "
             "simulated plays to perform before stopping.";
      break;
    case ARG_TOKEN_STOP_COND_PCT:
      usages[0] = "<stop_cond_pct>";
      examples[0] = "95";
      examples[1] = "98.5";
      examples[2] = "99";
      examples[3] = "99.9999";
      text = "Specifies the statistical confidence level for the simulations, "
             "ranging from 0 to 100 exclusive. A higher confidence level "
             "improves the accuracy of the results, but takes longer to run.";
      break;
    case ARG_TOKEN_INFERENCE_MARGIN:
      usages[0] = "<inference_equity_margin>";
      examples[0] = "1";
      examples[1] = "2.4";
      examples[2] = "10.0";
      text =
          "Specifies the tolerance, in terms of equity, for how much worse a "
          "move can be than the best equity move and still be considered a "
          "possible rack from which the opponent played.";
      break;
    case ARG_TOKEN_MOVEGEN_MARGIN:
      usages[0] = "<movegen_equity_margin>";
      examples[0] = "1";
      examples[1] = "5.5";
      examples[2] = "10.0";
      text =
          "Specifies the tolerance, in terms of equity, for how much worse a "
          "move can be than the best equity move and still be generated by "
          "the move generation command.";
      break;
    case ARG_TOKEN_MIN_PLAY_ITERATIONS:
      usages[0] = "<min_play_iterations>";
      examples[0] = "100";
      examples[1] = "500";
      text = "Specifies the minimum number of iterations a candidate move must "
             "receive when running simulations.";
      break;
    case ARG_TOKEN_USE_GAME_PAIRS:
      usages[0] = "<true_or_false>";
      examples[0] = "true";
      examples[1] = "false";
      text =
          "Specifies whether or not to use game pairs when running the "
          "autoplay command. Using game pairs reduces statistical noise in "
          "the autoplay results by playing games in pairs using the same "
          "seed, with player one going first in one game and player two going "
          "first in the other game. Since the games are deterministic for a "
          "given starting seed, if both players make the exact same decision "
          "for each corresponding play, the games will be identical.";
      break;
    case ARG_TOKEN_USE_SMALL_PLAYS:
      usages[0] = "<true_or_false>";
      examples[0] = "true";
      examples[1] = "false";
      text =
          "Specifies whether or not to use the small move format for endgames.";
      break;
    case ARG_TOKEN_SIM_WITH_INFERENCE:
      usages[0] = "<true_or_false>";
      examples[0] = "true";
      examples[1] = "false";
      text = "Specifies whether or not to run and use the inference result "
             "when simulating.";
      break;
    case ARG_TOKEN_USE_HEAT_MAP:
      usages[0] = "<true_or_false>";
      examples[0] = "true";
      examples[1] = "false";
      text = "Specifies whether or not to save heat map data "
             "when simulating. Heat maps may be memory intensive for many "
             "plays or plies.";
      break;
    case ARG_TOKEN_WRITE_BUFFER_SIZE:
      usages[0] = "<write_buffer_size>";
      examples[0] = "10000";
      examples[1] = "100000";
      text =
          "Specifies the size of the write buffer for the autoplay recorder.";
      break;
    case ARG_TOKEN_HUMAN_READABLE:
      usages[0] = "<true_or_false>";
      examples[0] = "true";
      examples[1] = "false";
      text = "Specifies whether or not to use a human readable move format for "
             "printing results.";
      break;
    case ARG_TOKEN_RANDOM_SEED:
      usages[0] = "<random_seed>";
      examples[0] = "0";
      examples[1] = "42";
      text = "Specifies the random seed to use for any gameplay that uses RNG.";
      break;
    case ARG_TOKEN_NUMBER_OF_THREADS:
      usages[0] = "<number_of_threads>";
      examples[0] = "1";
      examples[1] = "4";
      text = "Specifies the number of threads to use when running commands.";
      break;
    case ARG_TOKEN_PRINT_INTERVAL:
      usages[0] = "<print_interval>";
      examples[0] = "100";
      examples[1] = "1000";
      text = "Specifies the iteration or game interval at which to print the "
             "current command status.";
      break;
    case ARG_TOKEN_EXEC_MODE:
      usages[0] = "<exec_mode>";
      examples[0] = "sync";
      examples[1] = "async";
      text =
          "Specifies the execution mode to use when running commands. Running "
          "in sync mode will block on the main thread until the command is "
          "complete. Running in async mode will run the command in the "
          "background allowing the user to query the status of the command or "
          "stop it at any time.";
      break;
    case ARG_TOKEN_TT_FRACTION_OF_MEM:
      usages[0] = "<fraction_of_mem>";
      examples[0] = "0.25";
      examples[1] = "0.5";
      text = "Specifies the fraction of memory to use for the transposition "
             "table.";
      break;
    case ARG_TOKEN_TIME_LIMIT:
      usages[0] = "<time_limit>";
      examples[0] = "10";
      examples[1] = "30";
      text = "Specifies the time limit in seconds for simulations.";
      break;
    case ARG_TOKEN_SAMPLING_RULE:
      usages[0] = "<sampling_rule>";
      examples[0] = "rr";
      examples[1] = "tt";
      text = "Specifies the sampling rule to use when running simulations. The "
             "rr option implements a round robin where every play receives the "
             "same "
             "number of iterations. The tt option implements the Top Two "
             "sampling rule which attempts to minimize the number of total "
             "iterations needed to find the best move.";
      break;
    case ARG_TOKEN_THRESHOLD:
      usages[0] = "<threshold>";
      examples[0] = "gk16";
      examples[1] = "none";
      text =
          "Specifies the threshold to use when running simulations. The gk16 "
          "option implements the GK16 threshold which attempts to minimize the "
          "number of total iterations needed to find the best move. The none "
          "option will make the simulation run until it hits the max total "
          "iterations or time limit.";
      break;
    case ARG_TOKEN_CUTOFF:
      usages[0] = "<cutoff>";
      examples[0] = "0.005";
      examples[1] = "0.01";
      text =
          "Specifies the cutoff threshold for determining when simmed plays "
          "are equivalent. If both win percentages are within cutoff of 100.0 "
          "or both are within cutoff of 0.0, they are considered equivalent "
          "and tiebroken by equity. The default is 0.005.";
      break;
    case ARG_TOKEN_PRINT_BOARDS:
      usages[0] = "<true_or_false>";
      examples[0] = "true";
      examples[1] = "false";
      text = "Specifies whether or not to print the boards for each play.";
      break;
    case ARG_TOKEN_BOARD_COLOR:
      usages[0] = "<board_color>";
      examples[0] = "none";
      examples[1] = "ansi";
      examples[2] = "xterm";
      examples[3] = "truecolour";
      text = "Specifies the color of the board.";
      break;
    case ARG_TOKEN_BOARD_TILE_GLYPHS:
      usages[0] = "<board_tile_glyphs>";
      examples[0] = "primary";
      examples[1] = "alt";
      text = "Specifies the glyphs to use for the board tiles.";
      break;
    case ARG_TOKEN_BOARD_BORDER:
      usages[0] = "<board_border>";
      examples[0] = "ascii";
      examples[1] = "box";
      text = "Specifies the border type to use for the board.";
      break;
    case ARG_TOKEN_BOARD_COLUMN_LABEL:
      usages[0] = "<board_column_label>";
      examples[0] = "ascii";
      examples[1] = "fullwidth";
      text = "Specifies the column label type to use for the board.";
      break;
    case ARG_TOKEN_ON_TURN_MARKER:
      usages[0] = "<on_turn_marker>";
      examples[0] = "ascii";
      examples[1] = "arrowhead";
      text = "Specifies the marker to use for the current player.";
      break;
    case ARG_TOKEN_ON_TURN_COLOR:
      usages[0] = "<on_turn_color>";
      examples[0] = "none";
      examples[1] = "green";
      text = "Specifies the color of the marker for the current player.";
      break;
    case ARG_TOKEN_ON_TURN_SCORE_STYLE:
      usages[0] = "<on_turn_score_style>";
      examples[0] = "normal";
      examples[1] = "bold";
      text = "Specifies the style of the score for the current player.";
      break;
    case ARG_TOKEN_PRETTY:
      usages[0] = "<true_or_false>";
      examples[0] = "true";
      examples[1] = "false";
      text =
          "Specifies whether or not to use a preset collection of options to "
          "pretty print the board.";
      break;
    case ARG_TOKEN_PRINT_ON_FINISH:
      usages[0] = "<true_or_false>";
      examples[0] = "true";
      examples[1] = "false";
      text = "Specifies whether or not to print a finished message when a "
             "command completes execution.";
      break;
    case ARG_TOKEN_SHOW_PROMPT:
      usages[0] = "<true_or_false>";
      examples[0] = "true";
      examples[1] = "false";
      text = "Specifies whether or not to show the '" MAGPIE_PROMPT "' prompt.";
      break;
    case ARG_TOKEN_SAVE_SETTINGS:
      usages[0] = "<true_or_false>";
      examples[0] = "true";
      examples[1] = "false";
      text = "Specifies whether or not to save the current settings to file "
             "after every command. If settings are saved, they will be "
             "automatically loaded on the next startup.";
      break;
    case ARG_TOKEN_AUTOSAVE_GCG:
      usages[0] = "<true_or_false>";
      examples[0] = "true";
      examples[1] = "false";
      text = "Specifies whether or not to automatically save the game history "
             "as a GCG after every change.";
      break;
    case NUMBER_OF_ARG_TOKENS:
      log_fatal("encountered invalid arg token in help command");
      break;
    }
  }
  const char *is_arg_char = "-";
  if (is_command) {
    is_arg_char = "";
  }

  if (is_hotkey) {
    string_builder_add_formatted_string(sb, "%s%c, %s%s", is_arg_char, name[0],
                                        is_arg_char, name);
  } else {
    string_builder_add_formatted_string(sb, "%s%s, %s%s", is_arg_char,
                                        shortest_unambiguous_name, is_arg_char,
                                        name);
  }

  string_builder_add_formatted_string(sb, "\n\n%*sUsage:\n\n", HELP_INDENT, "");

  int usages_index = 0;
  while (usages[usages_index]) {
    string_builder_add_formatted_string(sb, "%*s%s %s\n", HELP_INDENT + 2, "",
                                        name, usages[usages_index]);
    usages_index++;
  }

  if (examples[0]) {
    string_builder_add_formatted_string(sb, "\n%*sExamples:\n\n", HELP_INDENT,
                                        "");
    int example_index = 0;
    while (examples[example_index]) {
      string_builder_add_formatted_string(sb, "%*s%s %s\n", HELP_INDENT + 2, "",
                                          name, examples[example_index]);
      example_index++;
    }
  }

  string_builder_add_formatted_string(sb, "\n%*s%s\n\n", HELP_INDENT, "", text);
}

char *impl_help(Config *config, ErrorStack *error_stack) {
  const char *help_arg = config_get_parg_value(config, ARG_TOKEN_HELP, 0);
  arg_token_t help_arg_token = NUMBER_OF_ARG_TOKENS;
  if (help_arg) {
    help_arg_token = get_token_from_string(config, help_arg, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return empty_string();
    }
  }

  StringBuilder *sb = string_builder_create();
  if (help_arg_token == NUMBER_OF_ARG_TOKENS) {
    // Show the full help command
    // Show help for the commands first
    string_builder_add_string(sb, "Commands\n\n");
    for (arg_token_t k = 0; k < NUMBER_OF_ARG_TOKENS; k++) {
      if (config->pargs[k]->is_command) {
        add_help_arg_to_string_builder(config, k, sb, false);
      }
    }
    // Show help for async commands
    string_builder_add_string(sb, "\n\nAsync commands\n\n");
    for (async_token_t k = 0; k < NUMBER_OF_ASYNC_COMMAND_TOKENS; k++) {
      add_help_arg_to_string_builder(config, k, sb, true);
    }
    // Then show help for the settings
    string_builder_add_string(sb, "\n\nSettings\n\n");
    for (arg_token_t k = 0; k < NUMBER_OF_ARG_TOKENS; k++) {
      if (!config->pargs[k]->is_command) {
        add_help_arg_to_string_builder(config, k, sb, false);
      }
    }
  } else {
    add_help_arg_to_string_builder(config, help_arg_token, sb, false);
  }
  char *result = string_builder_dump(sb, NULL);
  string_builder_destroy(sb);
  return result;
}

void execute_help(Config *config, ErrorStack *error_stack) {
  char *result = impl_help(config, error_stack);
  if (error_stack_is_empty(error_stack)) {
    thread_control_print(config->thread_control, result);
  }
  free(result);
}

char *str_api_help(Config *config, ErrorStack *error_stack) {
  return impl_help(config, error_stack);
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
    config_reset_move_list_and_invalidate_sim_results(config);
  }
  string_builder_destroy(cgp_builder);
}

// Adding moves

char *impl_add_moves(Config *config, ErrorStack *error_stack) {
  if (!config_has_game_data(config)) {
    error_stack_push(error_stack, ERROR_STATUS_CONFIG_LOAD_GAME_DATA_MISSING,
                     string_duplicate("cannot add moves without lexicon"));
    return empty_string();
  }

  config_init_game(config);

  const char *moves = config_get_parg_value(config, ARG_TOKEN_MOVES, 0);

  int player_on_turn_index = game_get_player_on_turn_index(config->game);

  ValidatedMoves *new_validated_moves =
      validated_moves_create(config->game, player_on_turn_index, moves, true,
                             false, true, error_stack);
  char *phonies_str = NULL;
  if (error_stack_is_empty(error_stack)) {
    const LetterDistribution *ld = game_get_ld(config->game);
    const Board *board = game_get_board(config->game);
    int number_of_new_moves =
        validated_moves_get_number_of_moves(new_validated_moves);
    StringBuilder *phonies_sb = string_builder_create();
    string_builder_add_validated_moves_phonies(phonies_sb, new_validated_moves,
                                               ld, board);
    phonies_str = string_builder_dump(phonies_sb, NULL);
    string_builder_destroy(phonies_sb);
    config_init_move_list(config, number_of_new_moves);
    validated_moves_add_to_sorted_move_list(new_validated_moves,
                                            config->move_list);
  } else {
    phonies_str = empty_string();
  }

  validated_moves_destroy(new_validated_moves);

  return phonies_str;
}

// Setting player rack

void impl_set_rack_internal(Config *config, const char *rack_str,
                            ErrorStack *error_stack) {
  if (!config_has_game_data(config)) {
    error_stack_push(
        error_stack, ERROR_STATUS_CONFIG_LOAD_GAME_DATA_MISSING,
        string_duplicate("cannot set player rack without lexicon"));
    return;
  }

  config_init_game(config);

  const int player_on_turn_index = game_get_player_on_turn_index(config->game);

  Rack new_rack;
  rack_copy(&new_rack, player_get_rack(game_get_player(config->game,
                                                       player_on_turn_index)));

  load_rack_or_push_to_error_stack(rack_str, config->ld,
                                   ERROR_STATUS_CONFIG_LOAD_MALFORMED_RACK_ARG,
                                   &new_rack, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  // Return letters on opponent rack that may have been inferred
  // due to an empty bag but are not known from previous phonies.
  const int player_off_turn_index = 1 - player_on_turn_index;
  return_rack_to_bag(config->game, player_off_turn_index);
  if (!draw_rack_from_bag(config->game, player_off_turn_index,
                          player_get_known_rack_from_phonies(game_get_player(
                              config->game, player_off_turn_index)))) {
    log_fatal("unexpectedly failed to draw known letters from phonies for "
              "opponent from the bag in set rack command");
  }

  if (rack_is_drawable(config->game, player_on_turn_index, &new_rack)) {
    return_rack_to_bag(config->game, player_on_turn_index);
    if (!draw_rack_from_bag(config->game, player_on_turn_index, &new_rack)) {
      log_fatal(
          "unexpectedly failed to draw rack from the bag in set rack command");
    }
    if (bag_get_letters(game_get_bag(config->game)) <= (RACK_SIZE)) {
      draw_to_full_rack(config->game, 1 - player_on_turn_index);
    }
  } else {
    error_stack_push(error_stack, ERROR_STATUS_CONFIG_LOAD_RACK_NOT_IN_BAG,
                     get_formatted_string(
                         "rack '%s' is not available in the bag", rack_str));
  }
  config_reset_move_list_and_invalidate_sim_results(config);
}

void impl_set_rack(Config *config, const arg_token_t arg_token,
                   ErrorStack *error_stack) {
  const char *rack_str = config_get_parg_value(config, arg_token, 0);
  impl_set_rack_internal(config, rack_str, error_stack);
}

void impl_set_random_rack(Config *config, ErrorStack *error_stack) {
  if (!config_has_game_data(config)) {
    error_stack_push(
        error_stack, ERROR_STATUS_CONFIG_LOAD_GAME_DATA_MISSING,
        string_duplicate("cannot set player rack without lexicon"));
    return;
  }
  config_init_game(config);
  const int player_index = game_get_player_on_turn_index(config->game);
  return_rack_to_bag(config->game, player_index);
  draw_to_full_rack(config->game, player_index);
  if (bag_get_letters(game_get_bag(config->game)) <= (RACK_SIZE)) {
    draw_to_full_rack(config->game, 1 - player_index);
  }
  config_reset_move_list_and_invalidate_sim_results(config);
}

// Move generation

void impl_move_gen_override_record_type(Config *config,
                                        move_record_t move_record_type) {
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
      .target_equity = EQUITY_MAX_VALUE,
      .target_leave_size_for_exchange_cutoff = UNSET_LEAVE_SIZE,
  };
  generate_moves_for_game_override_record_type(&args, move_record_type);
  move_list_sort_moves(config->move_list);
  sim_results_set_valid_for_current_game_state(config->sim_results, false);
}

void impl_move_gen(Config *config, ErrorStack *error_stack) {
  if (!config_has_game_data(config)) {
    error_stack_push(error_stack, ERROR_STATUS_CONFIG_LOAD_GAME_DATA_MISSING,
                     string_duplicate("cannot generate moves without lexicon"));
    return;
  }

  config_init_game(config);

  impl_move_gen_override_record_type(
      config, player_get_move_record_type(game_get_player(
                  config->game, game_get_player_on_turn_index(config->game))));
}

// Inference

void config_fill_infer_args(const Config *config, bool use_game_history,
                            int target_index, Equity target_score,
                            int target_num_exch, Rack *target_played_tiles,
                            bool use_infer_cutoff_optimization,
                            Rack *target_known_rack, Rack *nontarget_known_rack,
                            InferenceArgs *args) {
  infer_args_fill(args, config->num_plays, config->eq_margin_inference,
                  config->game_history, config->game, config->num_threads,
                  config->print_interval, config->thread_control,
                  use_game_history, use_infer_cutoff_optimization, target_index,
                  target_score, target_num_exch, target_played_tiles,
                  target_known_rack, nontarget_known_rack);
}

// Use target_index < 0 to infer using the game history
void config_infer(const Config *config, bool use_game_history, int target_index,
                  Equity target_score, int target_num_exch,
                  Rack *target_played_tiles, Rack *target_known_rack,
                  Rack *nontarget_known_rack,
                  bool use_inference_cutoff_optimization,
                  InferenceResults *results, ErrorStack *error_stack) {
  InferenceArgs args;
  config_fill_infer_args(config, use_game_history, target_index, target_score,
                         target_num_exch, target_played_tiles,
                         use_inference_cutoff_optimization, target_known_rack,
                         nontarget_known_rack, &args);
  infer_without_ctx(&args, results, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
  inference_results_set_valid_for_current_game_state(config->inference_results,
                                                     true);
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
                 &target_known_rack, &nontarget_known_rack, true,
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
      if (strings_iequal(
              game_history_player_get_nickname(config->game_history, i),
              target_name_or_index_str)) {
        target_index = i;
        break;
      }
    }
    if (target_index == -1) {
      error_stack_push(error_stack,
                       ERROR_STATUS_CONFIG_LOAD_MALFORMED_PLAYER_NAME,
                       get_formatted_string("unrecognized player nickname '%s'",
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
    if (!strings_equal(target_known_rack_str, "-")) {
      load_rack_or_push_to_error_stack(
          target_known_rack_str, ld,
          ERROR_STATUS_CONFIG_LOAD_MALFORMED_RACK_ARG, &target_known_rack,
          error_stack);
      if (!error_stack_is_empty(error_stack)) {
        return;
      }
    }
    next_arg_index++;
  }

  const char *nontarget_known_rack_str =
      config_get_parg_value(config, ARG_TOKEN_INFER, next_arg_index);
  if (nontarget_known_rack_str &&
      !strings_equal(nontarget_known_rack_str, "-")) {
    load_rack_or_push_to_error_stack(
        nontarget_known_rack_str, ld,
        ERROR_STATUS_CONFIG_LOAD_MALFORMED_RACK_ARG, &nontarget_known_rack,
        error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
  }

  config_infer(config, false, target_index, target_score, target_num_exch,
               &target_played_tiles, &target_known_rack, &nontarget_known_rack,
               true, config->inference_results, error_stack);
}

// Sim

void config_fill_sim_args(const Config *config, Rack *known_opp_rack,
                          Rack *target_played_tiles,
                          Rack *nontarget_known_tiles,
                          Rack *target_known_inference_tiles,
                          SimArgs *sim_args) {
  InferenceArgs inference_args;
  if (config->sim_with_inference) {
    // FIXME: enable sim inferences using data from the last play instead of
    // the whole history so that autoplay does not have to keep a whole
    // history and play to turn for each inference which will probably incur
    // more overhead than we would like.
    config_fill_infer_args(config, true, 0, 0, 0, target_played_tiles, false,
                           target_known_inference_tiles, nontarget_known_tiles,
                           &inference_args);
  }
  sim_args_fill(
      config->plies, config->move_list, known_opp_rack, config->win_pcts,
      config->inference_results, config->thread_control, config->game,
      config->sim_with_inference, config->use_heat_map, config->num_threads,
      config->print_interval, config->max_num_display_plays, config->seed,
      config->max_iterations, config->min_play_iterations,
      config->stop_cond_pct, config->threshold, (int)config->time_limit_seconds,
      config->sampling_rule, config->cutoff, &inference_args, sim_args);
}

void config_simulate(Config *config, SimCtx **sim_ctx, Rack *known_opp_rack,
                     SimResults *sim_results, ErrorStack *error_stack) {
  // Lazy load win_pcts if not already loaded
  if (config->win_pcts == NULL) {
    const char *win_pct_name =
        config_get_parg_value(config, ARG_TOKEN_WIN_PCT, 0);
    if (win_pct_name == NULL) {
      win_pct_name = DEFAULT_WIN_PCT;
    }
    config->win_pcts =
        win_pct_create(config->data_paths, win_pct_name, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      error_stack_push(
          error_stack, ERROR_STATUS_CONFIG_LOAD_WIN_PCT_ERROR,
          string_duplicate(
              "encountered an error loading the win percentage file"));
      return;
    }
  }

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
  if (sim_ctx) {
    simulate(&args, sim_ctx, sim_results, error_stack);
  } else {
    simulate_without_ctx(&args, sim_results, error_stack);
  }
}

void impl_sim(Config *config, const arg_token_t known_opp_rack_arg_token,
              const int known_opp_rack_str_index, ErrorStack *error_stack) {
  if (!config_has_game_data(config)) {
    error_stack_push(error_stack, ERROR_STATUS_CONFIG_LOAD_GAME_DATA_MISSING,
                     string_duplicate("cannot simulate without lexicon"));
    return;
  }

  config_init_game(config);

  const int ld_size = ld_get_size(game_get_ld(config->game));
  const char *known_opp_rack_str = config_get_parg_value(
      config, known_opp_rack_arg_token, known_opp_rack_str_index);
  Rack known_opp_rack;
  rack_set_dist_size_and_reset(&known_opp_rack, ld_size);

  // Set setting empty opp rack with '-'
  if (known_opp_rack_str && !strings_equal(known_opp_rack_str, "-")) {
    load_rack_or_push_to_error_stack(
        known_opp_rack_str, game_get_ld(config->game),
        ERROR_STATUS_CONFIG_LOAD_MALFORMED_RACK_ARG, &known_opp_rack,
        error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
  } else if (!known_opp_rack_str) {
    // If no input rack is specified, default to simming with the opponent's
    // currently known rack
    rack_copy(
        &known_opp_rack,
        player_get_rack(game_get_player(
            config->game, 1 - game_get_player_on_turn_index(config->game))));
  }
  config_simulate(config, NULL, &known_opp_rack, config->sim_results,
                  error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
  bool sim_results_valid = true;
  if (config->sim_with_inference) {
    // This will always set the state to false if it was interrupted
    inference_results_set_valid_for_current_game_state(
        config->inference_results, true);
    // If the inference was interrupted, the sim results will be invalid
    sim_results_valid = inference_results_get_valid_for_current_game_state(
        config->inference_results);
  }
  sim_results_set_valid_for_current_game_state(config->sim_results,
                                               sim_results_valid);
}

char *status_sim(Config *config) {
  SimResults *sim_results = config->sim_results;
  if (!sim_results) {
    return string_duplicate("simmer has not been initialized");
  }
  return sim_results_get_string(config->game, sim_results,
                                config->max_num_display_plays,
                                !config->human_readable);
}

// Gen and Sim

void impl_gen_and_sim(Config *config, ErrorStack *error_stack) {
  impl_move_gen(config, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
  impl_sim(config, ARG_TOKEN_SIM, 0, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
}

char *status_gen_and_sim(Config *config) { return status_sim(config); }

// Rack and gen and sim

void impl_rack_and_gen_and_sim(Config *config, ErrorStack *error_stack) {
  impl_set_rack(config, ARG_TOKEN_RACK_AND_GEN_AND_SIM, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
  impl_move_gen(config, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
  impl_sim(config, ARG_TOKEN_RACK_AND_GEN_AND_SIM, 1, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
}

char *status_rack_and_gen_and_sim(Config *config) { return status_sim(config); }

// Endgame

void config_fill_endgame_args(Config *config, EndgameArgs *endgame_args) {
  endgame_args->thread_control = config->thread_control;
  endgame_args->game = config->game;
  endgame_args->plies = config->endgame_plies;
  endgame_args->tt_fraction_of_mem = config->tt_fraction_of_mem;
  endgame_args->initial_small_move_arena_size =
      DEFAULT_INITIAL_SMALL_MOVE_ARENA_SIZE;
  endgame_args->num_threads = config->num_threads;
  endgame_args->per_ply_callback = NULL;
  endgame_args->per_ply_callback_data = NULL;
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

char *status_endgame(Config *config) {
  if (!config->endgame_results) {
    return string_duplicate("endgame results are not yet initialized.\n");
  }
  if (!endgame_results_get_valid_for_current_game_state(
          config->endgame_results)) {
    return get_formatted_string("endgame results are not yet initialized for "
                                "the current game state.\n");
  }
  return endgame_results_get_string(config->endgame_results, config->game,
                                    config->game_history, true);
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
  autoplay_args->thread_control = config_get_thread_control(config);
  autoplay_args->data_paths = config_get_data_paths(config);
  autoplay_args->game_string_options = config->game_string_options;
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

char *status_autoplay(Config *config) {
  AutoplayResults *autoplay_results = config->autoplay_results;
  if (!autoplay_results) {
    return string_duplicate("autoplay has not been initialized");
  }
  return autoplay_results_get_status(autoplay_results);
}

// Conversion

void config_fill_conversion_args(const Config *config, ConversionArgs *args) {
  args->conversion_type_string =
      config_get_parg_value(config, ARG_TOKEN_CONVERT, 0);
  args->data_paths = config_get_data_paths(config);
  args->input_and_output_name =
      config_get_parg_value(config, ARG_TOKEN_CONVERT, 1);
  args->ld_name = config_get_parg_value(config, ARG_TOKEN_CONVERT, 2);
  args->num_threads = config_get_num_threads(config);
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

    if (!ld) {
      error_stack_push(
          error_stack, ERROR_STATUS_CREATE_DATA_MISSING_LETTER_DISTRIBUTION,
          get_formatted_string("cannot create %s without letter distribution",
                               create_type_str));
      return;
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

char *impl_show_game(Config *config, ErrorStack *error_stack) {
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
                          config->game_history, game_string);

  // Get the string and destroy the builder
  char *result = string_builder_dump(game_string, NULL);
  string_builder_destroy(game_string);

  return result;
}

void execute_show_game(Config *config, ErrorStack *error_stack) {
  char *result = impl_show_game(config, error_stack);
  if (error_stack_is_empty(error_stack)) {
    thread_control_print(config->thread_control, result);
  }
  free(result);
}

char *str_api_show_game(Config *config, ErrorStack *error_stack) {
  return impl_show_game(config, error_stack);
}

// Show moves

char *impl_show_moves_or_sim_results(Config *config, ErrorStack *error_stack) {
  if (!config_has_game_data(config)) {
    error_stack_push(error_stack, ERROR_STATUS_CONFIG_LOAD_GAME_DATA_MISSING,
                     string_duplicate("cannot show game without lexicon"));
    return empty_string();
  }
  if (!config->game || !config->move_list ||
      move_list_get_count(config->move_list) == 0) {
    error_stack_push(error_stack, ERROR_STATUS_NO_MOVES_TO_SHOW,
                     string_duplicate("no moves to show"));
    return empty_string();
  }
  const char *max_num_display_plays_str =
      config_get_parg_value(config, ARG_TOKEN_SHOW_MOVES, 0);

  int max_num_display_plays = config->max_num_display_plays;
  if (max_num_display_plays_str) {
    string_to_int_or_push_error("max num display plays",
                                max_num_display_plays_str, 1, INT_MAX,
                                ERROR_STATUS_CONFIG_LOAD_INT_ARG_OUT_OF_BOUNDS,
                                &max_num_display_plays, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return empty_string();
    }
  }

  char *result = NULL;
  if (sim_results_get_valid_for_current_game_state(config->sim_results)) {
    result =
        sim_results_get_string(config->game, config->sim_results,
                               max_num_display_plays, !config->human_readable);
  } else {
    result = move_list_get_string(
        config->move_list, game_get_board(config->game), config->ld,
        max_num_display_plays, !config->human_readable);
  }
  return result;
}

void execute_show_moves_or_sim_results(Config *config,
                                       ErrorStack *error_stack) {
  char *result = impl_show_moves_or_sim_results(config, error_stack);
  if (error_stack_is_empty(error_stack)) {
    thread_control_print(config->thread_control, result);
  }
  free(result);
}

char *str_api_show_moves_or_sim_results(Config *config,
                                        ErrorStack *error_stack) {
  return impl_show_moves_or_sim_results(config, error_stack);
}

// Show inference results

char *impl_show_inference(Config *config, ErrorStack *error_stack) {
  if (!config->game || !inference_results_get_valid_for_current_game_state(
                           config->inference_results)) {
    error_stack_push(error_stack, ERROR_STATUS_NO_INFERENCE_TO_SHOW,
                     string_duplicate("no inference results to show"));
    return empty_string();
  }

  const char *max_num_display_leaves_str =
      config_get_parg_value(config, ARG_TOKEN_SHOW_INFERENCE, 0);

  int max_num_display_leaves = config->max_num_display_plays;
  if (max_num_display_leaves_str) {
    string_to_int_or_push_error("max num display leaves",
                                max_num_display_leaves_str, 1, INT_MAX,
                                ERROR_STATUS_CONFIG_LOAD_INT_ARG_OUT_OF_BOUNDS,
                                &max_num_display_leaves, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return empty_string();
    }
  }

  return inference_result_get_string(config->inference_results, config->ld,
                                     max_num_display_leaves,
                                     !config->human_readable);
}

void execute_show_inference(Config *config, ErrorStack *error_stack) {
  char *result = impl_show_inference(config, error_stack);
  if (error_stack_is_empty(error_stack)) {
    thread_control_print(config->thread_control, result);
  }
  free(result);
}

char *str_api_show_inference(Config *config, ErrorStack *error_stack) {
  return impl_show_inference(config, error_stack);
}

// Show endgame

char *impl_show_endgame(const Config *config, ErrorStack *error_stack) {
  if (!config->game || !endgame_results_get_valid_for_current_game_state(
                           config->endgame_results)) {
    error_stack_push(error_stack, ERROR_STATUS_NO_ENDGAME_TO_SHOW,
                     string_duplicate("no endgame results to show"));
    return empty_string();
  }
  return endgame_results_get_string(config->endgame_results, config->game,
                                    config->game_history,
                                    config->human_readable);
}

void execute_show_endgame(Config *config, ErrorStack *error_stack) {
  char *result = impl_show_endgame(config, error_stack);
  if (error_stack_is_empty(error_stack)) {
    thread_control_print(config->thread_control, result);
  }
  free(result);
}

char *str_api_show_endgame(Config *config, ErrorStack *error_stack) {
  return impl_show_endgame(config, error_stack);
}

// Show heat map

char *impl_show_heat_map(const Config *config, ErrorStack *error_stack) {
  if (!config->game ||
      !sim_results_get_valid_for_current_game_state(config->sim_results)) {
    error_stack_push(error_stack, ERROR_STATUS_NO_HEAT_MAP_TO_SHOW,
                     string_duplicate("no heat map to show"));
    return empty_string();
  }

  const int num_plays = sim_results_get_number_of_plays(config->sim_results);
  const int num_plies = sim_results_get_num_plies(config->sim_results);
  const char *play_index_by_win_pct_str =
      config_get_parg_value(config, ARG_TOKEN_SHOW_HEAT_MAP, 0);

  int play_index_by_win_pct;
  string_to_int_or_push_error("move index", play_index_by_win_pct_str, 1,
                              num_plays,
                              ERROR_STATUS_HEAT_MAP_MOVE_INDEX_OUT_OF_RANGE,
                              &play_index_by_win_pct, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return empty_string();
  }
  // Convert from 1-indexed user input to 0-indexed internal
  play_index_by_win_pct--;

  const char *ply_index_str = NULL;
  const char *type_str = NULL;

  const char *pos_arg_2 =
      config_get_parg_value(config, ARG_TOKEN_SHOW_HEAT_MAP, 1);
  const char *pos_arg_3 =
      config_get_parg_value(config, ARG_TOKEN_SHOW_HEAT_MAP, 2);

  if (pos_arg_2) {
    if (is_all_digits_or_empty(pos_arg_2)) {
      ply_index_str = pos_arg_2;
    } else {
      type_str = pos_arg_2;
    }
    if (pos_arg_3) {
      if (type_str) {
        error_stack_push(error_stack, ERROR_STATUS_EXTRANEOUS_HEAT_MAP_ARG,
                         string_duplicate("extraneous heat map argument"));
        return empty_string();
      }
      type_str = pos_arg_3;
    }
  }

  int ply_index = 0;

  if (ply_index_str) {
    string_to_int_or_push_error("ply index", ply_index_str, 1, num_plies,
                                ERROR_STATUS_HEAT_MAP_PLY_INDEX_OUT_OF_RANGE,
                                &ply_index, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return empty_string();
    }
    // Convert from 1-indexed user input to 0-indexed internal
    ply_index--;
  }

  // The heat maps are only stored in the actual simmed plays and not the
  // display simmed plays to save space. Therefore, we need to get the actual
  // simmed play from the display simmed play by using the play index by sort
  // type, which is the index of the play in the sim results before it was
  // sorted by win pct.
  const SimmedPlay *display_simmed_play = sim_results_get_display_simmed_play(
      config->sim_results, play_index_by_win_pct);
  const HeatMap *heat_map = simmed_play_get_heat_map(
      sim_results_get_simmed_play(
          config->sim_results,
          simmed_play_get_play_index_by_sort_type(display_simmed_play)),
      ply_index);

  if (!heat_map) {
    error_stack_push(error_stack, ERROR_STATUS_NO_HEAT_MAP_TO_SHOW,
                     string_duplicate("no heat map to show"));
    return empty_string();
  }

  heat_map_t heat_map_type = HEAT_MAP_TYPE_ALL;
  if (type_str) {
    if (has_iprefix(type_str, "all")) {
      heat_map_type = HEAT_MAP_TYPE_ALL;
    } else if (has_iprefix(type_str, "bingos")) {
      heat_map_type = HEAT_MAP_TYPE_BINGO;
    } else {
      error_stack_push(
          error_stack, ERROR_STATUS_HEAT_MAP_UNRECOGNIZED_TYPE,
          get_formatted_string("unrecognized heat map type '%s'", type_str));
      return empty_string();
    }
  }

  char *result = NULL;
  StringBuilder *hm_string = string_builder_create();

  string_builder_add_simmed_play_ply_counts(
      hm_string, game_get_board(config->game), config->ld, display_simmed_play,
      ply_index);

  if (config->game_string_options->board_color ==
      GAME_STRING_BOARD_COLOR_NONE) {
    string_builder_add_heat_map(hm_string, heat_map, heat_map_type,
                                config->max_num_display_plays);
  } else {
    Game *game_dupe = game_duplicate(config->game);
    Move move;
    move_copy(&move, simmed_play_get_move(sim_results_get_display_simmed_play(
                         config->sim_results, play_index_by_win_pct)));
    const int play_on_turn_index = game_get_player_on_turn_index(game_dupe);
    Rack leave;
    get_leave_for_move(&move, game_dupe, &leave);
    play_move_without_drawing_tiles(&move, game_dupe);
    return_rack_to_bag(game_dupe, play_on_turn_index);
    draw_rack_from_bag(game_dupe, play_on_turn_index, &leave);
    string_builder_add_game_with_heat_map(
        game_dupe, NULL, config->game_string_options, config->game_history,
        heat_map, heat_map_type, hm_string);
    game_destroy(game_dupe);
  }
  result = string_builder_dump(hm_string, NULL);
  string_builder_destroy(hm_string);

  return result;
}

void execute_show_heat_map(Config *config, ErrorStack *error_stack) {
  char *result = impl_show_heat_map(config, error_stack);
  if (error_stack_is_empty(error_stack)) {
    thread_control_print(config->thread_control, result);
  }
  free(result);
}

char *str_api_show_heat_map(Config *config, ErrorStack *error_stack) {
  return impl_show_heat_map(config, error_stack);
}

// Export GCG

void impl_export_internal(Config *config, ErrorStack *error_stack,
                          const char *filename) {
  // Set a default filename if none exists
  if (filename || !game_history_get_gcg_filename(config->game_history)) {
    game_history_set_gcg_filename(config->game_history, filename);
  }

  write_gcg(game_history_get_gcg_filename(config->game_history), config->ld,
            config->game_history, error_stack);
}

// Writes the current game history to a GCG file if autosave is enabled
// and the game history has a filename.
void impl_export_if_autosave(Config *config, ErrorStack *error_stack) {
  if (config->autosave_gcg &&
      game_history_get_gcg_filename(config->game_history)) {
    impl_export_internal(config, error_stack, NULL);
  }
}

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

  impl_export_internal(config, error_stack,
                       config_get_parg_value(config, ARG_TOKEN_EXPORT, 0));

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
    game_history_player_reset_last_rack(config->game_history, player_index);
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
  const char *user_provided_filename =
      config_get_parg_value(config, ARG_TOKEN_NEW_GAME, 0);
  if (user_provided_filename) {
    game_history_set_gcg_filename(config->game_history, user_provided_filename);
  }
  impl_export_if_autosave(config, error_stack);
  return empty_string();
}

void execute_new_game(Config *config, ErrorStack *error_stack) {
  char *result = impl_new_game(config, error_stack);
  if (error_stack_is_empty(error_stack)) {
    execute_show_game(config, error_stack);
    thread_control_print(config->thread_control, result);
  }
  free(result);
}

char *str_api_new_game(Config *config, ErrorStack *error_stack) {
  return impl_new_game(config, error_stack);
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

void config_game_play_events_internal(Config *config, ErrorStack *error_stack) {
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
                    "rack to draw before game end pass out "
                    "'%s' for player '%s'"
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

// Runs game_play_n_events and then calculates if
// - the consecutive pass game end procedures should be applied
// - the game history needs to wait for the final pass/challenge from the user
// If there are no resulting errors, the move list is reset if it exists
void config_game_play_events(Config *config, ErrorStack *error_stack) {
  config_game_play_events_internal(config, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
  // Invalidate the moves when moving to a new position
  config_reset_move_list_and_invalidate_sim_results(config);
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
       game_event_type != GAME_EVENT_CHALLENGE_BONUS &&
       game_event_type != GAME_EVENT_PHONY_TILES_RETURNED)) {
    error_stack_push(error_stack,
                     ERROR_STATUS_COMMIT_WAITING_FOR_PASS_OR_CHALLENGE_BONUS,
                     string_duplicate("waiting for final pass or challenge but "
                                      "got a different game event"));
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
    // Challenge bonus events are handled elsewhere by inserting, not
    // appending to the history
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
    // End rack penalty events are never submitted directly by the user and
    // are handled elsewhere
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
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
  }

  config_game_play_events(config, error_stack);

  if (!error_stack_is_empty(error_stack)) {
    return;
  }
}

void parse_commit(Config *config, StringBuilder *move_string_builder,
                  ValidatedMoves **vms, ErrorStack *error_stack,
                  const char *commit_pos_arg_1, const char *commit_pos_arg_2,
                  const char *commit_pos_arg_3) {
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
          get_formatted_string("extraneous argument '%s' provided when "
                               "committing move by index",
                               commit_pos_arg_3));
      return;
    }
    int num_moves = 0;
    if (config->move_list) {
      num_moves = move_list_get_count(config->move_list);
    }
    if (num_moves == 0) {
      error_stack_push(
          error_stack, ERROR_STATUS_COMMIT_MOVE_INDEX_OUT_OF_RANGE,
          get_formatted_string(
              "cannot commit move with index '%s' with no generated moves",
              commit_pos_arg_1));
      return;
    }
    // If no second arg is provided and the first arg is all digits, the
    // digits represent the rank of the move to commit.

    int commit_move_index;
    string_to_int_or_push_error("move index", commit_pos_arg_1, 1, num_moves,
                                ERROR_STATUS_COMMIT_MOVE_INDEX_OUT_OF_RANGE,
                                &commit_move_index, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
    // Convert from 1-indexed user input to 0-indexed internal representation
    commit_move_index--;
    // If there are valid sim results, prefer to use them for the move index
    // lookup.
    if (sim_results_get_valid_for_current_game_state(config->sim_results)) {
      const SimResults *sim_results = config->sim_results;
      const int num_simmed_plays = sim_results_get_number_of_plays(sim_results);
      if (num_simmed_plays != num_moves) {
        log_fatal("encountered unexpected discrepancy between number of "
                  "generated plays (%d) and the number of simmed plays (%d)\n",
                  num_moves, num_simmed_plays);
      }
      move_copy(&move, simmed_play_get_move(sim_results_get_display_simmed_play(
                           config->sim_results, commit_move_index)));
    } else {
      move_copy(&move,
                move_list_get_move(config->move_list, commit_move_index));
    }

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

char *impl_commit_with_pos_args(Config *config, ErrorStack *error_stack,
                                const char *commit_pos_arg_1,
                                const char *commit_pos_arg_2,
                                const char *commit_pos_arg_3) {
  if (!config_has_game_data(config)) {
    error_stack_push(error_stack, ERROR_STATUS_CONFIG_LOAD_GAME_DATA_MISSING,
                     string_duplicate("cannot commit a move without lexicon"));
    return empty_string();
  }

  config_init_game(config);

  if (!game_history_get_waiting_for_final_pass_or_challenge(
          config->game_history) &&
      game_get_game_end_reason(config->game) != GAME_END_REASON_NONE) {
    error_stack_push(error_stack, ERROR_STATUS_COMMIT_GAME_OVER,
                     string_duplicate("cannot commit a move in a game that has "
                                      "already ended"));
    return empty_string();
  }

  StringBuilder *sb = string_builder_create();
  ValidatedMoves *vms = NULL;

  config_backup_game_and_history(config);

  const bool bag_empty_before_commit = bag_is_empty(game_get_bag(config->game));
  const int noncommit_player_index =
      1 - game_get_player_on_turn_index(config->game);
  Rack noncommit_player_rack;
  rack_copy(&noncommit_player_rack, player_get_rack(game_get_player(
                                        config->game, noncommit_player_index)));

  Board *precommit_board_copy = board_duplicate(game_get_board(config->game));

  parse_commit(config, sb, &vms, error_stack, commit_pos_arg_1,
               commit_pos_arg_2, commit_pos_arg_3);

  char *return_str = NULL;
  if (error_stack_is_empty(error_stack)) {
    string_builder_clear(sb);
    string_builder_add_validated_moves_phonies(sb, vms, config->ld,
                                               precommit_board_copy);
    return_str = string_builder_dump(sb, NULL);
  } else {
    return_str = empty_string();
  }

  board_destroy(precommit_board_copy);
  string_builder_destroy(sb);
  validated_moves_destroy(vms);

  if (!error_stack_is_empty(error_stack)) {
    config_restore_game_and_history(config);
    return return_str;
  }

  if (bag_empty_before_commit) {
    return_rack_to_bag(config->game, 0);
    return_rack_to_bag(config->game, 1);
    draw_rack_from_bag(config->game, noncommit_player_index,
                       &noncommit_player_rack);
    draw_to_full_rack(config->game, 1 - noncommit_player_index);
  }

  impl_export_if_autosave(config, error_stack);

  if (!error_stack_is_empty(error_stack)) {
    config_restore_game_and_history(config);
    return return_str;
  }

  return return_str;
}

char *impl_commit(Config *config, ErrorStack *error_stack) {
  const char *commit_pos_arg_1 =
      config_get_parg_value(config, ARG_TOKEN_COMMIT, 0);
  const char *commit_pos_arg_2 =
      config_get_parg_value(config, ARG_TOKEN_COMMIT, 1);
  const char *commit_pos_arg_3 =
      config_get_parg_value(config, ARG_TOKEN_COMMIT, 2);
  return impl_commit_with_pos_args(config, error_stack, commit_pos_arg_1,
                                   commit_pos_arg_2, commit_pos_arg_3);
}

void execute_commit(Config *config, ErrorStack *error_stack) {
  char *result = impl_commit(config, error_stack);
  if (error_stack_is_empty(error_stack)) {
    execute_show_game(config, error_stack);
    thread_control_print(config->thread_control, result);
  }
  free(result);
}

char *str_api_commit(Config *config, ErrorStack *error_stack) {
  return impl_commit(config, error_stack);
}

// Top commit

char *impl_top_commit(Config *config, ErrorStack *error_stack) {
  if (!config_has_game_data(config)) {
    error_stack_push(error_stack, ERROR_STATUS_CONFIG_LOAD_GAME_DATA_MISSING,
                     string_duplicate("cannot generate moves without lexicon"));
    return empty_string();
  }

  config_init_game(config);

  const char *rack_str = config_get_parg_value(config, ARG_TOKEN_TOP_COMMIT, 0);
  if (rack_str) {
    impl_set_rack_internal(config, rack_str, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return empty_string();
    }
    impl_move_gen_override_record_type(config, MOVE_RECORD_BEST);
  } else if (!config->move_list ||
             move_list_get_count(config->move_list) == 0) {
    const Rack *player_rack = player_get_rack(game_get_player(
        config->game, game_get_player_on_turn_index(config->game)));
    if (rack_is_empty(player_rack)) {
      error_stack_push(
          error_stack, ERROR_STATUS_COMMIT_EMPTY_RACK,
          string_duplicate("cannot generate best play with an empty rack"));
      return empty_string();
    }
    impl_move_gen_override_record_type(config, MOVE_RECORD_BEST);
  }
  return impl_commit_with_pos_args(config, error_stack, "1", NULL, NULL);
}

void execute_top_commit(Config *config, ErrorStack *error_stack) {
  char *result = impl_top_commit(config, error_stack);
  if (error_stack_is_empty(error_stack)) {
    thread_control_print(config->thread_control, result);
    execute_show_game(config, error_stack);
  }
  free(result);
}

char *str_api_top_commit(Config *config, ErrorStack *error_stack) {
  return impl_top_commit(config, error_stack);
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

  impl_export_if_autosave(config, error_stack);

  if (!error_stack_is_empty(error_stack)) {
    config_restore_game_and_history(config);
    return empty_string();
  }

  return empty_string();
}

void execute_challenge(Config *config, ErrorStack *error_stack) {
  char *result = impl_challenge(config, error_stack);
  if (error_stack_is_empty(error_stack)) {
    execute_show_game(config, error_stack);
    thread_control_print(config->thread_control, result);
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

  impl_export_if_autosave(config, error_stack);

  if (!error_stack_is_empty(error_stack)) {
    config_restore_game_and_history(config);
    return empty_string();
  }

  return empty_string();
}

void execute_unchallenge(Config *config, ErrorStack *error_stack) {
  char *result = impl_unchallenge(config, error_stack);
  if (error_stack_is_empty(error_stack)) {
    execute_show_game(config, error_stack);
    thread_control_print(config->thread_control, result);
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

  if (!error_stack_is_empty(error_stack)) {
    config_restore_game_and_history(config);
    return empty_string();
  }

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
        get_formatted_string("no prior game event has a cumulative score "
                             "for player '%s' when "
                             "applying time penalty",
                             player_nickname));
    config_restore_game_and_history(config);
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

  if (!error_stack_is_empty(error_stack)) {
    config_restore_game_and_history(config);
    return empty_string();
  }

  config_game_play_events(config, error_stack);

  if (!error_stack_is_empty(error_stack)) {
    config_restore_game_and_history(config);
    return empty_string();
  }

  impl_export_if_autosave(config, error_stack);

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
    execute_show_game(config, error_stack);
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
  game_history_switch_names(config->game_history);
  impl_export_if_autosave(config, error_stack);
  return empty_string();
}

void execute_switch_names(Config *config, ErrorStack *error_stack) {
  char *result = impl_switch_names(config, error_stack);
  if (error_stack_is_empty(error_stack)) {
    thread_control_print(config->thread_control, result);
    execute_show_game(config, error_stack);
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
    execute_show_game(config, error_stack);
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
    execute_show_game(config, error_stack);
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
    execute_show_game(config, error_stack);
  }
  free(result);
}

char *str_api_goto(Config *config, ErrorStack *error_stack) {
  return impl_goto(config, error_stack);
}

// Note

char *impl_note(Config *config, ErrorStack *error_stack) {
  if (!config_has_game_data(config)) {
    error_stack_push(error_stack, ERROR_STATUS_CONFIG_LOAD_GAME_DATA_MISSING,
                     string_duplicate("cannot add a note without loaded game"));
    return empty_string();
  }

  if (game_history_get_num_events(config->game_history) == 0) {
    error_stack_push(
        error_stack, ERROR_STATUS_NOTE_NO_GAME_EVENTS,
        string_duplicate("cannot add a note to an empty game history"));
    return empty_string();
  }

  config_init_game(config);

  game_history_set_note_for_most_recent_event(
      config->game_history, config_get_parg_value(config, ARG_TOKEN_NOTE, 0));
  impl_export_if_autosave(config, error_stack);
  return empty_string();
}

void execute_note(Config *config, ErrorStack *error_stack) {
  char *result = impl_note(config, error_stack);
  if (error_stack_is_empty(error_stack)) {
    thread_control_print(config->thread_control, result);
    execute_show_game(config, error_stack);
  }
  free(result);
}

char *str_api_note(Config *config, ErrorStack *error_stack) {
  return impl_note(config, error_stack);
}

// Set player names

char *impl_set_player_name(Config *config, ErrorStack *error_stack,
                           arg_token_t arg_token) {
  const char *new_player_name = config_get_parg_value(config, arg_token, 0);
  int player_index = -1;
  switch (arg_token) {
  case ARG_TOKEN_P1_NAME:
    player_index = 0;
    break;
  case ARG_TOKEN_P2_NAME:
    player_index = 1;
    break;
  default:
    log_fatal("encountered unexpected arg token when setting player name");
    break;
  }
  game_history_player_reset_names(config->game_history, player_index,
                                  new_player_name, NULL);
  impl_export_if_autosave(config, error_stack);
  return empty_string();
}

void execute_set_player_name(Config *config, ErrorStack *error_stack,
                             arg_token_t arg_token) {
  char *result = impl_set_player_name(config, error_stack, arg_token);
  thread_control_print(config->thread_control, result);
  free(result);
}

void execute_set_player_one_name(Config *config, ErrorStack *error_stack) {
  execute_set_player_name(config, error_stack, ARG_TOKEN_P1_NAME);
}

void execute_set_player_two_name(Config *config, ErrorStack *error_stack) {
  execute_set_player_name(config, error_stack, ARG_TOKEN_P2_NAME);
}

char *str_api_set_player_name(Config *config, ErrorStack *error_stack,
                              arg_token_t arg_token) {
  return impl_set_player_name(config, error_stack, arg_token);
}

char *str_api_set_player_one_name(Config *config, ErrorStack *error_stack) {
  return impl_set_player_name(config, error_stack, ARG_TOKEN_P1_NAME);
}

char *str_api_set_player_two_name(Config *config, ErrorStack *error_stack) {
  return impl_set_player_name(config, error_stack, ARG_TOKEN_P2_NAME);
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
    execute_show_game(config, error_stack);
  }
  free(result);
}

char *str_api_load_gcg(Config *config, ErrorStack *error_stack) {
  return impl_load_gcg(config, error_stack);
}

// The pargs takes ownership of the value
void add_value_to_parg(ParsedArg *current_parg, int *current_value_index,
                       const char *value) {
  free(current_parg->values[*current_value_index]);
  if (!value) {
    current_parg->values[*current_value_index] = empty_string();
  } else {
    current_parg->values[*current_value_index] = string_duplicate(value);
  }
  *current_value_index = *current_value_index + 1;
  current_parg->num_set_values = *current_value_index;
}

// Config load helpers

void config_load_parsed_args(Config *config,
                             const StringSplitter *cmd_split_string,
                             const char *cmd, ErrorStack *error_stack) {
  int number_of_input_strs =
      string_splitter_get_number_of_items(cmd_split_string);
  config->exec_parg_token = NUMBER_OF_ARG_TOKENS;

  for (int k = 0; k < NUMBER_OF_ARG_TOKENS; k++) {
    config->pargs[k]->num_set_values = 0;
  }

  ParsedArg *current_parg = NULL;
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
                                   "'%s', expected %d but got %d",
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

      arg_token_t current_arg_token =
          get_token_from_string(config, arg_name, error_stack);

      if (!error_stack_is_empty(error_stack)) {
        return;
      }

      current_parg = config->pargs[current_arg_token];

      if (current_parg->num_set_values > 0) {
        error_stack_push(
            error_stack, ERROR_STATUS_CONFIG_LOAD_DUPLICATE_ARG,
            get_formatted_string("command '%s' was provided more than once",
                                 arg_name));
        return;
      }

      if (current_parg->is_command) {
        if (i > 0) {
          error_stack_push(
              error_stack, ERROR_STATUS_CONFIG_LOAD_MISPLACED_COMMAND,
              get_formatted_string("encountered unexpected command '%s'",
                                   arg_name));
          return;
        }
        config->exec_parg_token = current_arg_token;
      }
      current_value_index = 0;
      if (current_arg_token == ARG_TOKEN_NOTE ||
          current_arg_token == ARG_TOKEN_P1_NAME ||
          current_arg_token == ARG_TOKEN_P2_NAME) {
        // Add the rest of the remaining string to the next parg value,
        // which basically treats the rest of the string after the command
        // as a single argument.
        char *cmd_content = strchr(cmd, ' ');
        if (cmd_content) {
          cmd_content = cmd_content + 1;
        }
        add_value_to_parg(current_parg, &current_value_index, cmd_content);
        return;
      }
    } else {
      if (!current_parg || current_value_index >= current_parg->num_values) {
        error_stack_push(
            error_stack, ERROR_STATUS_CONFIG_LOAD_UNRECOGNIZED_ARG,
            get_formatted_string("unrecognized command or argument '%s'",
                                 input_str));
        return;
      }
      add_value_to_parg(current_parg, &current_value_index, input_str);
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
  if (has_iprefix(sort_type_str, MOVE_SORT_EQUITY_STRING)) {
    players_data_set_move_sort_type(config->players_data, player_index,
                                    MOVE_SORT_EQUITY);
  } else if (has_iprefix(sort_type_str, MOVE_SORT_SCORE_STRING)) {
    players_data_set_move_sort_type(config->players_data, player_index,
                                    MOVE_SORT_SCORE);
  } else {
    error_stack_push(
        error_stack, ERROR_STATUS_CONFIG_LOAD_MALFORMED_MOVE_SORT_TYPE,
        get_formatted_string("unrecognized move sort type: %s", sort_type_str));
  }
}

void string_builder_add_move_sort_type(StringBuilder *sb,
                                       const move_sort_t sort_type) {
  switch (sort_type) {
  case MOVE_SORT_EQUITY:
    string_builder_add_string(sb, MOVE_SORT_EQUITY_STRING);
    break;
  case MOVE_SORT_SCORE:
    string_builder_add_string(sb, MOVE_SORT_SCORE_STRING);
    break;
  }
}

void config_load_record_type(Config *config, const char *record_type_str,
                             int player_index, ErrorStack *error_stack) {
  if (has_iprefix(record_type_str, MOVE_RECORD_BEST_STRING)) {
    players_data_set_move_record_type(config->players_data, player_index,
                                      MOVE_RECORD_BEST);
  } else if (has_iprefix(record_type_str,
                         MOVE_RECORD_WITHIN_X_EQUITY_OF_BEST_STRING)) {
    players_data_set_move_record_type(config->players_data, player_index,
                                      MOVE_RECORD_WITHIN_X_EQUITY_OF_BEST);
  } else if (has_iprefix(record_type_str, MOVE_RECORD_ALL_STRING)) {
    players_data_set_move_record_type(config->players_data, player_index,
                                      MOVE_RECORD_ALL);
  } else if (has_iprefix(record_type_str, MOVE_RECORD_ALL_SMALL_STRING)) {
    players_data_set_move_record_type(config->players_data, player_index,
                                      MOVE_RECORD_ALL_SMALL);
  } else {
    error_stack_push(error_stack,
                     ERROR_STATUS_CONFIG_LOAD_MALFORMED_MOVE_RECORD_TYPE,
                     get_formatted_string("unrecognized move record type: %s",
                                          record_type_str));
  }
}

void string_builder_add_move_record_type(StringBuilder *sb,
                                         const move_record_t record_type) {
  switch (record_type) {
  case MOVE_RECORD_BEST:
    string_builder_add_string(sb, MOVE_RECORD_BEST_STRING);
    break;
  case MOVE_RECORD_WITHIN_X_EQUITY_OF_BEST:
    string_builder_add_string(sb, MOVE_RECORD_WITHIN_X_EQUITY_OF_BEST_STRING);
    break;
  case MOVE_RECORD_ALL:
    string_builder_add_string(sb, MOVE_RECORD_ALL_STRING);
    break;
  case MOVE_RECORD_ALL_SMALL:
    string_builder_add_string(sb, MOVE_RECORD_ALL_SMALL_STRING);
    break;
  }
}

void string_builder_add_exec_mode_type(StringBuilder *sb,
                                       const exec_mode_t exec_mode) {
  switch (exec_mode) {
  case EXEC_MODE_SYNC:
    string_builder_add_string(sb, EXEC_MODE_SYNC_STRING);
    break;
  case EXEC_MODE_ASYNC:
    string_builder_add_string(sb, EXEC_MODE_ASYNC_STRING);
    break;
  case EXEC_MODE_UNKNOWN:
    log_fatal("cannot convert unknown exec mode to string");
    break;
  }
}

void config_load_sampling_rule(Config *config, const char *sampling_rule_str,
                               ErrorStack *error_stack) {
  if (has_iprefix(sampling_rule_str, BAI_SAMPLING_RULE_ROUND_ROBIN_STRING)) {
    config->sampling_rule = BAI_SAMPLING_RULE_ROUND_ROBIN;
  } else if (has_iprefix(sampling_rule_str,
                         BAI_SAMPLING_RULE_TOP_TWO_IDS_STRING)) {
    config->sampling_rule = BAI_SAMPLING_RULE_TOP_TWO_IDS;
  } else {
    error_stack_push(error_stack,
                     ERROR_STATUS_CONFIG_LOAD_MALFORMED_SAMPLING_RULE,
                     get_formatted_string("unrecognized sampling rule: %s",
                                          sampling_rule_str));
  }
}

void string_builder_add_sampling_rule(StringBuilder *sb,
                                      const bai_sampling_rule_t sampling_rule) {
  switch (sampling_rule) {
  case BAI_SAMPLING_RULE_ROUND_ROBIN:
    string_builder_add_string(sb, BAI_SAMPLING_RULE_ROUND_ROBIN_STRING);
    break;
  case BAI_SAMPLING_RULE_TOP_TWO_IDS:
    string_builder_add_string(sb, BAI_SAMPLING_RULE_TOP_TWO_IDS_STRING);
    break;
  }
}

void config_load_threshold(Config *config, const char *threshold_str,
                           ErrorStack *error_stack) {
  if (has_iprefix(threshold_str, BAI_THRESHOLD_NONE_STRING)) {
    config->threshold = BAI_THRESHOLD_NONE;
  } else if (has_iprefix(threshold_str, BAI_THRESHOLD_GK16_STRING)) {
    config->threshold = BAI_THRESHOLD_GK16;
  } else {
    error_stack_push(
        error_stack, ERROR_STATUS_CONFIG_LOAD_MALFORMED_THRESHOLD,
        get_formatted_string("unrecognized threshold type: %s", threshold_str));
  }
}

void string_builder_add_threshold(StringBuilder *sb,
                                  const bai_threshold_t threshold) {
  switch (threshold) {
  case BAI_THRESHOLD_NONE:
    string_builder_add_string(sb, BAI_THRESHOLD_NONE_STRING);
    break;
  case BAI_THRESHOLD_GK16:
    string_builder_add_string(sb, BAI_THRESHOLD_GK16_STRING);
    break;
  }
}

void string_builder_add_game_string_board_color_type(
    StringBuilder *sb,
    const game_string_board_color_t game_string_board_color) {
  switch (game_string_board_color) {
  case GAME_STRING_BOARD_COLOR_NONE:
    string_builder_add_string(sb, GAME_STRING_BOARD_COLOR_NONE_STRING);
    break;
  case GAME_STRING_BOARD_COLOR_ANSI:
    string_builder_add_string(sb, GAME_STRING_BOARD_COLOR_ANSI_STRING);
    break;
  case GAME_STRING_BOARD_COLOR_XTERM_256:
    string_builder_add_string(sb, GAME_STRING_BOARD_COLOR_XTERM_256_STRING);
    break;
  case GAME_STRING_BOARD_COLOR_TRUECOLOR:
    string_builder_add_string(sb, GAME_STRING_BOARD_COLOR_TRUECOLOR_STRING);
    break;
  }
}

void string_builder_add_game_string_board_tile_glyphs_type(
    StringBuilder *sb,
    const game_string_board_tile_glyphs_t game_string_board_tile_glyphs) {
  switch (game_string_board_tile_glyphs) {
  case GAME_STRING_BOARD_TILE_GLYPHS_PRIMARY:
    string_builder_add_string(sb, GAME_STRING_BOARD_TILE_GLYPHS_PRIMARY_STRING);
    break;
  case GAME_STRING_BOARD_TILE_GLYPHS_ALT:
    string_builder_add_string(sb, GAME_STRING_BOARD_TILE_GLYPHS_ALT_STRING);
    break;
  }
}

void string_builder_add_game_string_board_border_type(
    StringBuilder *sb,
    const game_string_board_border_t game_string_board_border) {
  switch (game_string_board_border) {
  case GAME_STRING_BOARD_BORDER_ASCII:
    string_builder_add_string(sb, GAME_STRING_BOARD_BORDER_ASCII_STRING);
    break;
  case GAME_STRING_BOARD_BORDER_BOX_DRAWING:
    string_builder_add_string(sb, GAME_STRING_BOARD_BORDER_BOX_DRAWING_STRING);
    break;
  }
}

void string_builder_add_game_string_board_column_label_type(
    StringBuilder *sb,
    const game_string_board_column_label_t game_string_board_column_label) {
  switch (game_string_board_column_label) {
  case GAME_STRING_BOARD_COLUMN_LABEL_ASCII:
    string_builder_add_string(sb, GAME_STRING_BOARD_COLUMN_LABEL_ASCII_STRING);
    break;
  case GAME_STRING_BOARD_COLUMN_LABEL_FULLWIDTH:
    string_builder_add_string(sb,
                              GAME_STRING_BOARD_COLUMN_LABEL_FULLWIDTH_STRING);
    break;
  }
}

void string_builder_add_game_string_on_turn_marker_type(
    StringBuilder *sb,
    const game_string_on_turn_marker_t game_string_on_turn_marker) {
  switch (game_string_on_turn_marker) {
  case GAME_STRING_ON_TURN_MARKER_ASCII:
    string_builder_add_string(sb, GAME_STRING_ON_TURN_MARKER_ASCII_STRING);
    break;
  case GAME_STRING_ON_TURN_MARKER_ARROWHEAD:
    string_builder_add_string(sb, GAME_STRING_ON_TURN_MARKER_ARROWHEAD_STRING);
    break;
  }
}

void string_builder_add_game_string_on_turn_color_type(
    StringBuilder *sb,
    const game_string_on_turn_color_t game_string_on_turn_color) {
  switch (game_string_on_turn_color) {
  case GAME_STRING_ON_TURN_COLOR_NONE:
    string_builder_add_string(sb, GAME_STRING_ON_TURN_COLOR_NONE_STRING);
    break;
  case GAME_STRING_ON_TURN_COLOR_ANSI_GREEN:
    string_builder_add_string(sb, GAME_STRING_ON_TURN_COLOR_ANSI_GREEN_STRING);
    break;
  }
}

void string_builder_add_game_string_on_turn_score_style_type(
    StringBuilder *sb,
    const game_string_on_turn_score_style_t game_string_on_turn_score_style) {
  switch (game_string_on_turn_score_style) {
  case GAME_STRING_ON_TURN_SCORE_NORMAL:
    string_builder_add_string(sb, GAME_STRING_ON_TURN_SCORE_NORMAL_STRING);
    break;
  case GAME_STRING_ON_TURN_SCORE_BOLD:
    string_builder_add_string(sb, GAME_STRING_ON_TURN_SCORE_BOLD_STRING);
    break;
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
  if (has_iprefix(exec_mode_str, EXEC_MODE_SYNC_STRING)) {
    exec_mode = EXEC_MODE_SYNC;
  } else if (has_iprefix(exec_mode_str, EXEC_MODE_ASYNC_STRING)) {
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
  // Start by assuming we are just using whatever the existing wmp settings
  // are
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
          config_reset_move_list_and_invalidate_sim_results(config);
          inference_results_set_valid_for_current_game_state(
              config->inference_results, false);
          endgame_results_set_valid_for_current_game_state(
              config->endgame_results, false);
        }
      }
      if (error_stack_is_empty(error_stack)) {
        autoplay_results_set_ld(config->autoplay_results, config->ld);
      }
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

  config_load_int(config, ARG_TOKEN_MAX_NUMBER_OF_DISPLAY_PLAYS, 1, INT_MAX,
                  &config->max_num_display_plays, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  config_load_uint64(config, ARG_TOKEN_MAX_ITERATIONS, 1,
                     &config->max_iterations, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  config_load_uint64(config, ARG_TOKEN_MIN_PLAY_ITERATIONS, 1,
                     &config->min_play_iterations, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  config_load_int(config, ARG_TOKEN_NUMBER_OF_THREADS, 1, MAX_THREADS,
                  &config->num_threads, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  config_load_int(config, ARG_TOKEN_PRINT_INTERVAL, 0, INT_MAX,
                  &config->print_interval, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  config_load_uint64(config, ARG_TOKEN_TIME_LIMIT, 0,
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
      config_get_parg_value(config, ARG_TOKEN_INFERENCE_MARGIN, 0);
  if (new_eq_margin_inference_double) {
    double eq_margin_inference_double = 0;
    config_load_double(config, ARG_TOKEN_INFERENCE_MARGIN, 0, EQUITY_MAX_DOUBLE,
                       &eq_margin_inference_double, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
    assert(!isnan(eq_margin_inference_double));
    config->eq_margin_inference = double_to_equity(eq_margin_inference_double);
  }

  const char *new_eq_margin_movegen =
      config_get_parg_value(config, ARG_TOKEN_MOVEGEN_MARGIN, 0);
  if (new_eq_margin_movegen) {
    double eq_margin_movegen = NAN;
    config_load_double(config, ARG_TOKEN_MOVEGEN_MARGIN, 0, EQUITY_MAX_DOUBLE,
                       &eq_margin_movegen, error_stack);
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

  // Use heatmaps

  config_load_bool(config, ARG_TOKEN_USE_HEAT_MAP, &config->use_heat_map,
                   error_stack);
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

  // Save settings

  config_load_bool(config, ARG_TOKEN_SAVE_SETTINGS, &config->save_settings,
                   error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  // Autosave GCG
  config_load_bool(config, ARG_TOKEN_AUTOSAVE_GCG, &config->autosave_gcg,
                   error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  // Board color

  const char *board_color_str =
      config_get_parg_value(config, ARG_TOKEN_BOARD_COLOR, 0);
  if (board_color_str) {
    if (has_iprefix(board_color_str, GAME_STRING_BOARD_COLOR_NONE_STRING)) {
      config->game_string_options->board_color = GAME_STRING_BOARD_COLOR_NONE;
    } else if (has_iprefix(board_color_str,
                           GAME_STRING_BOARD_COLOR_ANSI_STRING)) {
      config->game_string_options->board_color = GAME_STRING_BOARD_COLOR_ANSI;
    } else if (has_iprefix(board_color_str,
                           GAME_STRING_BOARD_COLOR_XTERM_256_STRING)) {
      config->game_string_options->board_color =
          GAME_STRING_BOARD_COLOR_XTERM_256;
    } else if (has_iprefix(board_color_str,
                           GAME_STRING_BOARD_COLOR_TRUECOLOR_STRING)) {
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

  if (config_get_parg_value(config, ARG_TOKEN_RANDOM_SEED, 0)) {
    config_load_uint64(config, ARG_TOKEN_RANDOM_SEED, 0, &config->seed,
                       error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
  } else {
    config->seed++;
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

  const char *cutoff = config_get_parg_value(config, ARG_TOKEN_CUTOFF, 0);
  if (cutoff) {
    double user_cutoff = 0;
    config_load_double(config, ARG_TOKEN_CUTOFF, 0, 100, &user_cutoff,
                       error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
    config->cutoff = convert_user_cutoff_to_cutoff(user_cutoff);
  }

  // Non-lexical player data

  const arg_token_t sort_type_args[2] = {ARG_TOKEN_P1_MOVE_SORT_TYPE,
                                         ARG_TOKEN_P2_MOVE_SORT_TYPE};
  const arg_token_t record_type_args[2] = {ARG_TOKEN_P1_MOVE_RECORD_TYPE,
                                           ARG_TOKEN_P2_MOVE_RECORD_TYPE};

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
  }

  config_load_lexicon_dependent_data(config, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
  // If the letter distribution changed, destroy the existing game
  // and recreate it so that it will be lazily recreated with the new
  // letter distribution when initialized.
  if (config->ld_changed && config->game) {
    game_destroy(config->game);
    config->game = NULL;
  }

  // Update the game history
  update_game_history_with_config(config);

  // Set win pct - load if explicitly specified and either not loaded yet
  // or if name changed. Lazy loading happens in impl_sim when needed for
  // simulations that don't specify a win_pct explicitly.
  const char *new_win_pct_name =
      config_get_parg_value(config, ARG_TOKEN_WIN_PCT, 0);
  if (new_win_pct_name != NULL &&
      (config->win_pcts == NULL ||
       !strings_equal(win_pct_get_name(config->win_pcts), new_win_pct_name))) {
    win_pct_destroy(config->win_pcts);
    config->win_pcts =
        win_pct_create(config->data_paths, new_win_pct_name, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
  }
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
  config_load_parsed_args(config, cmd_split_string, cmd, error_stack);

  if (error_stack_is_empty(error_stack)) {
    config_load_data(config, error_stack);
  }

  string_splitter_destroy(cmd_split_string);
}

void config_execute_command(Config *config, ErrorStack *error_stack) {
  if (config_exec_parg_is_set(config)) {
    config_get_parg_exec_func(config, config->exec_parg_token)(config,
                                                               error_stack);
    if (config->print_on_finish && config->loaded_settings) {
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
  char *result = impl_add_moves(config, error_stack);
  if (error_stack_is_empty(error_stack)) {
    thread_control_print(config->thread_control, result);
  }
  free(result);
}

char *str_api_add_moves(Config *config, ErrorStack *error_stack) {
  return impl_add_moves(config, error_stack);
}

void execute_set_rack(Config *config, ErrorStack *error_stack) {
  impl_set_rack(config, ARG_TOKEN_RACK, error_stack);
}

char *str_api_set_rack(Config *config, ErrorStack *error_stack) {
  impl_set_rack(config, ARG_TOKEN_RACK, error_stack);
  return empty_string();
}

void execute_set_random_rack(Config *config, ErrorStack *error_stack) {
  impl_set_random_rack(config, error_stack);
}

char *str_api_set_random_rack(Config *config, ErrorStack *error_stack) {
  impl_set_random_rack(config, error_stack);
  return empty_string();
}

void execute_move_gen(Config *config, ErrorStack *error_stack) {
  impl_move_gen(config, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
  execute_show_moves_or_sim_results(config, error_stack);
}

char *str_api_move_gen(Config *config, ErrorStack *error_stack) {
  impl_move_gen(config, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return empty_string();
  }
  return impl_show_moves_or_sim_results(config, error_stack);
}

void execute_sim(Config *config, ErrorStack *error_stack) {
  impl_sim(config, ARG_TOKEN_SIM, 0, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
  execute_show_moves_or_sim_results(config, error_stack);
}

char *str_api_sim(Config *config, ErrorStack *error_stack) {
  impl_sim(config, ARG_TOKEN_SIM, 0, error_stack);
  return empty_string();
}

void execute_gen_and_sim(Config *config, ErrorStack *error_stack) {
  impl_gen_and_sim(config, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
  execute_show_moves_or_sim_results(config, error_stack);
}

char *str_api_gen_and_sim(Config *config, ErrorStack *error_stack) {
  impl_gen_and_sim(config, error_stack);
  return empty_string();
}

void execute_rack_and_gen_and_sim(Config *config, ErrorStack *error_stack) {
  impl_rack_and_gen_and_sim(config, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
  execute_show_moves_or_sim_results(config, error_stack);
}

char *str_api_rack_and_gen_and_sim(Config *config, ErrorStack *error_stack) {
  impl_rack_and_gen_and_sim(config, error_stack);
  return empty_string();
}

void execute_infer(Config *config, ErrorStack *error_stack) {
  impl_infer(config, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
  execute_show_inference(config, error_stack);
}

char *str_api_infer(Config *config, ErrorStack *error_stack) {
  impl_infer(config, error_stack);
  return empty_string();
}

void execute_endgame(Config *config, ErrorStack *error_stack) {
  impl_endgame(config, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
  execute_show_endgame(config, error_stack);
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

Config *config_create(const ConfigArgs *config_args, ErrorStack *error_stack) {
  Config *config = calloc_or_die(1, sizeof(Config));
  // Set the values specified by the args first
  if (!config_args || !config_args->data_paths) {
    config->data_paths = string_duplicate(DEFAULT_DATA_PATHS);
  } else {
    config->data_paths = string_duplicate(config_args->data_paths);
  }
  if (!config_args || !config_args->settings_filename) {
    config->settings_filename = string_duplicate(DEFAULT_SETTINGS_FILENAME);
  } else {
    config->settings_filename =
        string_duplicate(config_args->settings_filename);
  }
  bool default_use_wmp = true;
  if (config_args) {
    default_use_wmp = config_args->use_wmp;
  }
  // Attempt to load fields that might fail first
  config->board_layout =
      board_layout_create_default(config->data_paths, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_push(
        error_stack, ERROR_STATUS_CONFIG_LOAD_BOARD_LAYOUT_ERROR,
        string_duplicate(
            "encountered an error loading the default board layout"));
    return NULL;
  }

  // win_pcts is loaded lazily on first use (simulation, etc.)
  config->win_pcts = NULL;

  // Command parsed from string input
#define cmd(token, name, n_req, n_val, func, stat, hotkey)                     \
  parsed_arg_create(config, token, name, n_req, n_val, execute_##func,         \
                    str_api_##func, status_##stat, hotkey, true)

  // Non-command arg
#define arg(token, name, n_req, n_val)                                         \
  parsed_arg_create(config, token, name, n_req, n_val, execute_fatal,          \
                    str_api_fatal, status_generic, false, false)

  cmd(ARG_TOKEN_HELP, "help", 0, 1, help, generic, false);
  cmd(ARG_TOKEN_SET, "setoptions", 0, 0, noop, generic, false);
  cmd(ARG_TOKEN_CGP, "cgp", 4, 4, load_cgp, generic, false);
  cmd(ARG_TOKEN_LOAD, "load", 1, 1, load_gcg, generic, false);
  cmd(ARG_TOKEN_NEW_GAME, "newgame", 0, 1, new_game, generic, false);
  cmd(ARG_TOKEN_EXPORT, "export", 0, 1, export, generic, true);
  cmd(ARG_TOKEN_COMMIT, "commit", 1, 3, commit, generic, true);
  cmd(ARG_TOKEN_TOP_COMMIT, "tcommit", 0, 1, top_commit, generic, true);
  cmd(ARG_TOKEN_CHALLENGE, "challenge", 0, 1, challenge, generic, false);
  cmd(ARG_TOKEN_UNCHALLENGE, "unchallenge", 0, 1, unchallenge, generic, false);
  cmd(ARG_TOKEN_OVERTIME, "overtimepenalty", 2, 2, overtime, generic, false);
  cmd(ARG_TOKEN_SWITCH_NAMES, "switchnames", 0, 0, switch_names, generic,
      false);
  cmd(ARG_TOKEN_SHOW_GAME, "shgame", 0, 0, show_game, generic, true);
  cmd(ARG_TOKEN_SHOW_MOVES, "shmoves", 0, 1, show_moves_or_sim_results, generic,
      false);
  cmd(ARG_TOKEN_SHOW_INFERENCE, "shinference", 0, 1, show_inference, generic,
      false);
  cmd(ARG_TOKEN_SHOW_ENDGAME, "shendgame", 0, 0, show_endgame, generic, false);
  cmd(ARG_TOKEN_SHOW_HEAT_MAP, "heatmap", 1, 3, show_heat_map, generic, false);
  cmd(ARG_TOKEN_MOVES, "addmoves", 1, 1, add_moves, generic, true);
  cmd(ARG_TOKEN_RACK, "rack", 1, 1, set_rack, generic, true);
  cmd(ARG_TOKEN_RANDOM_RACK, "rrack", 0, 0, set_random_rack, generic, true);
  cmd(ARG_TOKEN_GEN, "generate", 0, 0, move_gen, generic, true);
  cmd(ARG_TOKEN_SIM, "simulate", 0, 1, sim, sim, false);
  cmd(ARG_TOKEN_GEN_AND_SIM, "gsimulate", 0, 1, gen_and_sim, gen_and_sim,
      false);
  cmd(ARG_TOKEN_RACK_AND_GEN_AND_SIM, "rgsimulate", 1, 2, rack_and_gen_and_sim,
      rack_and_gen_and_sim, false);
  cmd(ARG_TOKEN_INFER, "infer", 0, 5, infer, generic, false);
  cmd(ARG_TOKEN_ENDGAME, "endgame", 0, 0, endgame, endgame, false);
  cmd(ARG_TOKEN_AUTOPLAY, "autoplay", 2, 2, autoplay, autoplay, false);
  cmd(ARG_TOKEN_CONVERT, "convert", 2, 3, convert, generic, false);
  cmd(ARG_TOKEN_LEAVE_GEN, "leavegen", 2, 2, leave_gen, generic, false);
  cmd(ARG_TOKEN_CREATE_DATA, "createdata", 2, 3, create_data, generic, false);
  cmd(ARG_TOKEN_NEXT, "next", 0, 0, next, generic, true);
  cmd(ARG_TOKEN_PREVIOUS, "previous", 0, 0, previous, generic, true);
  cmd(ARG_TOKEN_GOTO, "goto", 1, 1, goto, generic, false);
  cmd(ARG_TOKEN_NOTE, "note", 1, 1, note, generic, false);
  cmd(ARG_TOKEN_P1_NAME, "p1", 1, 1, set_player_one_name, generic, false);
  cmd(ARG_TOKEN_P2_NAME, "p2", 1, 1, set_player_two_name, generic, false);

  arg(ARG_TOKEN_DATA_PATH, "path", 1, 1);
  arg(ARG_TOKEN_BINGO_BONUS, "bb", 1, 1);
  arg(ARG_TOKEN_CHALLENGE_BONUS, "cb", 1, 1);
  arg(ARG_TOKEN_BOARD_LAYOUT, "bdn", 1, 1);
  arg(ARG_TOKEN_GAME_VARIANT, "var", 1, 1);
  arg(ARG_TOKEN_LETTER_DISTRIBUTION, "ld", 1, 1);
  arg(ARG_TOKEN_LEXICON, "lex", 1, 1);
  arg(ARG_TOKEN_USE_WMP, "wmp", 1, 1);
  arg(ARG_TOKEN_LEAVES, "leaves", 1, 1);
  arg(ARG_TOKEN_P1_LEXICON, "l1", 1, 1);
  arg(ARG_TOKEN_P1_USE_WMP, "w1", 1, 1);
  arg(ARG_TOKEN_P1_LEAVES, "k1", 1, 1);
  arg(ARG_TOKEN_P1_MOVE_SORT_TYPE, "s1", 1, 1);
  arg(ARG_TOKEN_P1_MOVE_RECORD_TYPE, "r1", 1, 1);
  arg(ARG_TOKEN_P2_LEXICON, "l2", 1, 1);
  arg(ARG_TOKEN_P2_USE_WMP, "w2", 1, 1);
  arg(ARG_TOKEN_P2_LEAVES, "k2", 1, 1);
  arg(ARG_TOKEN_P2_MOVE_SORT_TYPE, "s2", 1, 1);
  arg(ARG_TOKEN_P2_MOVE_RECORD_TYPE, "r2", 1, 1);
  arg(ARG_TOKEN_WIN_PCT, "winpct", 1, 1);
  arg(ARG_TOKEN_PLIES, "plies", 1, 1);
  arg(ARG_TOKEN_ENDGAME_PLIES, "eplies", 1, 1);
  arg(ARG_TOKEN_NUMBER_OF_PLAYS, "numplays", 1, 1);
  arg(ARG_TOKEN_MAX_NUMBER_OF_DISPLAY_PLAYS, "maxnumdplays", 1, 1);
  arg(ARG_TOKEN_NUMBER_OF_SMALL_PLAYS, "numsmallplays", 1, 1);
  arg(ARG_TOKEN_MAX_ITERATIONS, "iterations", 1, 1);
  arg(ARG_TOKEN_MIN_PLAY_ITERATIONS, "minplayiterations", 1, 1);
  arg(ARG_TOKEN_STOP_COND_PCT, "scondition", 1, 1);
  arg(ARG_TOKEN_INFERENCE_MARGIN, "imargin", 1, 1);
  arg(ARG_TOKEN_MOVEGEN_MARGIN, "mmargin", 1, 1);
  arg(ARG_TOKEN_USE_GAME_PAIRS, "gp", 1, 1);
  arg(ARG_TOKEN_USE_SMALL_PLAYS, "sp", 1, 1);
  arg(ARG_TOKEN_SIM_WITH_INFERENCE, "sinfer", 1, 1);
  arg(ARG_TOKEN_USE_HEAT_MAP, "useheatmap", 1, 1);
  arg(ARG_TOKEN_HUMAN_READABLE, "hr", 1, 1);
  arg(ARG_TOKEN_WRITE_BUFFER_SIZE, "wb", 1, 1);
  arg(ARG_TOKEN_RANDOM_SEED, "seed", 1, 1);
  arg(ARG_TOKEN_NUMBER_OF_THREADS, "threads", 1, 1);
  arg(ARG_TOKEN_PRINT_INTERVAL, "pfrequency", 1, 1);
  arg(ARG_TOKEN_EXEC_MODE, "mode", 1, 1);
  arg(ARG_TOKEN_TT_FRACTION_OF_MEM, "ttfraction", 1, 1);
  arg(ARG_TOKEN_TIME_LIMIT, "tlim", 1, 1);
  arg(ARG_TOKEN_SAMPLING_RULE, "sr", 1, 1);
  arg(ARG_TOKEN_THRESHOLD, "threshold", 1, 1);
  arg(ARG_TOKEN_CUTOFF, "cutoff", 1, 1);
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
  arg(ARG_TOKEN_SAVE_SETTINGS, "savesettings", 1, 1);
  arg(ARG_TOKEN_AUTOSAVE_GCG, "autosavegcg", 1, 1);

#undef cmd
#undef arg

  // Set shortest disambiguous command hints
  Trie *trie = trie_create();
  for (size_t i = 0; i < NUMBER_OF_ARG_TOKENS; i++) {
    const char *name = config->pargs[i]->name;
    trie_add_word(trie, name);
    config->pargs[i]->shortest_unambiguous_name = string_duplicate(name);
  }
  for (size_t i = 0; i < NUMBER_OF_ARG_TOKENS; i++) {
    const int su_index =
        trie_get_shortest_unambiguous_index(trie, config->pargs[i]->name);
    config->pargs[i]->shortest_unambiguous_name[su_index] = '\0';
  }
  trie_destroy(trie);

  config->exec_parg_token = NUMBER_OF_ARG_TOKENS;
  config->ld_changed = false;
  config->exec_mode = EXEC_MODE_ASYNC;
  config->bingo_bonus = DEFAULT_BINGO_BONUS;
  config->challenge_bonus = DEFAULT_CHALLENGE_BONUS;
  config->num_plays = 100;
  config->max_num_display_plays = 15;
  config->num_small_plays = DEFAULT_SMALL_MOVE_LIST_CAPACITY;
  config->plies = 5;
  config->endgame_plies = 6;
  config->eq_margin_inference = int_to_equity(5);
  config->eq_margin_movegen = int_to_equity(5);
  config->min_play_iterations = 500;
  config->max_iterations = 1000000000000;
  config->stop_cond_pct = 99;
  config->cutoff = convert_user_cutoff_to_cutoff(0.005);
  config->time_limit_seconds = 0;
  config->num_threads = get_num_cores();
  config->print_interval = 0;
  config->seed = ctime_get_current_time();
  config->sampling_rule = BAI_SAMPLING_RULE_TOP_TWO_IDS;
  config->threshold = BAI_THRESHOLD_GK16;
  config->use_game_pairs = false;
  config->use_small_plays = false;
  config->human_readable = true;
  config->sim_with_inference = false;
  config->use_heat_map = false;
  config->print_boards = false;
  config->print_on_finish = false;
  config->show_prompt = true;
  config->save_settings = true;
  config->autosave_gcg = true;
  config->loaded_settings = true;
  config->game_variant = DEFAULT_GAME_VARIANT;
  config->ld = NULL;
  config->players_data = players_data_create(default_use_wmp);
  config->thread_control = thread_control_create();
  config->game = NULL;
  config->game_backup = NULL;
  config->move_list = NULL;
  config->game_history = game_history_create();
  config->game_history_backup = NULL;
  config->endgame_solver = endgame_solver_create();
  config->sim_results = sim_results_create(config->cutoff);
  config->inference_results = inference_results_create(NULL);
  config->endgame_results = endgame_results_create();
  config->autoplay_results = autoplay_results_create();
  config->conversion_results = conversion_results_create();
  config->tt_fraction_of_mem = 0.25;
  config->game_string_options = game_string_options_create_pretty();

  autoplay_results_set_players_data(config->autoplay_results,
                                    config->players_data);
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
  free(config->settings_filename);
  free(config->data_paths);
  free(config);
}

void config_add_string_setting_to_string_builder(const Config *config,
                                                 StringBuilder *sb,
                                                 arg_token_t arg_token,
                                                 const char *value) {
  if (value) {
    string_builder_add_formatted_string(sb, " -%s %s",
                                        config->pargs[arg_token]->name, value);
  }
}

void config_add_int_setting_to_string_builder(const Config *config,
                                              StringBuilder *sb,
                                              arg_token_t arg_token,
                                              int value) {
  string_builder_add_formatted_string(sb, " -%s %d",
                                      config->pargs[arg_token]->name, value);
}

void config_add_uint64_setting_to_string_builder(const Config *config,
                                                 StringBuilder *sb,
                                                 arg_token_t arg_token,
                                                 uint64_t value) {
  string_builder_add_formatted_string(sb, " -%s %lu",
                                      config->pargs[arg_token]->name, value);
}

void config_add_double_setting_to_string_builder(const Config *config,
                                                 StringBuilder *sb,
                                                 arg_token_t arg_token,
                                                 double value) {
  string_builder_add_formatted_string(sb, " -%s %.15f",
                                      config->pargs[arg_token]->name, value);
}

void config_add_bool_setting_to_string_builder(const Config *config,
                                               StringBuilder *sb,
                                               arg_token_t arg_token,
                                               bool value) {
  string_builder_add_formatted_string(
      sb, " -%s %s", config->pargs[arg_token]->name, value ? "true" : "false");
}

void config_add_settings_to_string_builder(const Config *config,
                                           StringBuilder *sb) {
  string_builder_add_string(sb, config->pargs[ARG_TOKEN_SET]->name);
  for (arg_token_t arg_token = 0; arg_token < NUMBER_OF_ARG_TOKENS;
       arg_token++) {
    switch (arg_token) {
    case ARG_TOKEN_HELP:
    case ARG_TOKEN_SET:
    case ARG_TOKEN_CGP:
    case ARG_TOKEN_MOVES:
    case ARG_TOKEN_RACK:
    case ARG_TOKEN_RANDOM_RACK:
    case ARG_TOKEN_GEN:
    case ARG_TOKEN_SIM:
    case ARG_TOKEN_GEN_AND_SIM:
    case ARG_TOKEN_RACK_AND_GEN_AND_SIM:
    case ARG_TOKEN_INFER:
    case ARG_TOKEN_ENDGAME:
    case ARG_TOKEN_AUTOPLAY:
    case ARG_TOKEN_CONVERT:
    case ARG_TOKEN_LEAVE_GEN:
    case ARG_TOKEN_CREATE_DATA:
    case ARG_TOKEN_LOAD:
    case ARG_TOKEN_NEW_GAME:
    case ARG_TOKEN_EXPORT:
    case ARG_TOKEN_COMMIT:
    case ARG_TOKEN_TOP_COMMIT:
    case ARG_TOKEN_CHALLENGE:
    case ARG_TOKEN_UNCHALLENGE:
    case ARG_TOKEN_OVERTIME:
    case ARG_TOKEN_SWITCH_NAMES:
    case ARG_TOKEN_SHOW_GAME:
    case ARG_TOKEN_SHOW_MOVES:
    case ARG_TOKEN_SHOW_INFERENCE:
    case ARG_TOKEN_SHOW_ENDGAME:
    case ARG_TOKEN_SHOW_HEAT_MAP:
    case ARG_TOKEN_NEXT:
    case ARG_TOKEN_PREVIOUS:
    case ARG_TOKEN_GOTO:
    case ARG_TOKEN_NOTE:
    case ARG_TOKEN_P1_NAME:
    case ARG_TOKEN_P2_NAME:
      break;
    case ARG_TOKEN_DATA_PATH:
      config_add_string_setting_to_string_builder(config, sb, arg_token,
                                                  config->data_paths);
      break;
    case ARG_TOKEN_BINGO_BONUS:
      config_add_int_setting_to_string_builder(config, sb, arg_token,
                                               config->bingo_bonus);
      break;
    case ARG_TOKEN_CHALLENGE_BONUS:
      config_add_int_setting_to_string_builder(config, sb, arg_token,
                                               config->challenge_bonus);
      break;
    case ARG_TOKEN_BOARD_LAYOUT:
      config_add_string_setting_to_string_builder(
          config, sb, arg_token, board_layout_get_name(config->board_layout));
      break;
    case ARG_TOKEN_GAME_VARIANT:
      string_builder_add_formatted_string(sb, " -%s ",
                                          config->pargs[arg_token]->name);
      string_builder_add_game_variant(sb, config->game_variant);

      break;
    case ARG_TOKEN_LETTER_DISTRIBUTION:
      if (config->ld) {
        config_add_string_setting_to_string_builder(config, sb, arg_token,
                                                    ld_get_name(config->ld));
      }
      break;
    case ARG_TOKEN_LEXICON:
    case ARG_TOKEN_USE_WMP:
    case ARG_TOKEN_LEAVES:
      // Set these values on a per-player basis
      break;
    case ARG_TOKEN_P1_LEXICON:
      config_add_string_setting_to_string_builder(
          config, sb, arg_token,
          players_data_get_data_name(config->players_data,
                                     PLAYERS_DATA_TYPE_KWG, 0));
      break;
    case ARG_TOKEN_P1_USE_WMP:
      config_add_bool_setting_to_string_builder(
          config, sb, arg_token,
          players_data_get_use_when_available(config->players_data,
                                              PLAYERS_DATA_TYPE_WMP, 0));
      break;
    case ARG_TOKEN_P1_LEAVES:
      config_add_string_setting_to_string_builder(
          config, sb, arg_token,
          players_data_get_data_name(config->players_data,
                                     PLAYERS_DATA_TYPE_KLV, 0));
      break;
    case ARG_TOKEN_P1_MOVE_SORT_TYPE:
      string_builder_add_formatted_string(sb, " -%s ",
                                          config->pargs[arg_token]->name);
      string_builder_add_move_sort_type(
          sb, players_data_get_move_sort_type(config->players_data, 0));
      break;
    case ARG_TOKEN_P1_MOVE_RECORD_TYPE:
      string_builder_add_formatted_string(sb, " -%s ",
                                          config->pargs[arg_token]->name);
      string_builder_add_move_record_type(
          sb, players_data_get_move_record_type(config->players_data, 0));
      break;
    case ARG_TOKEN_P2_LEXICON:
      config_add_string_setting_to_string_builder(
          config, sb, arg_token,
          players_data_get_data_name(config->players_data,
                                     PLAYERS_DATA_TYPE_KWG, 1));
      break;
    case ARG_TOKEN_P2_USE_WMP:
      config_add_bool_setting_to_string_builder(
          config, sb, arg_token,
          players_data_get_use_when_available(config->players_data,
                                              PLAYERS_DATA_TYPE_WMP, 1));
      break;
    case ARG_TOKEN_P2_LEAVES:
      config_add_string_setting_to_string_builder(
          config, sb, arg_token,
          players_data_get_data_name(config->players_data,
                                     PLAYERS_DATA_TYPE_KLV, 1));
      break;
    case ARG_TOKEN_P2_MOVE_SORT_TYPE:
      string_builder_add_formatted_string(sb, " -%s ",
                                          config->pargs[arg_token]->name);
      string_builder_add_move_sort_type(
          sb, players_data_get_move_sort_type(config->players_data, 1));
      break;
    case ARG_TOKEN_P2_MOVE_RECORD_TYPE:
      string_builder_add_formatted_string(sb, " -%s ",
                                          config->pargs[arg_token]->name);
      string_builder_add_move_record_type(
          sb, players_data_get_move_record_type(config->players_data, 1));
      break;
    case ARG_TOKEN_WIN_PCT:
      config_add_string_setting_to_string_builder(
          config, sb, arg_token,
          config->win_pcts ? win_pct_get_name(config->win_pcts)
                           : DEFAULT_WIN_PCT);
      break;
    case ARG_TOKEN_PLIES:
      config_add_int_setting_to_string_builder(config, sb, arg_token,
                                               config->plies);
      break;
    case ARG_TOKEN_ENDGAME_PLIES:
      config_add_int_setting_to_string_builder(config, sb, arg_token,
                                               config->endgame_plies);
      break;
    case ARG_TOKEN_NUMBER_OF_PLAYS:
      config_add_int_setting_to_string_builder(config, sb, arg_token,
                                               config->num_plays);
      break;
    case ARG_TOKEN_MAX_NUMBER_OF_DISPLAY_PLAYS:
      config_add_int_setting_to_string_builder(config, sb, arg_token,
                                               config->max_num_display_plays);
      break;
    case ARG_TOKEN_NUMBER_OF_SMALL_PLAYS:
      config_add_int_setting_to_string_builder(config, sb, arg_token,
                                               config->num_small_plays);
      break;
    case ARG_TOKEN_MAX_ITERATIONS:
      config_add_uint64_setting_to_string_builder(config, sb, arg_token,
                                                  config->max_iterations);
      break;
    case ARG_TOKEN_STOP_COND_PCT:
      config_add_double_setting_to_string_builder(config, sb, arg_token,
                                                  config->stop_cond_pct);
      break;
    case ARG_TOKEN_INFERENCE_MARGIN:
      if (config->eq_margin_inference != 0) {
        config_add_double_setting_to_string_builder(
            config, sb, arg_token,
            equity_to_double(config->eq_margin_inference));
      }
      break;
    case ARG_TOKEN_MOVEGEN_MARGIN:
      if (config->eq_margin_movegen != 0) {
        config_add_double_setting_to_string_builder(
            config, sb, arg_token, equity_to_double(config->eq_margin_movegen));
      }
      break;
    case ARG_TOKEN_MIN_PLAY_ITERATIONS:
      config_add_uint64_setting_to_string_builder(config, sb, arg_token,
                                                  config->min_play_iterations);
      break;
    case ARG_TOKEN_USE_GAME_PAIRS:
      config_add_bool_setting_to_string_builder(config, sb, arg_token,
                                                config->use_game_pairs);
      break;
    case ARG_TOKEN_USE_SMALL_PLAYS:
      config_add_bool_setting_to_string_builder(config, sb, arg_token,
                                                config->use_small_plays);
      break;
    case ARG_TOKEN_SIM_WITH_INFERENCE:
      config_add_bool_setting_to_string_builder(config, sb, arg_token,
                                                config->sim_with_inference);
      break;
    case ARG_TOKEN_USE_HEAT_MAP:
      config_add_bool_setting_to_string_builder(config, sb, arg_token,
                                                config->use_heat_map);
      break;
    case ARG_TOKEN_WRITE_BUFFER_SIZE:
      config_add_uint64_setting_to_string_builder(
          config, sb, arg_token,
          (uint64_t)autoplay_results_get_write_buffer_size(
              config->autoplay_results));
      break;
    case ARG_TOKEN_HUMAN_READABLE:
      config_add_bool_setting_to_string_builder(config, sb, arg_token,
                                                config->human_readable);
      break;
    case ARG_TOKEN_RANDOM_SEED:
      // Do not save the seed in the settings.
      // The seed should be explicitly set by the user if they want to
      // reproduce certain behaviors.
      break;
    case ARG_TOKEN_NUMBER_OF_THREADS:
      config_add_int_setting_to_string_builder(config, sb, arg_token,
                                               config->num_threads);
      break;
    case ARG_TOKEN_PRINT_INTERVAL:
      config_add_int_setting_to_string_builder(config, sb, arg_token,
                                               config->print_interval);
      break;
    case ARG_TOKEN_EXEC_MODE:
      string_builder_add_formatted_string(sb, " -%s ",
                                          config->pargs[arg_token]->name);
      string_builder_add_exec_mode_type(sb, config->exec_mode);
      break;
    case ARG_TOKEN_TT_FRACTION_OF_MEM:
      config_add_double_setting_to_string_builder(config, sb, arg_token,
                                                  config->tt_fraction_of_mem);
      break;
    case ARG_TOKEN_TIME_LIMIT:
      config_add_uint64_setting_to_string_builder(config, sb, arg_token,
                                                  config->time_limit_seconds);
      break;
    case ARG_TOKEN_SAMPLING_RULE:
      string_builder_add_formatted_string(sb, " -%s ",
                                          config->pargs[arg_token]->name);
      string_builder_add_sampling_rule(sb, config->sampling_rule);
      break;
    case ARG_TOKEN_THRESHOLD:
      string_builder_add_formatted_string(sb, " -%s ",
                                          config->pargs[arg_token]->name);
      string_builder_add_threshold(sb, config->threshold);
      break;
    case ARG_TOKEN_CUTOFF:
      config_add_double_setting_to_string_builder(
          config, sb, arg_token, convert_cutoff_to_user_cutoff(config->cutoff));
      break;
    case ARG_TOKEN_PRINT_BOARDS:
      config_add_bool_setting_to_string_builder(config, sb, arg_token,
                                                config->print_boards);
      break;
    case ARG_TOKEN_BOARD_COLOR:
      string_builder_add_formatted_string(sb, " -%s ",
                                          config->pargs[arg_token]->name);
      string_builder_add_game_string_board_color_type(
          sb, config->game_string_options->board_color);
      break;
    case ARG_TOKEN_BOARD_TILE_GLYPHS:
      string_builder_add_formatted_string(sb, " -%s ",
                                          config->pargs[arg_token]->name);
      string_builder_add_game_string_board_tile_glyphs_type(
          sb, config->game_string_options->board_tile_glyphs);
      break;
    case ARG_TOKEN_BOARD_BORDER:
      string_builder_add_formatted_string(sb, " -%s ",
                                          config->pargs[arg_token]->name);
      string_builder_add_game_string_board_border_type(
          sb, config->game_string_options->board_border);
      break;
    case ARG_TOKEN_BOARD_COLUMN_LABEL:
      string_builder_add_formatted_string(sb, " -%s ",
                                          config->pargs[arg_token]->name);
      string_builder_add_game_string_board_column_label_type(
          sb, config->game_string_options->board_column_label);
      break;
    case ARG_TOKEN_ON_TURN_MARKER:
      string_builder_add_formatted_string(sb, " -%s ",
                                          config->pargs[arg_token]->name);
      string_builder_add_game_string_on_turn_marker_type(
          sb, config->game_string_options->on_turn_marker);
      break;
    case ARG_TOKEN_ON_TURN_COLOR:
      string_builder_add_formatted_string(sb, " -%s ",
                                          config->pargs[arg_token]->name);
      string_builder_add_game_string_on_turn_color_type(
          sb, config->game_string_options->on_turn_color);
      break;
    case ARG_TOKEN_ON_TURN_SCORE_STYLE:
      string_builder_add_formatted_string(sb, " -%s ",
                                          config->pargs[arg_token]->name);
      string_builder_add_game_string_on_turn_score_style_type(
          sb, config->game_string_options->on_turn_score_style);
      break;
    case ARG_TOKEN_PRETTY:
      break;
    case ARG_TOKEN_PRINT_ON_FINISH:
      config_add_bool_setting_to_string_builder(config, sb, arg_token,
                                                config->print_on_finish);
      break;
    case ARG_TOKEN_SHOW_PROMPT:
      config_add_bool_setting_to_string_builder(config, sb, arg_token,
                                                config->show_prompt);
      break;
    case ARG_TOKEN_SAVE_SETTINGS:
      config_add_bool_setting_to_string_builder(config, sb, arg_token,
                                                config->save_settings);
      break;
    case ARG_TOKEN_AUTOSAVE_GCG:
      config_add_bool_setting_to_string_builder(config, sb, arg_token,
                                                config->autosave_gcg);
      break;
    case NUMBER_OF_ARG_TOKENS:
      log_fatal("encountered invalid arg token when saving settings");
      break;
    }
  }
}