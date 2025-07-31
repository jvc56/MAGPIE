#include "config.h"

#include "../def/autoplay_defs.h"
#include "../def/bai_defs.h"
#include "../def/config_defs.h"
#include "../def/equity_defs.h"
#include "../def/game_defs.h"
#include "../def/move_defs.h"
#include "../def/players_data_defs.h"
#include "../def/rack_defs.h"
#include "../def/thread_control_defs.h"
#include "../ent/autoplay_results.h"
#include "../ent/board.h"
#include "../ent/board_layout.h"
#include "../ent/conversion_results.h"
#include "../ent/equity.h"
#include "../ent/game.h"
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
#include "../str/sim_string.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "autoplay.h"
#include "cgp.h"
#include "convert.h"
#include "gameplay.h"
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
#include <stdlib.h>

typedef enum {
  ARG_TOKEN_SET,
  ARG_TOKEN_CGP,
  ARG_TOKEN_MOVES,
  ARG_TOKEN_RACK,
  ARG_TOKEN_GEN,
  ARG_TOKEN_SIM,
  ARG_TOKEN_INFER,
  ARG_TOKEN_AUTOPLAY,
  ARG_TOKEN_CONVERT,
  ARG_TOKEN_LEAVE_GEN,
  ARG_TOKEN_CREATE_DATA,
  ARG_TOKEN_DATA_PATH,
  ARG_TOKEN_BINGO_BONUS,
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
  ARG_TOKEN_P2_NAME,
  ARG_TOKEN_P2_LEXICON,
  ARG_TOKEN_P2_USE_WMP,
  ARG_TOKEN_P2_LEAVES,
  ARG_TOKEN_P2_MOVE_SORT_TYPE,
  ARG_TOKEN_P2_MOVE_RECORD_TYPE,
  ARG_TOKEN_WIN_PCT,
  ARG_TOKEN_PLIES,
  ARG_TOKEN_NUMBER_OF_PLAYS,
  ARG_TOKEN_NUMBER_OF_SMALL_PLAYS,
  ARG_TOKEN_MAX_ITERATIONS,
  ARG_TOKEN_STOP_COND_PCT,
  ARG_TOKEN_EQUITY_MARGIN,
  ARG_TOKEN_MAX_EQUITY_DIFF,
  ARG_TOKEN_MIN_PLAY_ITERATIONS,
  ARG_TOKEN_USE_GAME_PAIRS,
  ARG_TOKEN_USE_SMALL_PLAYS,
  ARG_TOKEN_WRITE_BUFFER_SIZE,
  ARG_TOKEN_HUMAN_READABLE,
  ARG_TOKEN_RANDOM_SEED,
  ARG_TOKEN_NUMBER_OF_THREADS,
  ARG_TOKEN_PRINT_INFO_INTERVAL,
  ARG_TOKEN_EXEC_MODE,
  ARG_TOKEN_TT_FRACTION_OF_MEM,
  ARG_TOKEN_TIME_LIMIT,
  ARG_TOKEN_SAMPLING_RULE,
  ARG_TOKEN_THRESHOLD,
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
  int num_plays;
  int num_small_plays;
  int plies;
  int max_iterations;
  int min_play_iterations;
  double stop_cond_pct;
  double equity_margin;
  Equity max_equity_diff;
  bool use_game_pairs;
  bool human_readable;
  bool use_small_plays;
  char *record_filepath;
  double tt_fraction_of_mem;
  int time_limit_seconds;
  bai_sampling_rule_t sampling_rule;
  bai_threshold_t threshold;
  game_variant_t game_variant;
  WinPct *win_pcts;
  BoardLayout *board_layout;
  LetterDistribution *ld;
  PlayersData *players_data;
  ThreadControl *thread_control;
  Game *game;
  MoveList *move_list;
  SimResults *sim_results;
  InferenceResults *inference_results;
  AutoplayResults *autoplay_results;
  ConversionResults *conversion_results;
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

int config_get_num_small_plays(const Config *config) {
  return config->num_small_plays;
}
int config_get_plies(const Config *config) { return config->plies; }

int config_get_max_iterations(const Config *config) {
  return config->max_iterations;
}

double config_get_stop_cond_pct(const Config *config) {
  return config->stop_cond_pct;
}

double config_get_equity_margin(const Config *config) {
  return config->equity_margin;
}

int config_get_time_limit_seconds(const Config *config) {
  return config->time_limit_seconds;
}

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

PlayersData *config_get_players_data(const Config *config) {
  return config->players_data;
}

LetterDistribution *config_get_ld(const Config *config) { return config->ld; }

ThreadControl *config_get_thread_control(const Config *config) {
  return config->thread_control;
}

Game *config_get_game(const Config *config) { return config->game; }

MoveList *config_get_move_list(const Config *config) {
  return config->move_list;
}

SimResults *config_get_sim_results(const Config *config) {
  return config->sim_results;
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
  game_args->seed = thread_control_get_seed(config->thread_control);
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
    error_stack_push(error_stack, error_code,
                     get_formatted_string("failed to parse value for %s: %s",
                                          int_str_name, int_str));
  }
  if (*dest < min || *dest > max) {
    error_stack_push(
        error_stack, error_code,
        get_formatted_string(
            "value for %s must be between %d and %d inclusive, but got %s",
            int_str_name, min, max, int_str));
  }
}

