#include "config.h"

#include <ctype.h>
#include <float.h>
#include <limits.h>
#include <stdbool.h>

#include "../def/autoplay_defs.h"
#include "../def/config_defs.h"
#include "../def/error_status_defs.h"
#include "../def/exec_defs.h"
#include "../def/file_handler_defs.h"
#include "../def/game_defs.h"
#include "../def/gen_defs.h"
#include "../def/inference_defs.h"
#include "../def/simmer_defs.h"
#include "../def/thread_control_defs.h"
#include "../def/validated_move_defs.h"
#include "../def/win_pct_defs.h"

#include "../ent/error_status.h"
#include "../ent/game.h"
#include "../ent/letter_distribution.h"
#include "../ent/players_data.h"
#include "../ent/thread_control.h"
#include "../ent/validated_move.h"

#include "autoplay.h"
#include "cgp.h"
#include "gameplay.h"
#include "inference.h"
#include "kwg_maker.h"
#include "simmer.h"

#include "../str/game_string.h"
#include "../str/move_string.h"
#include "../str/sim_string.h"

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
  ARG_TOKEN_BINGO_BONUS,
  ARG_TOKEN_BOARD_LAYOUT,
  ARG_TOKEN_GAME_VARIANT,
  ARG_TOKEN_LETTER_DISTRIBUTION,
  ARG_TOKEN_LEXICON,
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
  ARG_TOKEN_WIN_PCT,
  ARG_TOKEN_PLIES,
  ARG_TOKEN_NUMBER_OF_PLAYS,
  ARG_TOKEN_MAX_ITERATIONS,
  ARG_TOKEN_STOP_COND_PCT,
  ARG_TOKEN_EQUITY_MARGIN,
  ARG_TOKEN_USE_GAME_PAIRS,
  ARG_TOKEN_RANDOM_SEED,
  ARG_TOKEN_NUMBER_OF_THREADS,
  ARG_TOKEN_PRINT_INFO_INTERVAL,
  ARG_TOKEN_CHECK_STOP_INTERVAL,
  ARG_TOKEN_INFILE,
  ARG_TOKEN_OUTFILE,
  ARG_TOKEN_EXEC_MODE,
  // This must always be the last
  // token for the count to be accurate
  NUMBER_OF_ARG_TOKENS
} arg_token_t;

typedef void (*command_exec_func_t)(Config *);
typedef char *(*command_status_func_t)(Config *);

typedef struct ParsedArg {
  char *name;
  char **values;
  int num_req_values;
  int num_values;
  int num_set_values;
  command_exec_func_t exec_func;
  command_status_func_t status_func;
} ParsedArg;

struct Config {
  ParsedArg *pargs[NUMBER_OF_ARG_TOKENS];
  arg_token_t exec_parg_token;
  bool ld_changed;
  exec_mode_t exec_mode;
  int bingo_bonus;
  int num_plays;
  int plies;
  int max_iterations;
  double stop_cond_pct;
  double equity_margin;
  bool use_game_pairs;
  uint64_t seed;
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
  ErrorStatus *error_status;
};