void load_rack_or_push_to_error_stack(const char *rack_str,
                                      const LetterDistribution *ld,
                                      error_code_t error_code, Rack *rack,
                                      ErrorStack *error_stack) {
  if (rack_set_to_string(ld, rack, rack_str) < 0) {
    error_stack_push(
        error_stack, error_code,
        get_formatted_string("failed to parse rack: %s", rack_str));
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

// Used for string api commands that return nothing
char* empty_string() {
  return string_duplicate("");
}

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
  if (thread_control_is_finished(config->thread_control)) {
    status_str = get_status_finished_str(config);
  } else {
    status_str = get_status_running_str(config);
  }
  return status_str;
}

// Load CGP

void impl_cgp_load(Config *config, ErrorStack *error_stack) {
  if (!config_has_game_data(config)) {
    error_stack_push(
        error_stack, ERROR_STATUS_CONFIG_LOAD_GAME_DATA_MISSING,
        string_duplicate(
            "cannot load cgp without letter distribution and lexicon"));
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
    error_stack_push(
        error_stack, ERROR_STATUS_CONFIG_LOAD_GAME_DATA_MISSING,
        string_duplicate(
            "cannot add moves without letter distribution and lexicon"));
    return;
  }

  config_init_game(config);

  const char *moves = config_get_parg_value(config, ARG_TOKEN_MOVES, 0);

  int player_on_turn_index = game_get_player_on_turn_index(config->game);

  ValidatedMoves *new_validated_moves =
      validated_moves_create(config->game, player_on_turn_index, moves, true,
                             false, false, error_stack);

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
    validated_moves_add_to_move_list(new_validated_moves, config->move_list);
  }

  validated_moves_destroy(new_validated_moves);
}

// Setting player rack