void parsed_arg_create(Config *config, arg_token_t arg_token, const char *name,
                       int num_req_values, int num_values,
                       command_exec_func_t command_exec_func,
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

command_status_func_t config_get_parg_status_func(const Config *config,
                                                  arg_token_t arg_token) {
  return config_get_parg(config, arg_token)->status_func;
}

// Returns NULL if the value was not set in the most recent config load call.
const char *config_get_parg_value(const Config *config, arg_token_t arg_token,
                                  int value_index) {
  ParsedArg *parg = config_get_parg(config, arg_token);
  if (value_index >= parg->num_values) {
    log_fatal("value index exceeds number of values for %d: %d >= %d\n",
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

exec_mode_t config_get_exec_mode(const Config *config) {
  return config->exec_mode;
}

int config_get_bingo_bonus(const Config *config) { return config->bingo_bonus; }

int config_get_num_plays(const Config *config) { return config->num_plays; }

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

uint64_t config_get_seed(const Config *config) { return config->seed; }

PlayersData *config_get_players_data(const Config *config) {
  return config->players_data;
}

LetterDistribution *config_get_ld(const Config *config) { return config->ld; }

ThreadControl *config_get_thread_control(const Config *config) {
  return config->thread_control;
}

ErrorStatus *config_get_error_status(const Config *config) {
  return config->error_status;
}

Game *config_get_game(const Config *config) { return config->game; }

MoveList *config_get_move_list(const Config *config) {
  return config->move_list;
}

SimResults *config_get_sim_results(const Config *config) {
  return config->sim_results;
}

bool config_exec_parg_is_set(const Config *config) {
  return config->exec_parg_token != NUMBER_OF_ARG_TOKENS;
}

bool config_continue_on_coldstart(const Config *config) {
  return !config_exec_parg_is_set(config) ||
         config->exec_parg_token == ARG_TOKEN_CGP ||
         config_get_parg_num_set_values(config, ARG_TOKEN_INFILE) > 0 ||
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
  game_args->game_variant = config->game_variant;
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

void config_recreate_move_list(Config *config, int capacity) {
  config_init_move_list(config, capacity);
  if (move_list_get_capacity(config->move_list) == capacity) {
    move_list_reset(config->move_list);
  } else {
    move_list_destroy(config->move_list);
    config->move_list = move_list_create(capacity);
  }
}

bool game_exists(const Game *game, ErrorStatus *error_status,
                 error_status_t error_status_type, int status) {
  bool exists = true;
  if (!game) {
    set_or_clear_error_status(error_status, error_status_type, status);
    exists = false;
  }
  return exists;
}

bool string_to_int_or_set_error_status(const char *int_str, int min, int max,
                                       ErrorStatus *error_status,
                                       error_status_t error_status_type,
                                       int status, int *dest) {
  bool success = true;
  *dest = string_to_int_or_set_error(int_str, &success);
  if (!success || *dest < min || *dest > max) {
    set_or_clear_error_status(error_status, error_status_type, status);
    success = false;
  }
  return success;
}

bool load_rack_or_set_error_status(const char *rack_str,
                                   const LetterDistribution *ld,
                                   ErrorStatus *error_status,
                                   error_status_t error_status_type, int status,
                                   Rack *rack) {
  bool success = true;
  if (rack_set_to_string(ld, rack, rack_str) < 0) {
    set_or_clear_error_status(error_status, error_status_type, status);
    success = false;
  }
  return success;
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

config_load_status_t config_load_int(Config *config, arg_token_t arg_token,
                                     int min, int max, int *value) {
  const char *int_str = config_get_parg_value(config, arg_token, 0);
  if (int_str) {
    bool success = false;
    int new_value = string_to_int_or_set_error(int_str, &success);
    if (!success) {
      return CONFIG_LOAD_STATUS_MALFORMED_INT_ARG;
    }
    if (new_value < min || new_value > max) {
      return CONFIG_LOAD_STATUS_INT_ARG_OUT_OF_BOUNDS;
    }
    *value = new_value;
  }
  return CONFIG_LOAD_STATUS_SUCCESS;
}

config_load_status_t config_load_double(Config *config, arg_token_t arg_token,
                                        double min, double max, double *value) {
  const char *double_str = config_get_parg_value(config, arg_token, 0);
  if (double_str) {
    bool success = false;
    double new_value = string_to_double_or_set_error(double_str, &success);
    if (!success) {
      return CONFIG_LOAD_STATUS_MALFORMED_DOUBLE_ARG;
    }
    if (new_value < min || new_value > max) {
      return CONFIG_LOAD_STATUS_DOUBLE_ARG_OUT_OF_BOUNDS;
    }
    *value = new_value;
  }
  return CONFIG_LOAD_STATUS_SUCCESS;
}

config_load_status_t config_load_bool(Config *config, arg_token_t arg_token,
                                      bool *value) {
  const char *bool_str = config_get_parg_value(config, arg_token, 0);
  if (bool_str) {
    if (has_iprefix(bool_str, "true")) {
      *value = true;
    } else if (has_iprefix(bool_str, "false")) {
      *value = false;
    } else {
      return CONFIG_LOAD_STATUS_MALFORMED_BOOL_ARG;
    }
  }
  return CONFIG_LOAD_STATUS_SUCCESS;
}

config_load_status_t config_load_uint64(Config *config, arg_token_t arg_token,
                                        uint64_t *value) {
  const char *int_str = config_get_parg_value(config, arg_token, 0);
  if (int_str) {
    if (!is_all_digits_or_empty(int_str)) {
      return CONFIG_LOAD_STATUS_MALFORMED_INT_ARG;
    }
    bool success = false;
    uint64_t new_value = string_to_uint64_or_set_error(int_str, &success);
    if (!success) {
      return CONFIG_LOAD_STATUS_MALFORMED_INT_ARG;
    }
    *value = new_value;
  }
  return CONFIG_LOAD_STATUS_SUCCESS;
}

// Generic execution and status functions

// Used for pargs that are not commands.
void execute_fatal(Config *config) {
  log_fatal("attempted to execute nonexecutable argument (arg token %d)",
            config->exec_parg_token);
}

// Used for commands that only update the config state
void execute_noop(Config __attribute__((unused)) * config) { return; }

// Used for pargs that are not commands.
char *status_fatal(Config *config) {
  log_fatal("attempted to get status of nonexecutable argument (arg token %d)",
            config->exec_parg_token);
  return NULL;
}

// Load CGP

void execute_cgp_load(Config *config) {
  if (!config_has_game_data(config)) {
    set_or_clear_error_status(config->error_status,
                              ERROR_STATUS_TYPE_CONFIG_LOAD,
                              CONFIG_LOAD_STATUS_GAME_DATA_MISSING);
    return;
  }

  config_init_game(config);

  StringBuilder *cgp_builder = create_string_builder();
  int num_req_values = config_get_parg_num_req_values(config, ARG_TOKEN_CGP);
  for (int i = 0; i < num_req_values; i++) {
    const char *cgp_component = config_get_parg_value(config, ARG_TOKEN_CGP, i);
    if (!cgp_component) {
      // This should have been caught earlier by the insufficient values error
      log_fatal("missing cgp component %d\n", i);
    }
    string_builder_add_string(cgp_builder, cgp_component);
    string_builder_add_char(cgp_builder, ' ');
  }

  const char *cgp = string_builder_peek(cgp_builder);
  // First duplicate the game so that potential
  // cgp parse failures don't corrupt it.
  Game *game_dupe = game_duplicate(config->game);
  cgp_parse_status_t dupe_cgp_parse_status = game_load_cgp(game_dupe, cgp);
  set_or_clear_error_status(config->error_status, ERROR_STATUS_TYPE_CGP_LOAD,
                            (int)dupe_cgp_parse_status);
  game_destroy(game_dupe);
  if (dupe_cgp_parse_status == CGP_PARSE_STATUS_SUCCESS) {
    // Now that the duplicate game has been successfully loaded
    // with the cgp, load the actual game. A cgp parse failure
    // here should be impossible (since the duplicated game
    // was parsed without error) and is treated as a
    // catastrophic error.
    cgp_parse_status_t cgp_parse_status = game_load_cgp(config->game, cgp);
    if (cgp_parse_status != CGP_PARSE_STATUS_SUCCESS) {
      log_fatal("unexpected cgp load failure for: %s", cgp);
    }
    config_reset_move_list(config);
  }
  destroy_string_builder(cgp_builder);
}

char *status_cgp_load(Config __attribute__((unused)) * config) {
  return string_duplicate("no status available for cgp load");
}

// Adding moves

void execute_add_moves(Config *config) {
  if (!config_has_game_data(config)) {
    set_or_clear_error_status(config->error_status,
                              ERROR_STATUS_TYPE_CONFIG_LOAD,
                              CONFIG_LOAD_STATUS_GAME_DATA_MISSING);
    return;
  }

  config_init_game(config);

  const char *moves = config_get_parg_value(config, ARG_TOKEN_MOVES, 0);

  int player_on_turn_index = game_get_player_on_turn_index(config->game);

  ValidatedMoves *new_validated_moves = validated_moves_create(
      config->game, player_on_turn_index, moves, true, false, false);

  move_validation_status_t move_validation_status =
      validated_moves_get_validation_status(new_validated_moves);

  if (move_validation_status == MOVE_VALIDATION_STATUS_SUCCESS) {
    const LetterDistribution *ld = game_get_ld(config->game);
    const Board *board = game_get_board(config->game);
    StringBuilder *phonies_sb = create_string_builder();
    int number_of_new_moves =
        validated_moves_get_number_of_moves(new_validated_moves);
    for (int i = 0; i < number_of_new_moves; i++) {
      char *phonies_formed = validated_moves_get_phonies_string(
          game_get_ld(config->game), new_validated_moves, i);
      if (phonies_formed) {
        string_builder_clear(phonies_sb);
        string_builder_add_string(phonies_sb, "Phonies formed from ");
        string_builder_add_move(
            board, validated_moves_get_move(new_validated_moves, i), ld,
            phonies_sb);
        string_builder_add_string(phonies_sb, ": ");
        string_builder_add_string(phonies_sb, phonies_formed);
        log_warn(string_builder_peek(phonies_sb));
      }
      free(phonies_formed);
    }
    destroy_string_builder(phonies_sb);
    config_init_move_list(config, number_of_new_moves);
    validated_moves_add_to_move_list(new_validated_moves, config->move_list);
  }

  validated_moves_destroy(new_validated_moves);

  set_or_clear_error_status(config->error_status,
                            ERROR_STATUS_TYPE_MOVE_VALIDATION,
                            (int)move_validation_status);
}

char *status_add_moves(Config __attribute__((unused)) * config) {
  return string_duplicate("no status available for adding moves");
}

// Setting player rack

void execute_set_rack(Config *config) {
  if (!config_has_game_data(config)) {
    set_or_clear_error_status(config->error_status,
                              ERROR_STATUS_TYPE_CONFIG_LOAD,
                              CONFIG_LOAD_STATUS_GAME_DATA_MISSING);
    return;
  }

  config_init_game(config);

  int player_index;
  config_load_status_t config_load_status =
      config_load_int(config, ARG_TOKEN_RACK, 1, 2, &player_index);
  if (config_load_status != CONFIG_LOAD_STATUS_SUCCESS) {
    set_or_clear_error_status(config->error_status,
                              ERROR_STATUS_TYPE_CONFIG_LOAD,
                              (int)config_load_status);
    return;
  }
  // Convert from 1-indexed user input to 0-indexed internal use
  player_index--;

  Rack *player_rack =
      player_get_rack(game_get_player(config->game, player_index));
  Rack *new_rack = rack_duplicate(player_rack);
  rack_reset(new_rack);

  load_rack_or_set_error_status(
      config_get_parg_value(config, ARG_TOKEN_RACK, 1), config->ld,
      config->error_status, ERROR_STATUS_TYPE_CONFIG_LOAD,
      CONFIG_LOAD_STATUS_MALFORMED_RACK_ARG, new_rack);

  if (error_status_get_success(config->error_status)) {
    Bag *bag = game_get_bag(config->game);
    if (rack_is_drawable(bag, player_rack, new_rack)) {
      int player_draw_index =
          game_get_player_draw_index(config->game, player_index);
      return_rack_to_bag(player_rack, bag, player_draw_index);
      if (!draw_rack_from_bag(bag, player_rack, new_rack, player_draw_index)) {
        log_fatal("failed to draw rack from bag in set rack command");
      }
    } else {
      set_or_clear_error_status(config->error_status,
                                ERROR_STATUS_TYPE_CONFIG_LOAD,
                                CONFIG_LOAD_STATUS_RACK_NOT_IN_BAG);
    }
  }

  rack_destroy(new_rack);
}

char *status_set_rack(Config __attribute__((unused)) * config) {
  return string_duplicate("no status available for setting player rack");
}

// Move generation

void execute_move_gen(Config *config) {
  if (!config_has_game_data(config)) {
    set_or_clear_error_status(config->error_status,
                              ERROR_STATUS_TYPE_CONFIG_LOAD,
                              CONFIG_LOAD_STATUS_GAME_DATA_MISSING);
    return;
  }

  config_init_game(config);
  config_recreate_move_list(config, config_get_num_plays(config));
  MoveList *ml = config->move_list;
  generate_moves_for_game(config->game, 0, ml);
  print_ucgi_static_moves(config->game, ml, config->thread_control);
  set_or_clear_error_status(config->error_status, ERROR_STATUS_TYPE_NONE, 0);
}

char *status_move_gen(Config __attribute__((unused)) * config) {
  return get_formatted_string("no status available for move generation");
}

// Sim

void config_fill_sim_args(const Config *config, Rack *known_opp_rack,
                          SimArgs *sim_args) {
  sim_args->max_iterations = config->max_iterations;
  sim_args->num_plies = config_get_plies(config);
  sim_args->stop_cond_pct = config_get_stop_cond_pct(config);
  sim_args->seed = config->seed;
  sim_args->game = config_get_game(config);
  sim_args->move_list = config_get_move_list(config);
  sim_args->known_opp_rack = known_opp_rack;
  sim_args->win_pcts = config_get_win_pcts(config);
  sim_args->thread_control = config->thread_control;
}

sim_status_t config_simulate(const Config *config, Rack *known_opp_rack,
                             SimResults *sim_results) {
  SimArgs args;
  config_fill_sim_args(config, known_opp_rack, &args);
  return simulate(&args, sim_results);
}

void execute_sim(Config *config) {
  if (!config_has_game_data(config)) {
    set_or_clear_error_status(config->error_status,
                              ERROR_STATUS_TYPE_CONFIG_LOAD,
                              CONFIG_LOAD_STATUS_GAME_DATA_MISSING);
    return;
  }

  config_init_game(config);

  ErrorStatus *error_status = config->error_status;

  const char *known_opp_rack_str =
      config_get_parg_value(config, ARG_TOKEN_SIM, 0);
  Rack *known_opp_rack = NULL;

  if (known_opp_rack_str) {
    known_opp_rack = rack_create(ld_get_size(game_get_ld(config->game)));
    if (!load_rack_or_set_error_status(
            known_opp_rack_str, game_get_ld(config->game), error_status,
            ERROR_STATUS_TYPE_CONFIG_LOAD,
            CONFIG_LOAD_STATUS_MALFORMED_RACK_ARG, known_opp_rack)) {
      rack_destroy(known_opp_rack);
      return;
    }
  }

  sim_status_t status =
      config_simulate(config, known_opp_rack, config->sim_results);
  set_or_clear_error_status(config->error_status, ERROR_STATUS_TYPE_SIM,
                            (int)status);
  rack_destroy(known_opp_rack);
}

char *status_sim(Config *config) {
  SimResults *sim_results = config->sim_results;
  if (!sim_results) {
    log_warn("Simmer has not been initialized.");
    return NULL;
  }
  return ucgi_sim_stats(config->game, sim_results, config->thread_control,
                        true);
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

inference_status_t config_infer(const Config *config, int target_index,
                                int target_score, int target_num_exch,
                                Rack *target_played_tiles,
                                InferenceResults *results) {
  InferenceArgs args;
  config_fill_infer_args(config, target_index, target_score, target_num_exch,
                         target_played_tiles, &args);
  return infer(&args, results);
}

void execute_infer_with_rack(Config *config, Rack *target_played_tiles) {
  ErrorStatus *error_status = config->error_status;

  const char *target_index_str =
      config_get_parg_value(config, ARG_TOKEN_INFER, 0);
  int target_index;
  if (!string_to_int_or_set_error_status(
          target_index_str, 1, 2, error_status, ERROR_STATUS_TYPE_CONFIG_LOAD,
          CONFIG_LOAD_STATUS_MALFORMED_INT_ARG, &target_index)) {
    return;
  }
  // Convert from 1-indexed to 0-indexed
  target_index--;

  const char *target_played_tiles_or_num_exch_str =
      config_get_parg_value(config, ARG_TOKEN_INFER, 1);

  int target_num_exch = 0;
  bool is_tile_placement_move = false;

  if (is_all_digits_or_empty(target_played_tiles_or_num_exch_str)) {
    if (!string_to_int_or_set_error_status(
            target_played_tiles_or_num_exch_str, 0, RACK_SIZE, error_status,
            ERROR_STATUS_TYPE_CONFIG_LOAD, CONFIG_LOAD_STATUS_MALFORMED_INT_ARG,
            &target_num_exch)) {
      return;
    }
  } else {
    const LetterDistribution *ld = game_get_ld(config->game);
    if (!load_rack_or_set_error_status(
            target_played_tiles_or_num_exch_str, ld, error_status,
            ERROR_STATUS_TYPE_CONFIG_LOAD,
            CONFIG_LOAD_STATUS_MALFORMED_RACK_ARG, target_played_tiles)) {
      return;
    }
    is_tile_placement_move = true;
  }

  int target_score = 0;

  if (is_tile_placement_move) {
    const char *target_score_str =
        config_get_parg_value(config, ARG_TOKEN_INFER, 2);

    if (!target_score_str) {
      set_or_clear_error_status(config->error_status,
                                ERROR_STATUS_TYPE_CONFIG_LOAD,
                                CONFIG_LOAD_STATUS_MISSING_ARG);
      return;
    }

    if (!string_to_int_or_set_error_status(
            target_score_str, 0, INT_MAX, error_status,
            ERROR_STATUS_TYPE_CONFIG_LOAD, CONFIG_LOAD_STATUS_MALFORMED_INT_ARG,
            &target_score)) {
      return;
    }
  }

  inference_status_t status =
      config_infer(config, target_index, target_score, target_num_exch,
                   target_played_tiles, config->inference_results);

  set_or_clear_error_status(config->error_status, ERROR_STATUS_TYPE_INFER,
                            (int)status);
}

void execute_infer(Config *config) {
  if (!config_has_game_data(config)) {
    set_or_clear_error_status(config->error_status,
                              ERROR_STATUS_TYPE_CONFIG_LOAD,
                              CONFIG_LOAD_STATUS_GAME_DATA_MISSING);
    return;
  }

  config_init_game(config);
  Rack *target_played_tiles =
      rack_create(ld_get_size(game_get_ld(config->game)));
  execute_infer_with_rack(config, target_played_tiles);
  rack_destroy(target_played_tiles);
}

char *status_infer(Config __attribute__((unused)) * config) {
  return string_duplicate("no status available for inference");
}

// Autoplay

void config_fill_autoplay_args(const Config *config,
                               AutoplayArgs *autoplay_args) {
  autoplay_args->max_iterations = config->max_iterations;
  autoplay_args->use_game_pairs = config->use_game_pairs;
  autoplay_args->seed = config->seed;
  autoplay_args->thread_control = config->thread_control;
  config_fill_game_args(config, autoplay_args->game_args);
}

autoplay_status_t config_autoplay(const Config *config,
                                  AutoplayResults *autoplay_results) {
  AutoplayArgs args;
  GameArgs game_args;
  args.game_args = &game_args;
  config_fill_autoplay_args(config, &args);
  return autoplay(&args, autoplay_results);
}

void execute_autoplay(Config *config) {
  if (!config_has_game_data(config)) {
    set_or_clear_error_status(config->error_status,
                              ERROR_STATUS_TYPE_CONFIG_LOAD,
                              CONFIG_LOAD_STATUS_GAME_DATA_MISSING);
    return;
  }
  autoplay_status_t status = config_autoplay(config, config->autoplay_results);
  set_or_clear_error_status(config->error_status, ERROR_STATUS_TYPE_AUTOPLAY,
                            (int)status);
}

char *status_autoplay(Config __attribute__((unused)) * config) {
  return string_duplicate("no status available for autoplay");
}

// Conversion

void config_fill_conversion_args(const Config *config, ConversionArgs *args) {
  args->conversion_type_string =
      config_get_parg_value(config, ARG_TOKEN_CONVERT, 0);
  args->input_filename = config_get_parg_value(config, ARG_TOKEN_CONVERT, 1);
  args->output_filename = config_get_parg_value(config, ARG_TOKEN_CONVERT, 2);
  args->ld = config->ld;
}

conversion_status_t config_convert(const Config *config,
                                   ConversionResults *results) {
  ConversionArgs args;
  config_fill_conversion_args(config, &args);
  return convert(&args, results);
}

void execute_convert(Config *config) {
  conversion_status_t status =
      config_convert(config, config->conversion_results);
  set_or_clear_error_status(config->error_status, ERROR_STATUS_TYPE_CONVERT,
                            (int)status);
}

char *status_convert(Config __attribute__((unused)) * config) {
  return string_duplicate("no status available for convert");
}

// Config load helpers

config_load_status_t config_load_parsed_args(Config *config,
                                             StringSplitter *cmd_split_string) {
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
          return CONFIG_LOAD_STATUS_INSUFFICIENT_NUMBER_OF_VALUES;
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
            return CONFIG_LOAD_STATUS_AMBIGUOUS_COMMAND;
          }
          current_parg = config->pargs[k];
          current_arg_token = k;
        }
      }

      if (!current_parg) {
        return CONFIG_LOAD_STATUS_UNRECOGNIZED_ARG;
      }

      if (current_parg->num_set_values > 0) {
        return CONFIG_LOAD_STATUS_DUPLICATE_ARG;
      }

      if (current_parg->exec_func != execute_fatal) {
        if (i > 0) {
          return CONFIG_LOAD_STATUS_MISPLACED_COMMAND;
        } else {
          config->exec_parg_token = current_arg_token;
        }
      }
      current_value_index = 0;
    } else {
      if (!current_parg || current_value_index >= current_parg->num_values) {
        return CONFIG_LOAD_STATUS_UNRECOGNIZED_ARG;
      }
      free(current_parg->values[current_value_index]);
      current_parg->values[current_value_index] = string_duplicate(input_str);
      current_value_index++;
      current_parg->num_set_values = current_value_index;
    }
  }

  if (current_parg) {
    if (current_value_index < current_parg->num_req_values) {
      return CONFIG_LOAD_STATUS_INSUFFICIENT_NUMBER_OF_VALUES;
    }
    current_parg = NULL;
  }

  return CONFIG_LOAD_STATUS_SUCCESS;
}

config_load_status_t config_load_sort_type(Config *config,
                                           const char *sort_type_str,
                                           int player_index) {
  config_load_status_t config_load_status = CONFIG_LOAD_STATUS_SUCCESS;
  if (has_iprefix(sort_type_str, "equity")) {
    players_data_set_move_sort_type(config->players_data, player_index,
                                    MOVE_SORT_EQUITY);
  } else if (has_iprefix(sort_type_str, "score")) {
    players_data_set_move_sort_type(config->players_data, player_index,
                                    MOVE_SORT_SCORE);
  } else {
    config_load_status = CONFIG_LOAD_STATUS_MALFORMED_MOVE_SORT_TYPE;
  }
  return config_load_status;
}

config_load_status_t config_load_record_type(Config *config,
                                             const char *record_type_str,
                                             int player_index) {
  config_load_status_t config_load_status = CONFIG_LOAD_STATUS_SUCCESS;
  if (has_iprefix(record_type_str, "best")) {
    players_data_set_move_record_type(config->players_data, player_index,
                                      MOVE_RECORD_BEST);
  } else if (has_iprefix(record_type_str, "all")) {
    players_data_set_move_record_type(config->players_data, player_index,
                                      MOVE_RECORD_ALL);
  } else {
    config_load_status = CONFIG_LOAD_STATUS_MALFORMED_MOVE_RECORD_TYPE;
  }
  return config_load_status;
}

bool is_lexicon_required(const char *new_p1_leaves_name,
                         const char *new_p2_leaves_name,
                         const char *new_ld_name) {
  return new_p1_leaves_name || new_p2_leaves_name || new_ld_name;
}

bool lds_are_compatible(const char *ld_name_1, const char *ld_name_2) {
  return strings_equal(ld_name_1, ld_name_2) ||
         // English and French use the same letters so they are
         // allowed to play against each other.
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

config_load_status_t config_load_lexicon_dependent_data(Config *config) {
  // Lexical player data

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

  const char *new_p1_leaves_name =
      config_get_parg_value(config, ARG_TOKEN_P1_LEAVES, 0);
  const char *new_p2_leaves_name =
      config_get_parg_value(config, ARG_TOKEN_P2_LEAVES, 0);
  const char *new_ld_name =
      config_get_parg_value(config, ARG_TOKEN_LETTER_DISTRIBUTION, 0);

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
    // are null, it is guaranteed that there are no leaves, or ld
    // since they are all set after this if check.
    if (is_lexicon_required(new_p1_leaves_name, new_p2_leaves_name,
                            new_ld_name)) {
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

  char *updated_ld_name = NULL;
  const char *existing_ld_name = NULL;
  if (config->ld) {
    existing_ld_name = ld_get_name(config->ld);
  }
  if (!is_string_empty_or_null(new_ld_name)) {
    if (!ld_is_compatible_with_lexicon(updated_p1_lexicon_name, new_ld_name)) {
      return CONFIG_LOAD_STATUS_INCOMPATIBLE_LETTER_DISTRIBUTION;
    }
    updated_ld_name = string_duplicate(new_ld_name);
  } else if (new_p1_lexicon_name || new_p2_lexicon_name || !existing_ld_name) {
    updated_ld_name = ld_get_default_name(updated_p1_lexicon_name);
  }

  // If the letter distribution name has changed, update it
  config->ld_changed = false;
  if (updated_ld_name && !strings_equal(updated_ld_name, existing_ld_name)) {
    ld_destroy(config->ld);
    config->ld = ld_create(updated_ld_name);
    config->ld_changed = true;
  }

  free(updated_ld_name);

  return CONFIG_LOAD_STATUS_SUCCESS;
}

// Assumes all args are parsed and correctly set in pargs.
config_load_status_t config_load_data(Config *config) {
  config_load_status_t config_load_status = CONFIG_LOAD_STATUS_SUCCESS;

  // Exec Mode

  const char *new_exec_mode_str =
      config_get_parg_value(config, ARG_TOKEN_EXEC_MODE, 0);
  if (new_exec_mode_str) {
    exec_mode_t new_exec_mode = get_exec_mode_type_from_name(new_exec_mode_str);
    if (new_exec_mode == EXEC_MODE_UNKNOWN) {
      return CONFIG_LOAD_STATUS_UNRECOGNIZED_EXEC_MODE;
    }
    config->exec_mode = new_exec_mode;
  }

  // Int values

  config_load_status = config_load_int(config, ARG_TOKEN_BINGO_BONUS, INT_MIN,
                                       INT_MAX, &config->bingo_bonus);
  if (config_load_status != CONFIG_LOAD_STATUS_SUCCESS) {
    return config_load_status;
  }

  config_load_status =
      config_load_int(config, ARG_TOKEN_PLIES, 0, INT_MAX, &config->plies);
  if (config_load_status != CONFIG_LOAD_STATUS_SUCCESS) {
    return config_load_status;
  }

  config_load_status = config_load_int(config, ARG_TOKEN_NUMBER_OF_PLAYS, 0,
                                       INT_MAX, &config->num_plays);
  if (config_load_status != CONFIG_LOAD_STATUS_SUCCESS) {
    return config_load_status;
  }

  config_load_status = config_load_int(config, ARG_TOKEN_MAX_ITERATIONS, 1,
                                       INT_MAX, &config->max_iterations);
  if (config_load_status != CONFIG_LOAD_STATUS_SUCCESS) {
    return config_load_status;
  }

  int number_of_threads = -1;
  config_load_status = config_load_int(config, ARG_TOKEN_NUMBER_OF_THREADS, 1,
                                       MAX_THREADS, &number_of_threads);
  if (config_load_status != CONFIG_LOAD_STATUS_SUCCESS) {
    return config_load_status;
  }
  if (number_of_threads > 0) {
    thread_control_set_threads(config->thread_control, number_of_threads);
  }

  int print_info_interval = -1;
  config_load_status = config_load_int(config, ARG_TOKEN_PRINT_INFO_INTERVAL, 0,
                                       INT_MAX, &print_info_interval);
  if (config_load_status != CONFIG_LOAD_STATUS_SUCCESS) {
    return config_load_status;
  }
  if (print_info_interval > 0) {
    thread_control_set_print_info_interval(config->thread_control,
                                           print_info_interval);
  }

  int check_stop_interval = -1;
  config_load_status = config_load_int(config, ARG_TOKEN_CHECK_STOP_INTERVAL, 0,
                                       INT_MAX, &check_stop_interval);
  if (config_load_status != CONFIG_LOAD_STATUS_SUCCESS) {
    return config_load_status;
  }
  if (check_stop_interval > 0) {
    thread_control_set_check_stop_interval(config->thread_control,
                                           check_stop_interval);
  }

  // Double values

  const char *new_stop_cond_str =
      config_get_parg_value(config, ARG_TOKEN_STOP_COND_PCT, 0);
  if (new_stop_cond_str && has_iprefix(new_stop_cond_str, "none")) {
    config->stop_cond_pct = STOP_COND_NONE;
  } else {
    config_load_status = config_load_double(config, ARG_TOKEN_STOP_COND_PCT,
                                            0.0, 100.0, &config->stop_cond_pct);
    if (config_load_status != CONFIG_LOAD_STATUS_SUCCESS) {
      return config_load_status;
    }
  }

  config_load_status = config_load_double(config, ARG_TOKEN_EQUITY_MARGIN, 0,
                                          DBL_MAX, &config->equity_margin);
  if (config_load_status != CONFIG_LOAD_STATUS_SUCCESS) {
    return config_load_status;
  }

  // Game variant

  const char *new_game_variant_str =
      config_get_parg_value(config, ARG_TOKEN_GAME_VARIANT, 0);
  if (new_game_variant_str) {
    game_variant_t new_game_variant =
        get_game_variant_type_from_name(new_game_variant_str);
    if (new_game_variant == GAME_VARIANT_UNKNOWN) {
      return CONFIG_LOAD_STATUS_UNRECOGNIZED_GAME_VARIANT;
    }
    config->game_variant = new_game_variant;
  }

  // Game pairs

  config_load_status = config_load_bool(config, ARG_TOKEN_USE_GAME_PAIRS,
                                        &config->use_game_pairs);
  if (config_load_status != CONFIG_LOAD_STATUS_SUCCESS) {
    return config_load_status;
  }

  // Seed

  config_load_status =
      config_load_uint64(config, ARG_TOKEN_RANDOM_SEED, &config->seed);
  if (config_load_status != CONFIG_LOAD_STATUS_SUCCESS) {
    return config_load_status;
  }

  // Board layout

  const char *new_board_layout_name =
      config_get_parg_value(config, ARG_TOKEN_BOARD_LAYOUT, 0);
  if (new_board_layout_name &&
      !strings_equal(board_layout_get_name(config->board_layout),
                     new_board_layout_name)) {
    if (board_layout_load(config->board_layout, new_board_layout_name) !=
        BOARD_LAYOUT_LOAD_STATUS_SUCCESS) {
      return CONFIG_LOAD_STATUS_BOARD_LAYOUT_ERROR;
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
      config_load_status =
          config_load_sort_type(config, new_player_sort_type_str, player_index);
      if (config_load_status != CONFIG_LOAD_STATUS_SUCCESS) {
        return config_load_status;
      }
    }

    const char *new_player_record_type_str =
        config_get_parg_value(config, record_type_args[player_index], 0);
    if (new_player_record_type_str) {
      config_load_status = config_load_record_type(
          config, new_player_record_type_str, player_index);
      if (config_load_status != CONFIG_LOAD_STATUS_SUCCESS) {
        return config_load_status;
      }
    }

    const char *new_player_name =
        config_get_parg_value(config, pname_args[player_index], 0);
    if (new_player_name) {
      players_data_set_name(config->players_data, player_index,
                            new_player_name);
    }
  }

  config_load_status = config_load_lexicon_dependent_data(config);
  if (config_load_status != CONFIG_LOAD_STATUS_SUCCESS) {
    return config_load_status;
  }

  // Set win pct

  const char *new_win_pct_name =
      config_get_parg_value(config, ARG_TOKEN_WIN_PCT, 0);
  if (new_win_pct_name &&
      !strings_equal(win_pct_get_name(config->win_pcts), new_win_pct_name)) {
    win_pct_destroy(config->win_pcts);
    config->win_pcts = win_pct_create(new_win_pct_name);
  }

  // Set IO

  thread_control_set_io(config->thread_control,
                        config_get_parg_value(config, ARG_TOKEN_INFILE, 0),
                        config_get_parg_value(config, ARG_TOKEN_OUTFILE, 0));

  return config_load_status;
}

// Parses the arguments given by the cmd string and updates the state of
// the config data values, but does not execute the command.
config_load_status_t config_load_command(Config *config, const char *cmd) {
  // If the command is empty, consider this a set options
  // command where zero options are set and return without error.
  if (is_string_empty_or_whitespace(cmd)) {
    return CONFIG_LOAD_STATUS_SUCCESS;
  }

  StringSplitter *cmd_split_string = split_string_by_whitespace(cmd, true);
  // CGP data can have semicolons at the end, so
  // we trim these off to make loading easier.
  string_splitter_trim_char(cmd_split_string, ';');
  config_load_status_t config_load_status =
      config_load_parsed_args(config, cmd_split_string);

  if (config_load_status == CONFIG_LOAD_STATUS_SUCCESS) {
    config_load_status = config_load_data(config);
  }

  destroy_string_splitter(cmd_split_string);

  return config_load_status;
}

void config_execute_command(Config *config) {
  if (config_exec_parg_is_set(config)) {
    set_or_clear_error_status(config->error_status, ERROR_STATUS_TYPE_NONE, 0);
    config_get_parg_exec_func(config, config->exec_parg_token)(config);
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

Config *config_create_default() {
  Config *config = malloc_or_die(sizeof(Config));
  parsed_arg_create(config, ARG_TOKEN_SET, "setoptions", 0, 0, execute_noop,
                    status_cgp_load);
  parsed_arg_create(config, ARG_TOKEN_CGP, "cgp", 4, 4, execute_cgp_load,
                    status_cgp_load);
  parsed_arg_create(config, ARG_TOKEN_MOVES, "addmoves", 1, 1,
                    execute_add_moves, status_add_moves);
  parsed_arg_create(config, ARG_TOKEN_RACK, "rack", 2, 2, execute_set_rack,
                    status_set_rack);
  parsed_arg_create(config, ARG_TOKEN_GEN, "generate", 0, 0, execute_move_gen,
                    status_move_gen);
  parsed_arg_create(config, ARG_TOKEN_SIM, "simulate", 0, 1, execute_sim,
                    status_sim);
  parsed_arg_create(config, ARG_TOKEN_INFER, "infer", 2, 3, execute_infer,
                    status_infer);
  parsed_arg_create(config, ARG_TOKEN_AUTOPLAY, "autoplay", 0, 0,
                    execute_autoplay, status_autoplay);
  parsed_arg_create(config, ARG_TOKEN_CONVERT, "convert", 3, 3, execute_convert,
                    status_convert);
  parsed_arg_create(config, ARG_TOKEN_BINGO_BONUS, "bb", 1, 1, execute_fatal,
                    status_fatal);
  parsed_arg_create(config, ARG_TOKEN_BOARD_LAYOUT, "bdn", 1, 1, execute_fatal,
                    status_fatal);
  parsed_arg_create(config, ARG_TOKEN_GAME_VARIANT, "var", 1, 1, execute_fatal,
                    status_fatal);
  parsed_arg_create(config, ARG_TOKEN_LETTER_DISTRIBUTION, "ld", 1, 1,
                    execute_fatal, status_fatal);
  parsed_arg_create(config, ARG_TOKEN_LEXICON, "lex", 1, 1, execute_fatal,
                    status_fatal);
  parsed_arg_create(config, ARG_TOKEN_P1_NAME, "p1", 1, 1, execute_fatal,
                    status_fatal);
  parsed_arg_create(config, ARG_TOKEN_P1_LEXICON, "l1", 1, 1, execute_fatal,
                    status_fatal);
  parsed_arg_create(config, ARG_TOKEN_P1_LEAVES, "k1", 1, 1, execute_fatal,
                    status_fatal);
  parsed_arg_create(config, ARG_TOKEN_P1_MOVE_SORT_TYPE, "s1", 1, 1,
                    execute_fatal, status_fatal);
  parsed_arg_create(config, ARG_TOKEN_P1_MOVE_RECORD_TYPE, "r1", 1, 1,
                    execute_fatal, status_fatal);
  parsed_arg_create(config, ARG_TOKEN_P2_NAME, "p2", 1, 1, execute_fatal,
                    status_fatal);
  parsed_arg_create(config, ARG_TOKEN_P2_LEXICON, "l2", 1, 1, execute_fatal,
                    status_fatal);
  parsed_arg_create(config, ARG_TOKEN_P2_LEAVES, "k2", 1, 1, execute_fatal,
                    status_fatal);
  parsed_arg_create(config, ARG_TOKEN_P2_MOVE_SORT_TYPE, "s2", 1, 1,
                    execute_fatal, status_fatal);
  parsed_arg_create(config, ARG_TOKEN_P2_MOVE_RECORD_TYPE, "r2", 1, 1,
                    execute_fatal, status_fatal);
  parsed_arg_create(config, ARG_TOKEN_WIN_PCT, "winpct", 1, 1, execute_fatal,
                    status_fatal);
  parsed_arg_create(config, ARG_TOKEN_PLIES, "plies", 1, 1, execute_fatal,
                    status_fatal);
  parsed_arg_create(config, ARG_TOKEN_NUMBER_OF_PLAYS, "numplays", 1, 1,
                    execute_fatal, status_fatal);
  parsed_arg_create(config, ARG_TOKEN_MAX_ITERATIONS, "iterations", 1, 1,
                    execute_fatal, status_fatal);
  parsed_arg_create(config, ARG_TOKEN_STOP_COND_PCT, "scondition", 1, 1,
                    execute_fatal, status_fatal);
  parsed_arg_create(config, ARG_TOKEN_EQUITY_MARGIN, "equitymargin", 1, 1,
                    execute_fatal, status_fatal);
  parsed_arg_create(config, ARG_TOKEN_USE_GAME_PAIRS, "gp", 1, 1, execute_fatal,
                    status_fatal);
  parsed_arg_create(config, ARG_TOKEN_RANDOM_SEED, "seed", 1, 1, execute_fatal,
                    status_fatal);
  parsed_arg_create(config, ARG_TOKEN_NUMBER_OF_THREADS, "threads", 1, 1,
                    execute_fatal, status_fatal);
  parsed_arg_create(config, ARG_TOKEN_PRINT_INFO_INTERVAL, "pfrequency", 1, 1,
                    execute_fatal, status_fatal);
  parsed_arg_create(config, ARG_TOKEN_CHECK_STOP_INTERVAL, "cfrequency", 1, 1,
                    execute_fatal, status_fatal);
  parsed_arg_create(config, ARG_TOKEN_INFILE, "infile", 1, 1, execute_fatal,
                    status_fatal);
  parsed_arg_create(config, ARG_TOKEN_OUTFILE, "outfile", 1, 1, execute_fatal,
                    status_fatal);
  parsed_arg_create(config, ARG_TOKEN_EXEC_MODE, "mode", 1, 1, execute_fatal,
                    status_fatal);

  config->exec_parg_token = NUMBER_OF_ARG_TOKENS;
  config->ld_changed = false;
  config->exec_mode = EXEC_MODE_CONSOLE;
  config->bingo_bonus = DEFAULT_BINGO_BONUS;
  config->num_plays = DEFAULT_MOVE_LIST_CAPACITY;
  config->plies = 2;
  config->max_iterations = 0;
  config->stop_cond_pct = 99;
  config->equity_margin = 0;
  config->use_game_pairs = true;
  config->seed = 0;
  config->game_variant = DEFAULT_GAME_VARIANT;
  config->win_pcts = win_pct_create(DEFAULT_WIN_PCT);
  config->board_layout = board_layout_create_default();
  config->ld = NULL;
  config->players_data = players_data_create();
  config->thread_control = thread_control_create();
  config->game = NULL;
  config->move_list = NULL;
  config->sim_results = sim_results_create();
  config->inference_results = inference_results_create();
  config->autoplay_results = autoplay_results_create();
  config->conversion_results = conversion_results_create();
  config->error_status = error_status_create();
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
  error_status_destroy(config->error_status);
  free(config);
}