void impl_set_rack(Config *config, ErrorStack *error_stack) {
  if (!config_has_game_data(config)) {
    error_stack_push(
        error_stack, ERROR_STATUS_CONFIG_LOAD_GAME_DATA_MISSING,
        string_duplicate(
            "cannot set player rack without letter distribution and lexicon"));
    return;
  }

  config_init_game(config);

  int player_index;
  config_load_int(config, ARG_TOKEN_RACK, 1, 2, &player_index, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
  // Convert from 1-indexed user input to 0-indexed internal use
  player_index--;

  Rack *new_rack = rack_duplicate(
      player_get_rack(game_get_player(config->game, player_index)));
  rack_reset(new_rack);

  const char *rack_str = config_get_parg_value(config, ARG_TOKEN_RACK, 1);
  load_rack_or_push_to_error_stack(rack_str, config->ld,
                                   ERROR_STATUS_CONFIG_LOAD_MALFORMED_RACK_ARG,
                                   new_rack, error_stack);

  if (error_stack_is_empty(error_stack)) {
    if (rack_is_drawable(config->game, player_index, new_rack)) {
      return_rack_to_bag(config->game, player_index);
      if (!draw_rack_from_bag(config->game, player_index, new_rack)) {
        log_fatal("failed to draw rack from bag in set rack command");
      }
    } else {
      error_stack_push(error_stack, ERROR_STATUS_CONFIG_LOAD_RACK_NOT_IN_BAG,
                       get_formatted_string(
                           "rack %s is not available in the bag", rack_str));
    }
  }

  rack_destroy(new_rack);
}

// Move generation

void impl_move_gen(Config *config, ErrorStack *error_stack) {
  if (!config_has_game_data(config)) {
    error_stack_push(
        error_stack, ERROR_STATUS_CONFIG_LOAD_GAME_DATA_MISSING,
        string_duplicate(
            "cannot generate moves without letter distribution and lexicon"));
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
  MoveList *ml = config->move_list;
  const MoveGenArgs args = {
      .game = config->game,
      .move_list = ml,
      .thread_index = 0,
      .max_equity_diff = config->max_equity_diff,
  };
  generate_moves_for_game(&args);
}

// Sim

void config_fill_sim_args(const Config *config, Rack *known_opp_rack,
                          SimArgs *sim_args) {
  sim_args->num_plies = config_get_plies(config);
  sim_args->move_list = config_get_move_list(config);
  sim_args->known_opp_rack = known_opp_rack;
  sim_args->win_pcts = config_get_win_pcts(config);
  sim_args->thread_control = config->thread_control;
  sim_args->game = config_get_game(config);
  sim_args->move_list = config_get_move_list(config);
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
}

void config_simulate(const Config *config, Rack *known_opp_rack,
                     SimResults *sim_results, ErrorStack *error_stack) {
  SimArgs args;
  config_fill_sim_args(config, known_opp_rack, &args);
  return simulate(&args, sim_results, error_stack);
}

void impl_sim(Config *config, ErrorStack *error_stack) {
  if (!config_has_game_data(config)) {
    error_stack_push(
        error_stack, ERROR_STATUS_CONFIG_LOAD_GAME_DATA_MISSING,
        string_duplicate(
            "cannot simulate without letter distribution and lexicon"));
    return;
  }

  config_init_game(config);

  const char *known_opp_rack_str =
      config_get_parg_value(config, ARG_TOKEN_SIM, 0);
  Rack *known_opp_rack = NULL;

  if (known_opp_rack_str) {
    known_opp_rack = rack_create(ld_get_size(game_get_ld(config->game)));
    load_rack_or_push_to_error_stack(
        known_opp_rack_str, game_get_ld(config->game),
        ERROR_STATUS_CONFIG_LOAD_MALFORMED_RACK_ARG, known_opp_rack,
        error_stack);
    if (!error_stack_is_empty(error_stack)) {
      rack_destroy(known_opp_rack);
      return;
    }
  }

  config_simulate(config, known_opp_rack, config->sim_results, error_stack);
  rack_destroy(known_opp_rack);
}

char *status_sim(Config *config) {
  SimResults *sim_results = config->sim_results;
  if (!sim_results) {
    return string_duplicate("simmer has not been initialized");
  }
  char *status_str = NULL;
  if (thread_control_is_sim_printable(
          config->thread_control,
          sim_results_get_simmed_plays_initialized(sim_results))) {
    status_str = ucgi_sim_stats(
        config->game, sim_results,
        (double)sim_results_get_node_count(sim_results) /
            thread_control_get_seconds_elapsed(config->thread_control),
        true);
  } else {
    status_str = string_duplicate("simmer status not yet available");
  }
  return status_str;
}

// Inference

void config_fill_infer_args(const Config *config, int target_index,
                            int target_score, int target_num_exch,
                            Rack *target_played_tiles, InferenceArgs *args) {
  args->target_index = target_index;
  args->target_score = target_score;
  args->target_num_exch = target_num_exch;
  args->move_capacity = config_get_num_plays(config);
  args->equity_margin = config_get_equity_margin(config);
  args->target_played_tiles = target_played_tiles;
  args->game = config_get_game(config);
  args->thread_control = config->thread_control;
}

void config_infer(const Config *config, int target_index, int target_score,
                  int target_num_exch, Rack *target_played_tiles,
                  InferenceResults *results, ErrorStack *error_stack) {
  InferenceArgs args;
  config_fill_infer_args(config, target_index, target_score, target_num_exch,
                         target_played_tiles, &args);
  return infer(&args, results, error_stack);
}

void config_infer_with_rack(Config *config, Rack *target_played_tiles,
                            ErrorStack *error_stack) {
  const char *target_index_str =
      config_get_parg_value(config, ARG_TOKEN_INFER, 0);
  int target_index;
  string_to_int_or_push_error("inferred player", target_index_str, 1, 2,
                              ERROR_STATUS_CONFIG_LOAD_MALFORMED_INT_ARG,
                              &target_index, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
  // Convert from 1-indexed to 0-indexed
  target_index--;

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
    const LetterDistribution *ld = game_get_ld(config->game);
    load_rack_or_push_to_error_stack(
        target_played_tiles_or_num_exch_str, ld,
        ERROR_STATUS_CONFIG_LOAD_MALFORMED_RACK_ARG, target_played_tiles,
        error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
    is_tile_placement_move = true;
  }

  Equity target_score = 0;

  if (is_tile_placement_move) {
    const char *target_score_str =
        config_get_parg_value(config, ARG_TOKEN_INFER, 2);
    if (!target_score_str) {
      error_stack_push(error_stack, ERROR_STATUS_CONFIG_LOAD_MISSING_ARG,
                       string_duplicate("missing inferred player score"));
      return;
    }
    string_to_int_or_push_error(
        "inferred player score", target_score_str, 0, INT_MAX,
        ERROR_STATUS_CONFIG_LOAD_MALFORMED_INT_ARG, &target_score, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
  }

  config_infer(config, target_index, target_score, target_num_exch,
               target_played_tiles, config->inference_results, error_stack);
}

void impl_infer(Config *config, ErrorStack *error_stack) {
  if (!config_has_game_data(config)) {
    error_stack_push(
        error_stack, ERROR_STATUS_CONFIG_LOAD_GAME_DATA_MISSING,
        string_duplicate(
            "cannot infer without letter distribution and lexicon"));
    return;
  }

  config_init_game(config);
  Rack *target_played_tiles =
      rack_create(ld_get_size(game_get_ld(config->game)));
  config_infer_with_rack(config, target_played_tiles, error_stack);
  rack_destroy(target_played_tiles);
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
  autoplay_args->thread_control = config_get_thread_control(config);
  autoplay_args->data_paths = config_get_data_paths(config);
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
    error_stack_push(
        error_stack, ERROR_STATUS_CONFIG_LOAD_GAME_DATA_MISSING,
        string_duplicate(
            "cannot autoplay without letter distribution and lexicon"));
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
        string_duplicate(
            "cannot generate leaves without letter distribution and lexicon"));
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
void impl_create_data(Config *config, ErrorStack *error_stack) {
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

      for (int k = 0; k < NUMBER_OF_ARG_TOKENS; k++) {
        if (has_prefix(arg_name, config->pargs[k]->name)) {
          if (current_parg) {
            error_stack_push(
                error_stack, ERROR_STATUS_CONFIG_LOAD_AMBIGUOUS_COMMAND,
                get_formatted_string("ambiguous command %s, could be %s or %s",
                                     arg_name, current_parg->name,
                                     config->pargs[k]->name));
            return;
          }
          current_parg = config->pargs[k];
          current_arg_token = k;
        }
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

bool is_lexicon_required(const char *new_p1_leaves_name,
                         const char *new_p2_leaves_name,
                         const char *new_ld_name, const bool use_wmp1,
                         const bool use_wmp2) {
  return new_p1_leaves_name || new_p2_leaves_name || new_ld_name || use_wmp1 ||
         use_wmp2;
}

bool lex_lex_compat(const char *p1_lexicon_name, const char *p2_lexicon_name,
                    ErrorStack *error_stack) {
  if (!p1_lexicon_name && !p2_lexicon_name) {
    return true;
  }
  if (!p1_lexicon_name || !p2_lexicon_name) {
    return false;
  }
  return ld_types_compat(
      ld_get_type_from_lex_name(p1_lexicon_name, error_stack),
      ld_get_type_from_lex_name(p2_lexicon_name, error_stack));
}

bool lex_ld_compat(const char *lexicon_name, const char *ld_name,
                   ErrorStack *error_stack) {
  if (!lexicon_name && !ld_name) {
    return true;
  }
  if (!lexicon_name || !ld_name) {
    return false;
  }
  return ld_types_compat(ld_get_type_from_lex_name(lexicon_name, error_stack),
                         ld_get_type_from_ld_name(ld_name, error_stack));
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
  if (has_iprefix(exec_mode_str, "console")) {
    exec_mode = EXEC_MODE_CONSOLE;
  } else if (has_iprefix(exec_mode_str, "ucgi")) {
    exec_mode = EXEC_MODE_UCGI;
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
  bool p1_use_wmp = !!players_data_get_data_name(config->players_data,
                                                 PLAYERS_DATA_TYPE_WMP, 0);
  bool p2_use_wmp = !!players_data_get_data_name(config->players_data,
                                                 PLAYERS_DATA_TYPE_WMP, 1);

  if (config_get_parg_value(config, ARG_TOKEN_USE_WMP, 0)) {
    config_load_bool(config, ARG_TOKEN_USE_WMP, &p1_use_wmp, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
    p2_use_wmp = p1_use_wmp;
  }

  // The "w1" and "w2" args override the "use_wmp" arg
  if (config_get_parg_value(config, ARG_TOKEN_P1_USE_WMP, 0)) {
    config_load_bool(config, ARG_TOKEN_P1_USE_WMP, &p1_use_wmp, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
  }

  if (config_get_parg_value(config, ARG_TOKEN_P2_USE_WMP, 0)) {
    config_load_bool(config, ARG_TOKEN_P2_USE_WMP, &p2_use_wmp, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
  }

  // Both lexicons are not specified, so we don't
  // load any of the lexicon dependent data
  if (!updated_p1_lexicon_name && !updated_p2_lexicon_name) {
    // We can use the new_* variables here since if lexicons
    // are null, it is guaranteed that there are no leaves or ld
    // since they are all set after this if check.
    if (is_lexicon_required(new_p1_leaves_name, new_p2_leaves_name, new_ld_name,
                            p1_use_wmp, p2_use_wmp)) {
      error_stack_push(
          error_stack, ERROR_STATUS_CONFIG_LOAD_LEXICON_MISSING,
          string_duplicate("cannot set leaves, letter distribition, or word "
                           "maps without a lexicon"));
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
  if (p1_use_wmp) {
    p1_wmp_name = updated_p1_lexicon_name;
  }

  const char *p2_wmp_name = NULL;
  if (p2_use_wmp) {
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

  config_load_int(config, ARG_TOKEN_PLIES, 0, INT_MAX, &config->plies,
                  error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  config_load_int(config, ARG_TOKEN_NUMBER_OF_PLAYS, 0, INT_MAX,
                  &config->num_plays, error_stack);
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

  int number_of_threads = -1;
  config_load_int(config, ARG_TOKEN_NUMBER_OF_THREADS, 1, MAX_THREADS,
                  &number_of_threads, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  if (number_of_threads > 0) {
    thread_control_set_threads(config->thread_control, number_of_threads);
  }

  int print_info_interval = -1;
  config_load_int(config, ARG_TOKEN_PRINT_INFO_INTERVAL, 0, INT_MAX,
                  &print_info_interval, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  if (print_info_interval >= 0) {
    thread_control_set_print_info_interval(config->thread_control,
                                           print_info_interval);
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

  config_load_double(config, ARG_TOKEN_EQUITY_MARGIN, 0, 1e100,
                     &config->equity_margin, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  const char *new_max_equity_diff_double =
      config_get_parg_value(config, ARG_TOKEN_MAX_EQUITY_DIFF, 0);
  if (new_max_equity_diff_double) {
    double max_equity_diff_double = NAN;
    config_load_double(config, ARG_TOKEN_MAX_EQUITY_DIFF, 0, EQUITY_MAX_DOUBLE,
                       &max_equity_diff_double, error_stack);
    if (!error_stack_is_empty(error_stack)) {
      return;
    }
    assert(!isnan(max_equity_diff_double));
    config->max_equity_diff = double_to_equity(max_equity_diff_double);
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

  // Human readable

  config_load_bool(config, ARG_TOKEN_HUMAN_READABLE, &config->human_readable,
                   error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
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

  uint64_t seed = 0;
  config_load_uint64(config, ARG_TOKEN_RANDOM_SEED, &seed, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    return;
  }

  if (!config_get_parg_value(config, ARG_TOKEN_RANDOM_SEED, 0)) {
    thread_control_increment_seed(config->thread_control);
  } else {
    thread_control_set_seed(config->thread_control, seed);
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

bool config_execute_command_silent(Config *config, ErrorStack *error_stack) {
  if (config_exec_parg_is_set(config)) {
    config_get_parg_exec_func(config, config->exec_parg_token)(config,
                                                               error_stack);
    return true;
  } else {
    return false;
  }
}

void config_execute_command(Config *config, ErrorStack *error_stack) {
  if (config_execute_command_silent(config, error_stack)) {
    char *finished_msg = get_status_finished_str(config);
    thread_control_print(config_get_thread_control(config), finished_msg);
    free(finished_msg);
  }
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


void execute_cgp_load(Config *config, ErrorStack *error_stack) {
  impl_cgp_load(config, error_stack);
}

char *str_api_cgp_load(Config *config, ErrorStack *error_stack) {
  impl_cgp_load(config, error_stack);
  return empty_string();
}

void execute_add_moves(Config *config, ErrorStack *error_stack) {
  impl_add_moves(config, error_stack);
}

char* str_api_add_moves(Config *config, ErrorStack *error_stack) {
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
  print_ucgi_static_moves(config->game, config->move_list, config->thread_control);
}

char *str_api_move_gen(Config *config, ErrorStack *error_stack) {
  impl_move_gen(config, error_stack);
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
}

char *str_api_infer(Config *config, ErrorStack *error_stack) {
  impl_infer(config, error_stack);
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
#define cmd(token, name, n_req, n_val, func, stat) \
  parsed_arg_create(config, token, name, n_req, n_val, \
  execute_##func, str_api_##func, status_##stat)

  // Non-command arg
#define arg(token, name, n_req, n_val) \
  parsed_arg_create(config, token, name, n_req, n_val, \
  execute_fatal, str_api_fatal, status_generic)

  cmd(ARG_TOKEN_SET, "setoptions", 0, 0, noop, generic);
  cmd(ARG_TOKEN_CGP, "cgp", 4, 4, cgp_load, generic);
  cmd(ARG_TOKEN_MOVES, "addmoves", 1, 1, add_moves, generic);
  cmd(ARG_TOKEN_RACK, "rack", 2, 2, set_rack, generic);
  cmd(ARG_TOKEN_GEN, "generate", 0, 0, move_gen, generic);
  cmd(ARG_TOKEN_SIM, "simulate", 0, 1, sim, sim);
  cmd(ARG_TOKEN_INFER, "infer", 2, 3, infer, generic);
  cmd(ARG_TOKEN_AUTOPLAY, "autoplay", 2, 2, autoplay, generic);
  cmd(ARG_TOKEN_CONVERT, "convert", 2, 3, convert, generic);
  cmd(ARG_TOKEN_LEAVE_GEN, "leavegen", 2, 2, leave_gen, generic);
  cmd(ARG_TOKEN_CREATE_DATA, "createdata", 2, 3, create_data, generic);

  arg(ARG_TOKEN_DATA_PATH, "path", 1, 1);
  arg(ARG_TOKEN_BINGO_BONUS, "bb", 1, 1);
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
  arg(ARG_TOKEN_P2_NAME, "p2", 1, 1);
  arg(ARG_TOKEN_P2_LEXICON, "l2", 1, 1);
  arg(ARG_TOKEN_P2_USE_WMP, "w2", 1, 1);
  arg(ARG_TOKEN_P2_LEAVES, "k2", 1, 1);
  arg(ARG_TOKEN_P2_MOVE_SORT_TYPE, "s2", 1, 1);
  arg(ARG_TOKEN_P2_MOVE_RECORD_TYPE, "r2", 1, 1);
  arg(ARG_TOKEN_WIN_PCT, "winpct", 1, 1);
  arg(ARG_TOKEN_PLIES, "plies", 1, 1);
  arg(ARG_TOKEN_NUMBER_OF_PLAYS, "numplays", 1, 1);
  arg(ARG_TOKEN_NUMBER_OF_SMALL_PLAYS, "numsmallplays", 1, 1);
  arg(ARG_TOKEN_MAX_ITERATIONS, "iterations", 1, 1);
  arg(ARG_TOKEN_MIN_PLAY_ITERATIONS, "minplayiterations", 1, 1);
  arg(ARG_TOKEN_STOP_COND_PCT, "scondition", 1, 1);
  arg(ARG_TOKEN_EQUITY_MARGIN, "equitymargin", 1, 1);
  arg(ARG_TOKEN_MAX_EQUITY_DIFF, "maxequitydifference", 1, 1);
  arg(ARG_TOKEN_USE_GAME_PAIRS, "gp", 1, 1);
  arg(ARG_TOKEN_USE_SMALL_PLAYS, "sp", 1, 1);
  arg(ARG_TOKEN_HUMAN_READABLE, "hr", 1, 1);
  arg(ARG_TOKEN_WRITE_BUFFER_SIZE, "wb", 1, 1);
  arg(ARG_TOKEN_RANDOM_SEED, "seed", 1, 1);
  arg(ARG_TOKEN_NUMBER_OF_THREADS, "threads", 1, 1);
  arg(ARG_TOKEN_PRINT_INFO_INTERVAL, "pfrequency", 1, 1);
  arg(ARG_TOKEN_EXEC_MODE, "mode", 1, 1);
  arg(ARG_TOKEN_TT_FRACTION_OF_MEM, "ttfraction", 1, 1);
  arg(ARG_TOKEN_TIME_LIMIT, "tlim", 1, 1);
  arg(ARG_TOKEN_SAMPLING_RULE, "sr", 1, 1);
  arg(ARG_TOKEN_THRESHOLD, "threshold", 1, 1);

#undef cmd
#undef arg

  config->exec_parg_token = NUMBER_OF_ARG_TOKENS;
  config->ld_changed = false;
  config->exec_mode = EXEC_MODE_CONSOLE;
  config->bingo_bonus = DEFAULT_BINGO_BONUS;
  config->num_plays = DEFAULT_MOVE_LIST_CAPACITY;
  config->num_small_plays = DEFAULT_SMALL_MOVE_LIST_CAPACITY;
  config->plies = 2;
  config->equity_margin = 0;
  config->max_equity_diff = int_to_equity(10);
  config->min_play_iterations = 100;
  config->max_iterations = 5000;
  config->stop_cond_pct = 99;
  config->time_limit_seconds = 0;
  config->sampling_rule = BAI_SAMPLING_RULE_TOP_TWO;
  config->threshold = BAI_THRESHOLD_GK16;
  config->use_game_pairs = false;
  config->use_small_plays = false;
  config->human_readable = false;
  config->game_variant = DEFAULT_GAME_VARIANT;
  config->ld = NULL;
  config->players_data = players_data_create();
  config->thread_control = thread_control_create();
  config->game = NULL;
  config->move_list = NULL;
  config->sim_results = sim_results_create();
  config->inference_results = inference_results_create();
  config->autoplay_results = autoplay_results_create();
  config->conversion_results = conversion_results_create();
  config->tt_fraction_of_mem = 0.25;

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
  move_list_destroy(config->move_list);
  sim_results_destroy(config->sim_results);
  inference_results_destroy(config->inference_results);
  autoplay_results_destroy(config->autoplay_results);
  conversion_results_destroy(config->conversion_results);
  free(config->data_paths);
  free(config);
}
