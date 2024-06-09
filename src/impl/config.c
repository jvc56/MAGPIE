#include "config.h"

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

// FIXME: order this whole mess properly

typedef enum {
  // Commands
  ARG_TOKEN_CGP,
  ARG_TOKEN_MOVES,
  ARG_TOKEN_GEN,
  ARG_TOKEN_SIM,
  ARG_TOKEN_SIM_KNOWN,
  ARG_TOKEN_INFER,
  ARG_TOKEN_AUTOPLAY,
  ARG_TOKEN_CONVERT,
  // Stateful config values that should
  // retain their value over multiple commands
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
  ARG_TOKEN_GAME_PAIRS,
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

typedef enum {
  // Tokens are ordered by chronological load time
  // Most token orderings don't matter, however, the
  // following ordering for certain tokens must be enforced:
  //
  // CONFIG_FIELD_TOKEN_PLAYERS_DATA <
  // CONFIG_FIELD_TOKEN_LETTER_DISTRIBUTION
  //   (lexicons are needed to establish default letter distribution)
  //
  // *all other tokens* < NUMBER_OF_CONFIG_FIELD_TOKENS
  //   (must be last to correctly designate the number of tokens)
  //
  CONFIG_FIELD_TOKEN_BINGO_BONUS,
  CONFIG_FIELD_TOKEN_BOARD_LAYOUT,
  CONFIG_FIELD_TOKEN_GAME_VARIANT,
  CONFIG_FIELD_TOKEN_EQUITY_MARGIN,
  CONFIG_FIELD_TOKEN_WIN_PCTS,
  CONFIG_FIELD_TOKEN_NUM_PLAYS,
  CONFIG_FIELD_TOKEN_PLIES,
  CONFIG_FIELD_TOKEN_MAX_ITERATIONS,
  CONFIG_FIELD_TOKEN_STOP_COND_PCT,
  CONFIG_FIELD_TOKEN_USE_GAME_PAIRS,
  CONFIG_FIELD_TOKEN_SEED,
  CONFIG_FIELD_TOKEN_PLAYERS_DATA,
  CONFIG_FIELD_TOKEN_LETTER_DISTRIBUTION,
  CONFIG_FIELD_TOKEN_THREAD_CONTROL,
  CONFIG_FIELD_TOKEN_EXEC_MODE,
  // This must always be the last
  // token for the count to be accurate
  NUMBER_OF_CONFIG_FIELD_TOKENS
} config_field_token_t;

typedef void (*command_exec_func_t)(Config *);
typedef char *(*command_status_func_t)(Config *);

typedef struct ParsedArg {
  const char ***seqs;
  char **values;
  int num_values;
  bool has_value;
  command_exec_func_t exec_func;
  command_status_func_t status_func;
} ParsedArg;

typedef struct ConfigField ConfigField;

typedef void (*config_value_create_func_t)(ConfigField *);
typedef void (*config_value_destroy_func_t)(ConfigField *);
typedef config_load_status_t (*config_value_load_parg_func_t)(
    Config *, config_field_token_t, arg_token_t);

struct ConfigField {
  void *data;
  char *data_name;
  arg_token_t arg_token;
  config_value_destroy_func_t destroy_func;
  config_value_load_parg_func_t load_parg_func;
};

struct Config {
  ParsedArg *pargs[NUMBER_OF_ARG_TOKENS];
  // FIXME: clarify separation between ConfigField fields
  // and normal fields or remove the distinct entirely
  ConfigField *fields[NUMBER_OF_CONFIG_FIELD_TOKENS];
  ParsedArg *exec_parg;
  arg_token_t exec_parg_token;
  Game *game;
  MoveList *move_list;
  SimResults *sim_results;
  InferenceResults *inference_results;
  AutoplayResults *autoplay_results;
  ConversionResults *conversion_results;
  ErrorStatus *error_status;
};

const char *config_get_parg_value(const Config *config, arg_token_t arg_token,
                                  int value_index) {
  return config->pargs[arg_token]->values[value_index];
}

bool config_get_parg_has_value(const Config *config, arg_token_t arg_token) {
  return config->pargs[arg_token]->has_value;
}

int config_get_parg_num_values(const Config *config, arg_token_t arg_token) {
  return config->pargs[arg_token]->num_values;
}

void *config_field_get(const Config *config,
                       config_field_token_t config_field_token) {
  return config->fields[config_field_token]->data;
}

const char *config_field_get_name(const Config *config,
                                  config_field_token_t config_field_token) {
  return config->fields[config_field_token]->data_name;
}

// Config data getter functions

int config_get_bingo_bonus(const Config *config) {
  return *(int *)config_field_get(config, CONFIG_FIELD_TOKEN_BINGO_BONUS);
}

BoardLayout *config_get_board_layout(const Config *config) {
  return (BoardLayout *)config_field_get(config,
                                         CONFIG_FIELD_TOKEN_BOARD_LAYOUT);
}

const char *config_get_board_layout_name(const Config *config) {
  return (const char *)config_field_get_name(config,
                                             CONFIG_FIELD_TOKEN_BOARD_LAYOUT);
}

game_variant_t config_get_game_variant(const Config *config) {
  return *(game_variant_t *)config_field_get(config,
                                             CONFIG_FIELD_TOKEN_GAME_VARIANT);
}

double config_get_equity_margin(const Config *config) {
  return *(double *)config_field_get(config, CONFIG_FIELD_TOKEN_EQUITY_MARGIN);
}

WinPct *config_get_win_pcts(const Config *config) {
  return (WinPct *)config_field_get(config, CONFIG_FIELD_TOKEN_WIN_PCTS);
}

int config_get_num_plays(const Config *config) {
  return *(int *)config_field_get(config, CONFIG_FIELD_TOKEN_NUM_PLAYS);
}

int config_get_plies(const Config *config) {
  return *(int *)config_field_get(config, CONFIG_FIELD_TOKEN_PLIES);
}

int config_get_max_iterations(const Config *config) {
  return *(int *)config_field_get(config, CONFIG_FIELD_TOKEN_MAX_ITERATIONS);
}

double config_get_stop_cond_pct(const Config *config) {
  return *(double *)config_field_get(config, CONFIG_FIELD_TOKEN_STOP_COND_PCT);
}

bool config_get_use_game_pairs(const Config *config) {
  return *(bool *)config_field_get(config, CONFIG_FIELD_TOKEN_USE_GAME_PAIRS);
}

uint64_t config_get_seed(const Config *config) {
  return *(uint64_t *)config_field_get(config, CONFIG_FIELD_TOKEN_SEED);
}

PlayersData *config_get_players_data(const Config *config) {
  return (PlayersData *)config_field_get(config,
                                         CONFIG_FIELD_TOKEN_PLAYERS_DATA);
}

LetterDistribution *config_get_ld(const Config *config) {
  return (LetterDistribution *)config_field_get(
      config, CONFIG_FIELD_TOKEN_LETTER_DISTRIBUTION);
}

const char *config_get_ld_name(const Config *config) {
  return (const char *)config_field_get_name(
      config, CONFIG_FIELD_TOKEN_LETTER_DISTRIBUTION);
}

ThreadControl *config_get_thread_control(const Config *config) {
  return (ThreadControl *)config_field_get(config,
                                           CONFIG_FIELD_TOKEN_THREAD_CONTROL);
}

ErrorStatus *config_get_error_status(const Config *config) {
  return config->error_status;
}

exec_mode_t config_get_exec_mode(const Config *config) {
  return *(exec_mode_t *)config_field_get(config, CONFIG_FIELD_TOKEN_EXEC_MODE);
}

Game *config_get_game(const Config *config) { return config->game; }

MoveList *config_get_move_list(const Config *config) {
  return config->move_list;
}

bool config_continue_on_coldstart(const Config *config) {
  ParsedArg *exec_parg = config->exec_parg;
  return !exec_parg || config->exec_parg_token == ARG_TOKEN_CGP ||
         config_get_parg_has_value(config, ARG_TOKEN_INFILE) ||
         config_get_parg_has_value(config, ARG_TOKEN_EXEC_MODE);
}

// Config entity creators

// FIXME: fillers should also get the parg strings

void config_fill_game_args(const Config *config, GameArgs *game_args) {
  game_args->players_data = config_get_players_data(config);
  game_args->board_layout = config_get_board_layout(config);
  game_args->ld = config_get_ld(config);
  game_args->game_variant = config_get_game_variant(config);
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

void config_fill_autoplay_args(const Config *config,
                               AutoplayArgs *autoplay_args) {
  autoplay_args->max_iterations = config_get_max_iterations(config);
  autoplay_args->use_game_pairs = config_get_use_game_pairs(config);
  autoplay_args->seed = config_get_seed(config);
  GameArgs game_args;
  config_fill_game_args(config, &game_args);
  autoplay_args->game_args = &game_args;
  autoplay_args->thread_control = config_get_thread_control(config);
}

autoplay_status_t config_autoplay(const Config *config,
                                  AutoplayResults *autoplay_results) {
  AutoplayArgs args;
  config_fill_autoplay_args(config, &args);
  return autoplay(&args, autoplay_results);
}

void config_fill_sim_args(const Config *config, Rack *known_opp_rack,
                          SimArgs *sim_args) {
  sim_args->max_iterations = config_get_max_iterations(config);
  sim_args->num_simmed_plays = config_get_num_plays(config);
  sim_args->num_plies = config_get_plies(config);
  sim_args->stop_cond_pct = config_get_stop_cond_pct(config);
  sim_args->seed = config_get_seed(config);
  sim_args->game = config_get_game(config);
  sim_args->move_list = config_get_move_list(config);
  sim_args->known_opp_rack = known_opp_rack;
  sim_args->win_pcts = config_get_win_pcts(config);
  sim_args->thread_control = config_get_thread_control(config);
}

sim_status_t config_simulate(const Config *config, Rack *known_opp_rack,
                             SimResults *sim_results) {
  SimArgs args;
  config_fill_sim_args(config, known_opp_rack, &args);
  return simulate(&args, sim_results);
}

void config_fill_conversion_args(const Config *config, ConversionArgs *args) {
  args->conversion_type_string =
      config_get_parg_value(config, ARG_TOKEN_EXEC_MODE, 0);
  args->input_filename = config_get_parg_value(config, ARG_TOKEN_INFILE, 0);
  args->output_filename = config_get_parg_value(config, ARG_TOKEN_OUTFILE, 0);
  args->ld = config_get_ld(config);
}

conversion_status_t config_convert(const Config *config,
                                   ConversionResults *results) {
  ConversionArgs args;
  config_fill_conversion_args(config, &args);
  return convert(&args, results);
}

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
  args->thread_control = config_get_thread_control(config);
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

//

bool is_game_recreation_required(const Config *config) {
  // If the ld changes (bag and rack size)
  // a recreation is required to resize the
  // dynamically allocated fields.
  return config_get_parg_has_value(config, ARG_TOKEN_LETTER_DISTRIBUTION);
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

// Exec helpers

bool confirm_nonnull_game_or_set_error_status(const Game *game,
                                              ErrorStatus *error_status,
                                              error_status_t error_status_type,
                                              int status) {
  bool success = true;
  if (!game) {
    set_or_clear_error_status(error_status, error_status_type, status);
    success = false;
  }
  return success;
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

// Loading CGP

void execute_cgp_load(Config *config) {
  config_init_game(config);

  StringBuilder *cgp_builder = create_string_builder();
  int num_values = config_get_parg_num_values(config, ARG_TOKEN_CGP);
  for (int i = 0; i < num_values; i++) {
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
  ErrorStatus *error_status = config->error_status;

  if (!confirm_nonnull_game_or_set_error_status(
          config->game, error_status, ERROR_STATUS_TYPE_MOVE_VALIDATION,
          MOVE_VALIDATION_STATUS_GAME_NOT_LOADED)) {
    return;
  }

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

  if (move_validation_status != MOVE_VALIDATION_STATUS_SUCCESS) {
    set_or_clear_error_status(config->error_status,
                              ERROR_STATUS_TYPE_MOVE_VALIDATION,
                              (int)move_validation_status);
  }
}

char *status_add_moves(Config __attribute__((unused)) * config) {
  return string_duplicate("no status available for adding moves");
}

// Move generation

void execute_move_gen(Config *config) {
  ErrorStatus *error_status = config->error_status;

  if (!confirm_nonnull_game_or_set_error_status(
          config->game, error_status, ERROR_STATUS_TYPE_MOVE_GEN,
          MOVE_GEN_STATUS_NO_GAME_LOADED)) {
    return;
  }

  config_recreate_move_list(config, config_get_num_plays(config));
  MoveList *ml = config->move_list;
  generate_moves_for_game(config->game, 0, ml);
  print_ucgi_static_moves(config->game, ml, config_get_thread_control(config));
  set_or_clear_error_status(config->error_status, ERROR_STATUS_TYPE_SIM,
                            (int)GEN_STATUS_SUCCESS);
}

char *status_move_gen(Config __attribute__((unused)) * config) {
  return get_formatted_string("no status available for move generation");
}

// Sim - no opponent known rack

void execute_sim(Config *config) {
  ErrorStatus *error_status = config->error_status;

  if (!confirm_nonnull_game_or_set_error_status(config->game, error_status,
                                                ERROR_STATUS_TYPE_SIM,
                                                SIM_STATUS_GAME_NOT_LOADED)) {
    return;
  }

  sim_status_t status = config_simulate(config, NULL, config->sim_results);
  set_or_clear_error_status(config->error_status, ERROR_STATUS_TYPE_SIM,
                            (int)status);
}

char *status_sim(Config *config) {
  SimResults *sim_results = config->sim_results;
  if (!sim_results) {
    log_warn("Simmer has not been initialized.");
    return NULL;
  }
  return ucgi_sim_stats(config->game, sim_results,
                        config_get_thread_control(config), true);
}

// Sim - with opponent known rack

void execute_sim_with_known_opp_rack(Config *config) {
  ErrorStatus *error_status = config->error_status;

  if (!confirm_nonnull_game_or_set_error_status(config->game, error_status,
                                                ERROR_STATUS_TYPE_SIM,
                                                SIM_STATUS_GAME_NOT_LOADED)) {
    return;
  }

  const char *known_opp_rack_str =
      config_get_parg_value(config, ARG_TOKEN_SIM, 1);
  Rack *known_opp_rack = rack_create(ld_get_size(game_get_ld(config->game)));
  if (!load_rack_or_set_error_status(
          known_opp_rack_str, game_get_ld(config->game), error_status,
          ERROR_STATUS_TYPE_SIM, SIM_STATUS_EXCHANGE_MALFORMED_RACK,
          known_opp_rack)) {
    rack_destroy(known_opp_rack);
    return;
  }

  sim_status_t status =
      config_simulate(config, known_opp_rack, config->sim_results);
  set_or_clear_error_status(config->error_status, ERROR_STATUS_TYPE_SIM,
                            (int)status);
  rack_destroy(known_opp_rack);
}

// Inference

void execute_infer_with_rack(Config *config, Rack *target_played_tiles) {
  ErrorStatus *error_status = config->error_status;

  const char *target_index_str =
      config_get_parg_value(config, ARG_TOKEN_INFER, 0);
  int target_index;
  if (!string_to_int_or_set_error_status(
          target_index_str, 0, 1, error_status, ERROR_STATUS_TYPE_INFER,
          INFERENCE_STATUS_EXCHANGE_MALFORMED_PLAYER_INDEX, &target_index)) {
    return;
  }

  const char *target_played_tiles_str =
      config_get_parg_value(config, ARG_TOKEN_INFER, 1);
  if (!load_rack_or_set_error_status(
          target_played_tiles_str, game_get_ld(config->game), error_status,
          ERROR_STATUS_TYPE_INFER, INFERENCE_STATUS_EXCHANGE_MALFORMED_RACK,
          target_played_tiles)) {
    rack_destroy(target_played_tiles);
    return;
  }

  const char *target_score_str =
      config_get_parg_value(config, ARG_TOKEN_INFER, 2);
  int target_score;
  if (!string_to_int_or_set_error_status(
          target_score_str, 0, INT_MAX, error_status, ERROR_STATUS_TYPE_INFER,
          INFERENCE_STATUS_EXCHANGE_MALFORMED_PLAYER_INDEX, &target_score)) {
    return;
  }

  const char *target_num_exch_str =
      config_get_parg_value(config, ARG_TOKEN_INFER, 3);
  int target_num_exch;
  if (!string_to_int_or_set_error_status(
          target_num_exch_str, 0, RACK_SIZE, error_status,
          ERROR_STATUS_TYPE_INFER,
          INFERENCE_STATUS_EXCHANGE_MALFORMED_PLAYER_INDEX, &target_num_exch)) {
    return;
  }

  inference_status_t status =
      config_infer(config, target_index, target_score, target_num_exch,
                   target_played_tiles, config->inference_results);

  set_or_clear_error_status(config->error_status, ERROR_STATUS_TYPE_INFER,
                            (int)status);
}

void execute_infer(Config *config) {
  ErrorStatus *error_status = config->error_status;

  if (!confirm_nonnull_game_or_set_error_status(
          config->game, error_status, ERROR_STATUS_TYPE_INFER,
          INFERENCE_STATUS_NO_GAME_LOADED)) {
    return;
  }

  Rack *target_played_tiles =
      rack_create(ld_get_size(game_get_ld(config->game)));
  execute_infer_with_rack(config, target_played_tiles);
  rack_destroy(target_played_tiles);
}

char *status_infer(Config __attribute__((unused)) * config) {
  return string_duplicate("no status available for inference");
}
// Autoplay

void execute_autoplay(Config *config) {
  autoplay_status_t status = config_autoplay(config, config->autoplay_results);
  set_or_clear_error_status(config->error_status, ERROR_STATUS_TYPE_AUTOPLAY,
                            (int)status);
}

char *status_autoplay(Config __attribute__((unused)) * config) {
  return string_duplicate("no status available for autoplay");
}

// Convert

void execute_convert(Config *config) {
  conversion_status_t status =
      config_convert(config, config->conversion_results);
  set_or_clear_error_status(config->error_status, ERROR_STATUS_TYPE_CONVERT,
                            (int)status);
}

char *status_convert(Config __attribute__((unused)) * config) {
  return string_duplicate("no status available for convert");
}

// Fatal execution

void execute_fatal(Config *config) {
  log_fatal("attempted to execute nonexecutable argument (arg token %d)",
            config->exec_parg_token);
}

char *status_fatal(Config *config) {
  log_fatal("attempted to get status of nonexecutable argument (arg token %d)",
            config->exec_parg_token);
  return NULL;
}

config_load_status_t config_load_parsed_args(Config *config,
                                             StringSplitter *cmd_split_string) {
  int number_of_input_args =
      string_splitter_get_number_of_items(cmd_split_string);
  int current_pos_arg_index = 0;
  config->exec_parg = NULL;
  config->exec_parg_token = NUMBER_OF_ARG_TOKENS;
  int number_of_parsed_args = 0;
  for (int i = 0; i < number_of_input_args;) {
    const char *input_arg = string_splitter_get_item(cmd_split_string, i);
    bool is_recognized_arg = false;
    for (int j = 0; j < NUMBER_OF_ARG_TOKENS; j++) {
      ParsedArg *parg = config->pargs[i];
      int current_invocation_sequence_index = 0;
      while (true) {
        const char **invocation_sequence =
            parg->seqs[current_invocation_sequence_index++];
        if (!invocation_sequence) {
          break;
        }
        if (strings_equal(input_arg,
                          invocation_sequence[current_pos_arg_index])) {
          if (!invocation_sequence[current_pos_arg_index + 1]) {
            // The full invocation sequence has been matched.
            if (parg->values[0]) {
              return CONFIG_LOAD_STATUS_DUPLICATE_ARG;
            }
            if (parg->exec_func != execute_fatal) {
              if (number_of_parsed_args > 0) {
                return CONFIG_LOAD_STATUS_MISPLACED_COMMAND;
              } else {
                config->exec_parg = parg;
                config->exec_parg_token = (arg_token_t)j;
              }
            }
            current_pos_arg_index = 0;
            int number_of_vals = config_get_parg_num_values(config, j);
            if (i + number_of_vals < number_of_input_args) {
              // Since the input arg name has been recognized, parse the
              // next single_arg->number_of_values to load the data
              // for this arg.
              for (int k = 0; k < number_of_vals; k++) {
                parg->values[k] = get_formatted_string(
                    "%s",
                    string_splitter_get_item(cmd_split_string, i + k + 1));
              }
              // Advance the current arg past all of the
              // data that were just loaded.
              i += number_of_vals + 1;
            } else {
              return CONFIG_LOAD_STATUS_INSUFFICIENT_NUMBER_OF_VALUES;
            }
            parg->has_value = true;
            number_of_parsed_args++;
            is_recognized_arg = true;
            break;
          } else {
            // The invocation sequence has been partially matched.
            current_pos_arg_index++;
          }
        }
      }
      if (is_recognized_arg) {
        break;
      }
    }
    if (!is_recognized_arg) {
      return CONFIG_LOAD_STATUS_UNRECOGNIZED_ARG;
    }
  }

  return CONFIG_LOAD_STATUS_SUCCESS;
}

// Players data

bool is_lexicon_required(const Config *config, const char *new_p1_leaves_name,
                         const char *new_p2_leaves_name,
                         const char *new_ld_name) {
  return config->exec_parg || new_p1_leaves_name || new_p2_leaves_name ||
         new_ld_name;
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

config_load_status_t
config_field_set_players_data(Config *config,
                              config_field_token_t config_field_token,
                              arg_token_t __attribute__((unused)) arg_token) {
  PlayersData *players_data = config_field_get(config, config_field_token);

  const char *new_p1_lexicon_name =
      config_get_parg_value(config, ARG_TOKEN_P1_LEXICON, 0);
  const char *new_p2_lexicon_name =
      config_get_parg_value(config, ARG_TOKEN_P2_LEXICON, 0);
  const char *new_p1_leaves_name =
      config_get_parg_value(config, ARG_TOKEN_P1_LEAVES, 0);
  const char *new_p2_leaves_name =
      config_get_parg_value(config, ARG_TOKEN_P2_LEAVES, 0);
  const char *new_ld_name =
      config_get_parg_value(config, ARG_TOKEN_LETTER_DISTRIBUTION, 0);

  // Load lexicons first
  const char *existing_p1_lexicon_name =
      players_data_get_data_name(players_data, PLAYERS_DATA_TYPE_KWG, 0);
  const char *existing_p2_lexicon_name =
      players_data_get_data_name(players_data, PLAYERS_DATA_TYPE_KWG, 1);

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

  players_data_set(players_data, PLAYERS_DATA_TYPE_KWG, updated_p1_lexicon_name,
                   updated_p2_lexicon_name);

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

  players_data_set(players_data, PLAYERS_DATA_TYPE_KLV, updated_p1_leaves_name,
                   updated_p2_leaves_name);

  free(updated_p1_leaves_name);
  free(updated_p2_leaves_name);

  return CONFIG_LOAD_STATUS_SUCCESS;
}

// FIXME: make create/destroy ordering consistent across names
void config_field_players_data_create(ConfigField *config_field) {
  config_field->data = players_data_create();
}

void config_field_players_data_destroy(ConfigField *config_field) {
  players_data_destroy(config_field->data);
}

// Letter distribution

config_load_status_t
config_field_set_ld(Config *config,
                    config_field_token_t
                    __attribute__((unused)) unused_config_field_token,
                    arg_token_t __attribute__((unused)) unused_arg_token) {
  PlayersData *players_data =
      config_field_get(config, CONFIG_FIELD_TOKEN_PLAYERS_DATA);
  const char *new_ld_name =
      config_get_parg_value(config, ARG_TOKEN_LETTER_DISTRIBUTION, 0);
  const char *existing_ld_name =
      config_field_get(config, CONFIG_FIELD_TOKEN_LETTER_DISTRIBUTION);

  // This is the updated p1 lexicon name since the players data
  // is always set before this function is called.
  const char *updated_p1_lexicon_name =
      players_data_get_data_name(players_data, PLAYERS_DATA_TYPE_KWG, 0);

  char *updated_ld_name;
  if (!is_string_empty_or_null(new_ld_name)) {
    // if the user specified letter distribution
    // isn't compatible, return an error
    if (!ld_is_compatible_with_lexicon(updated_p1_lexicon_name, new_ld_name)) {
      return CONFIG_LOAD_STATUS_INCOMPATIBLE_LETTER_DISTRIBUTION;
    }
    updated_ld_name = string_duplicate(new_ld_name);
  } else if (is_string_empty_or_null(existing_ld_name)) {
    // No letter distribution was specified and the current
    // letter distribution is null, so load the default letter
    // distribution based on player one's lexicon
    updated_ld_name = ld_get_default_name(updated_p1_lexicon_name);
  } else {
    // If the existing letter distribution isn't compatible,
    // just assume we want to use the default.
    if (!ld_is_compatible_with_lexicon(updated_p1_lexicon_name,
                                       existing_ld_name)) {
      updated_ld_name = ld_get_default_name(updated_p1_lexicon_name);
    } else {
      updated_ld_name = string_duplicate(existing_ld_name);
    }
  }

  // Here we can assume that the lexicons are already
  // compatible with each other, so we only need to
  // check the letter distribution with 1 lexicon

  // If the letter distribution name has changed, update it
  if (!strings_equal(existing_ld_name, updated_ld_name)) {
    LetterDistribution *updated_ld = ld_create(updated_ld_name);
    ld_destroy(config->fields[CONFIG_FIELD_TOKEN_LETTER_DISTRIBUTION]->data);
    config->fields[CONFIG_FIELD_TOKEN_LETTER_DISTRIBUTION]->data = updated_ld;
    free(config->fields[CONFIG_FIELD_TOKEN_LETTER_DISTRIBUTION]->data_name);
    config->fields[CONFIG_FIELD_TOKEN_LETTER_DISTRIBUTION]->data_name =
        string_duplicate(updated_ld_name);
  } else {
    free(updated_ld_name);
  }
  return CONFIG_LOAD_STATUS_SUCCESS;
}

void config_field_ld_destroy(ConfigField *config_field) {
  free(config_field->data_name);
  ld_destroy(config_field->data);
}

// Strings

config_load_status_t
config_field_set_string(Config *config, config_field_token_t config_field_token,
                        arg_token_t arg_token) {
  const char *arg_string_value = config_get_parg_value(config, arg_token, 0);
  if (arg_string_value) {
    free(config->fields[config_field_token]->data);
    config->fields[config_field_token]->data =
        string_duplicate(arg_string_value);
  }
  return CONFIG_LOAD_STATUS_SUCCESS;
}

void config_field_destroy_string(ConfigField *config_field) {
  char **string = config_field->data;
  free(*string);
  free(string);
}

// uint64

void config_field_create_uint64(ConfigField *config_field) {
  config_field->data = malloc_or_die(sizeof(uint64_t));
}

config_load_status_t
config_field_set_uint64(Config *config, config_field_token_t config_field_token,
                        arg_token_t arg_token) {
  const char *new_value_string = config_get_parg_value(config, arg_token, 0);
  if (new_value_string) {
    if (!is_all_digits_or_empty(new_value_string)) {
      return CONFIG_LOAD_STATUS_MALFORMED_INT_ARG;
    }
    *(uint64_t *)config->fields[config_field_token]->data =
        string_to_uint64(new_value_string);
  } else if (config_field_token == CONFIG_FIELD_TOKEN_SEED) {
    *(uint64_t *)config->fields[config_field_token]->data = time(NULL);
  }
  return CONFIG_LOAD_STATUS_SUCCESS;
}

// double

void config_field_create_double(ConfigField *config_field) {
  config_field->data = malloc_or_die(sizeof(double));
}

config_load_status_t
config_field_set_double(Config *config, config_field_token_t config_field_token,
                        arg_token_t arg_token) {
  const char *equity_margin_string =
      config_get_parg_value(config, arg_token, 0);
  if (equity_margin_string) {
    if (!is_decimal_number(equity_margin_string)) {
      return CONFIG_LOAD_STATUS_MALFORMED_FLOAT_ARG;
    }
    *(double *)config->fields[config_field_token]->data =
        string_to_double(equity_margin_string);
  }
  return CONFIG_LOAD_STATUS_SUCCESS;
}

// bool

void config_field_create_bool(ConfigField *config_field) {
  config_field->data = malloc_or_die(sizeof(bool));
}

config_load_status_t
config_field_set_bool(Config *config, config_field_token_t config_field_token,
                      arg_token_t arg_token) {
  const char *bool_string = config_get_parg_value(config, arg_token, 0);
  if (bool_string) {
    bool new_value;
    if (strings_iequal(bool_string, "on")) {
      new_value = true;
    } else if (strings_iequal(bool_string, "off")) {
      new_value = false;
    } else {
      return CONFIG_LOAD_STATUS_MALFORMED_BOOL_ARG;
    }
    *(bool *)config->fields[config_field_token]->data = new_value;
  }
  return CONFIG_LOAD_STATUS_SUCCESS;
}

// Board layout

void config_field_create_board_layout(ConfigField *config_field) {
  config_field->data = board_layout_create_default();
}

config_load_status_t
config_field_set_board_layout(Config *config,
                              config_field_token_t config_field_token,
                              arg_token_t arg_token) {
  const char *new_board_layout_name =
      config_get_parg_value(config, arg_token, 0);
  if (new_board_layout_name) {
    BoardLayout *board_layout = config_field_get(config, config_field_token);
    board_layout_load_status_t board_layout_load_status =
        board_layout_load(board_layout, new_board_layout_name);
    if (board_layout_load_status != BOARD_LAYOUT_LOAD_STATUS_SUCCESS) {
      return CONFIG_LOAD_STATUS_BOARD_LAYOUT_ERROR;
    }
  }
  return CONFIG_LOAD_STATUS_SUCCESS;
}

void config_field_destroy_board_layout(ConfigField *config_field) {
  board_layout_destroy(config_field->data);
}

// game variant

config_load_status_t
config_field_set_game_variant(Config *config,
                              config_field_token_t config_field_token,
                              arg_token_t arg_token) {
  const char *new_game_variant_name =
      config_get_parg_value(config, arg_token, 0);
  if (new_game_variant_name) {
    game_variant_t new_game_variant =
        get_game_variant_type_from_name(new_game_variant_name);
    if (new_game_variant == GAME_VARIANT_UNKNOWN) {
      return CONFIG_LOAD_STATUS_UNRECOGNIZED_GAME_VARIANT;
    }
    *(game_variant_t *)config->fields[config_field_token]->data =
        new_game_variant;
  }
  return CONFIG_LOAD_STATUS_SUCCESS;
}

// Win percentages

void config_field_create_win_pct(ConfigField *config_field) {
  config_field->data = win_pct_create(DEFAULT_WIN_PCT);
}

config_load_status_t
config_field_set_win_pct(Config *config,
                         config_field_token_t config_field_token,
                         arg_token_t arg_token) {
  const char *new_win_pct_name = config_get_parg_value(config, arg_token, 0);
  if (new_win_pct_name) {

    const char *existing_win_pct_name =
        config_field_get_name(config, config_field_token);
    if (strings_equal(new_win_pct_name, existing_win_pct_name)) {
      return CONFIG_LOAD_STATUS_SUCCESS;
    }
    WinPct *existing_win_pct = config_field_get(config, config_field_token);
    WinPct *new_win_pcts = win_pct_create(new_win_pct_name);
    win_pct_destroy(existing_win_pct);
    config->fields[config_field_token]->data = new_win_pcts;
    free(config->fields[config_field_token]->data_name);
    config->fields[config_field_token]->data_name =
        string_duplicate(new_win_pct_name);
  }
  return CONFIG_LOAD_STATUS_SUCCESS;
}

void config_field_destroy_win_pct(ConfigField *config_field) {
  win_pct_destroy(config_field->data);
}

// Thread control

void config_field_create_thread_control(ConfigField *config_field) {
  config_field->data = thread_control_create();
}

config_load_status_t
config_field_set_thread_control(Config *config,
                                config_field_token_t config_field_token,
                                arg_token_t __attribute((unused)) arg_token) {
  ThreadControl *thread_control = config_field_get(config, config_field_token);
  const char *new_threads =
      config_get_parg_value(config, ARG_TOKEN_NUMBER_OF_THREADS, 0);
  if (new_threads) {
    if (!is_all_digits_or_empty(new_threads)) {
      return CONFIG_LOAD_STATUS_MALFORMED_NUMBER_OF_THREADS;
    }
    thread_control_set_threads(thread_control, string_to_int(new_threads));
  }

  const char *new_print_info_interval =
      config_get_parg_value(config, ARG_TOKEN_PRINT_INFO_INTERVAL, 0);
  if (new_print_info_interval) {
    if (!is_all_digits_or_empty(new_print_info_interval)) {
      return CONFIG_LOAD_STATUS_MALFORMED_PRINT_INFO_INTERVAL;
    }
    thread_control_set_threads(thread_control,
                               string_to_int(new_print_info_interval));
  }

  const char *new_check_stop =
      config_get_parg_value(config, ARG_TOKEN_CHECK_STOP_INTERVAL, 0);
  if (new_check_stop) {
    if (!is_all_digits_or_empty(new_check_stop)) {
      return CONFIG_LOAD_STATUS_MALFORMED_CHECK_STOP_INTERVAL;
    }
    thread_control_set_threads(thread_control, string_to_int(new_check_stop));
  }
  const char *new_infile = config_get_parg_value(config, ARG_TOKEN_INFILE, 0);
  const char *new_outfile = config_get_parg_value(config, ARG_TOKEN_OUTFILE, 0);
  thread_control_set_io(thread_control, new_infile, new_outfile);
  return CONFIG_LOAD_STATUS_SUCCESS;
}

void config_field_destroy_thread_control(ConfigField *config_field) {
  thread_control_destroy(config_field->data);
}

// Exec mode

exec_mode_t get_exec_mode_type_from_name(const char *exec_mode_str) {
  exec_mode_t exec_mode = EXEC_MODE_UNKNOWN;
  if (strings_iequal(exec_mode_str, "console")) {
    exec_mode = EXEC_MODE_CONSOLE;
  } else if (strings_equal(exec_mode_str, "ucgi")) {
    exec_mode = EXEC_MODE_UCGI;
  }
  return exec_mode;
}

config_load_status_t
config_field_set_exec_mode(Config *config,
                           config_field_token_t config_field_token,
                           arg_token_t arg_token) {
  const char *new_exec_mode_name = config_get_parg_value(config, arg_token, 0);
  if (new_exec_mode_name) {
    exec_mode_t new_exec_mode =
        get_exec_mode_type_from_name(new_exec_mode_name);
    if (new_exec_mode == EXEC_MODE_UNKNOWN) {
      return CONFIG_LOAD_STATUS_UNRECOGNIZED_GAME_VARIANT;
    }
    *(exec_mode_t *)config->fields[config_field_token]->data = new_exec_mode;
  }
  return CONFIG_LOAD_STATUS_SUCCESS;
}

// Generic

void config_field_create_noop(ConfigField __attribute((unused)) *
                              config_field) {
  return;
}

void config_field_destroy_noop(ConfigField __attribute((unused)) *
                               config_field) {
  return;
}

void *config_field_get_data(ConfigField *config_field) {
  return config_field->data;
}

void config_field_set_noop(const ConfigField __attribute((unused)) *
                           config_field) {
  return;
}

void config_field_die_on_set(const ConfigField __attribute((unused)) *
                                 config_field,
                             arg_token_t __attribute((unused)) arg_token) {
  log_fatal("attempted to set unsettable config data\n");
}

void config_field_create(Config *config, config_field_token_t config_token,
                         arg_token_t arg_token,
                         config_value_create_func_t create_func,
                         config_value_destroy_func_t destroy_func,
                         config_value_load_parg_func_t load_parg_func) {
  ConfigField *config_value = malloc_or_die(sizeof(ConfigField));
  config_value->destroy_func = destroy_func;
  config_value->load_parg_func = load_parg_func;
  config_value->data = NULL;
  config_value->data_name = NULL;
  config_value->arg_token = arg_token;

  create_func(config_value);

  config->fields[config_token] = config_value;
}

void config_field_destroy(ConfigField *config_field) {
  if (!config_field) {
    return;
  }

  if (config_field->destroy_func && config_field->data) {
    config_field->destroy_func(config_field->data);
  }

  if (config_field->data_name) {
    free(config_field->data_name);
  }

  free(config_field);
}

void parsed_arg_create(Config *config, arg_token_t arg_token,
                       const char ***seqs, int num_values,
                       command_exec_func_t command_exec_func,
                       command_status_func_t command_status_func) {
  ParsedArg *parsed_arg = malloc_or_die(sizeof(ParsedArg));
  parsed_arg->num_values = num_values;
  parsed_arg->values = malloc_or_die(sizeof(char *) * parsed_arg->num_values);
  for (int i = 0; i < parsed_arg->num_values; i++) {
    parsed_arg->values[i] = NULL;
  }

  int seqs_count = 0;
  while (seqs[seqs_count] != NULL) {
    seqs_count++;
  }

  parsed_arg->seqs = malloc_or_die(sizeof(char **) * (seqs_count + 1));
  for (int i = 0; i < seqs_count; i++) {
    int seq_length = 0;
    while (seqs[i][seq_length] != NULL) {
      seq_length++;
    }
    parsed_arg->seqs[i] = malloc_or_die(sizeof(char *) * (seq_length + 1));
    for (int j = 0; j < seq_length; j++) {
      parsed_arg->seqs[i][j] = string_duplicate(seqs[i][j]);
    }
    parsed_arg->seqs[i][seq_length] = NULL;
  }
  parsed_arg->seqs[seqs_count] = NULL;

  parsed_arg->has_value = false;
  parsed_arg->exec_func = command_exec_func;
  parsed_arg->status_func = command_status_func;

  config->pargs[arg_token] = parsed_arg;
}

void parsed_arg_destroy(ParsedArg *parsed_arg) {
  if (!parsed_arg) {
    return;
  }

  // Free each sequence in seqs
  if (parsed_arg->seqs) {
    for (int i = 0; parsed_arg->seqs[i] != NULL; i++) {
      for (int j = 0; parsed_arg->seqs[i][j] != NULL; j++) {
        free((char *)parsed_arg->seqs[i][j]); // Cast to char* to match strdup
      }
      free(parsed_arg->seqs[i]);
    }
    free(parsed_arg->seqs);
  }

  // Free each value in values
  if (parsed_arg->values) {
    for (int i = 0; i < parsed_arg->num_values; i++) {
      free(parsed_arg->values[i]);
    }
    free(parsed_arg->values);
  }

  // Finally, free the ParsedArg structure itself
  free(parsed_arg);
}

Config *config_create_default() {
  Config *config = malloc_or_die(sizeof(Config));
  // FIXME: just use a single string for the args
  parsed_arg_create(config, ARG_TOKEN_CGP,
                    (const char **[]){(const char *[]){"cgp", NULL}, NULL}, 4,
                    execute_cgp_load, status_cgp_load);
  parsed_arg_create(config, ARG_TOKEN_MOVES,
                    (const char **[]){(const char *[]){"moves", NULL}, NULL}, 1,
                    execute_add_moves, status_add_moves);
  parsed_arg_create(config, ARG_TOKEN_GEN,
                    (const char **[]){(const char *[]){"gen", NULL}, NULL}, 0,
                    execute_move_gen, status_move_gen);
  parsed_arg_create(config, ARG_TOKEN_SIM,
                    (const char **[]){(const char *[]){"sim", NULL}, NULL}, 0,
                    execute_sim, status_sim);
  parsed_arg_create(config, ARG_TOKEN_SIM_KNOWN,
                    (const char **[]){(const char *[]){"simk", NULL}, NULL}, 1,
                    execute_sim, status_sim);
  parsed_arg_create(config, ARG_TOKEN_INFER,
                    (const char **[]){(const char *[]){"infer", NULL}, NULL}, 4,
                    execute_infer, status_infer);
  parsed_arg_create(config, ARG_TOKEN_AUTOPLAY,
                    (const char **[]){(const char *[]){"autoplay", NULL}, NULL},
                    0, execute_autoplay, status_autoplay);
  parsed_arg_create(config, ARG_TOKEN_CONVERT,
                    (const char **[]){(const char *[]){"convert", NULL}, NULL},
                    3, execute_convert, status_convert);
  parsed_arg_create(config, ARG_TOKEN_BINGO_BONUS,
                    (const char **[]){(const char *[]){"bb", NULL}, NULL}, 1,
                    execute_fatal, status_fatal);
  parsed_arg_create(config, ARG_TOKEN_BOARD_LAYOUT,
                    (const char **[]){(const char *[]){"bdn", NULL}, NULL}, 1,
                    execute_fatal, status_fatal);
  parsed_arg_create(config, ARG_TOKEN_GAME_VARIANT,
                    (const char **[]){(const char *[]){"var", NULL}, NULL}, 1,
                    execute_fatal, status_fatal);
  parsed_arg_create(config, ARG_TOKEN_LETTER_DISTRIBUTION,
                    (const char **[]){(const char *[]){"ld", NULL}, NULL}, 1,
                    execute_fatal, status_fatal);
  parsed_arg_create(config, ARG_TOKEN_LEXICON,
                    (const char **[]){(const char *[]){"lex", NULL}, NULL}, 1,
                    execute_fatal, status_fatal);
  parsed_arg_create(config, ARG_TOKEN_P1_NAME,
                    (const char **[]){(const char *[]){"p1", NULL}, NULL}, 1,
                    execute_fatal, status_fatal);
  parsed_arg_create(config, ARG_TOKEN_P1_LEXICON,
                    (const char **[]){(const char *[]){"l1", NULL}, NULL}, 1,
                    execute_fatal, status_fatal);
  parsed_arg_create(config, ARG_TOKEN_P1_LEAVES,
                    (const char **[]){(const char *[]){"k1", NULL}, NULL}, 1,
                    execute_fatal, status_fatal);
  parsed_arg_create(config, ARG_TOKEN_P1_MOVE_SORT_TYPE,
                    (const char **[]){(const char *[]){"s1", NULL}, NULL}, 1,
                    execute_fatal, status_fatal);
  parsed_arg_create(config, ARG_TOKEN_P1_MOVE_RECORD_TYPE,
                    (const char **[]){(const char *[]){"r1", NULL}, NULL}, 1,
                    execute_fatal, status_fatal);
  parsed_arg_create(config, ARG_TOKEN_P2_NAME,
                    (const char **[]){(const char *[]){"p2", NULL}, NULL}, 1,
                    execute_fatal, status_fatal);
  parsed_arg_create(config, ARG_TOKEN_P2_LEXICON,
                    (const char **[]){(const char *[]){"l2", NULL}, NULL}, 1,
                    execute_fatal, status_fatal);
  parsed_arg_create(config, ARG_TOKEN_P2_LEAVES,
                    (const char **[]){(const char *[]){"k2", NULL}, NULL}, 1,
                    execute_fatal, status_fatal);
  parsed_arg_create(config, ARG_TOKEN_P2_MOVE_SORT_TYPE,
                    (const char **[]){(const char *[]){"s2", NULL}, NULL}, 1,
                    execute_fatal, status_fatal);
  parsed_arg_create(config, ARG_TOKEN_P2_MOVE_RECORD_TYPE,
                    (const char **[]){(const char *[]){"r2", NULL}, NULL}, 1,
                    execute_fatal, status_fatal);
  parsed_arg_create(config, ARG_TOKEN_WIN_PCT,
                    (const char **[]){(const char *[]){"winpct", NULL}, NULL},
                    1, execute_fatal, status_fatal);
  parsed_arg_create(config, ARG_TOKEN_PLIES,
                    (const char **[]){(const char *[]){"plies", NULL}, NULL}, 1,
                    execute_fatal, status_fatal);
  parsed_arg_create(config, ARG_TOKEN_NUMBER_OF_PLAYS,
                    (const char **[]){(const char *[]){"numplays", NULL}, NULL},
                    1, execute_fatal, status_fatal);
  parsed_arg_create(config, ARG_TOKEN_MAX_ITERATIONS,
                    (const char **[]){(const char *[]){"i", NULL}, NULL}, 1,
                    execute_fatal, status_fatal);
  parsed_arg_create(config, ARG_TOKEN_STOP_COND_PCT,
                    (const char **[]){(const char *[]){"cond", NULL}, NULL}, 1,
                    execute_fatal, status_fatal);
  parsed_arg_create(config, ARG_TOKEN_EQUITY_MARGIN,
                    (const char **[]){(const char *[]){"eq", NULL}, NULL}, 1,
                    execute_fatal, status_fatal);
  parsed_arg_create(config, ARG_TOKEN_NUMBER_OF_THREADS,
                    (const char **[]){(const char *[]){"threads", NULL}, NULL},
                    1, execute_fatal, status_fatal);
  parsed_arg_create(config, ARG_TOKEN_PRINT_INFO_INTERVAL,
                    (const char **[]){(const char *[]){"info", NULL}, NULL}, 1,
                    execute_fatal, status_fatal);
  parsed_arg_create(config, ARG_TOKEN_CHECK_STOP_INTERVAL,
                    (const char **[]){(const char *[]){"check", NULL}, NULL}, 1,
                    execute_fatal, status_fatal);
  parsed_arg_create(config, ARG_TOKEN_INFILE,
                    (const char **[]){(const char *[]){"infile", NULL}, NULL},
                    1, execute_fatal, status_fatal);
  parsed_arg_create(config, ARG_TOKEN_OUTFILE,
                    (const char **[]){(const char *[]){"outfile", NULL}, NULL},
                    1, execute_fatal, status_fatal);
  parsed_arg_create(config, ARG_TOKEN_EXEC_MODE,
                    (const char **[]){(const char *[]){"mode", NULL}, NULL}, 1,
                    execute_fatal, status_fatal);

  config_field_create(config, CONFIG_FIELD_TOKEN_PLAYERS_DATA,
                      // Arg token is unused for the specialized
                      // config_field_set_players_data function
                      0, config_field_players_data_create,
                      config_field_players_data_destroy,
                      config_field_set_players_data);
  config_field_create(config, CONFIG_FIELD_TOKEN_LETTER_DISTRIBUTION,
                      ARG_TOKEN_LETTER_DISTRIBUTION, config_field_create_noop,
                      config_field_ld_destroy, config_field_set_ld);
  config_field_create(config, CONFIG_FIELD_TOKEN_BINGO_BONUS,
                      ARG_TOKEN_BINGO_BONUS, config_field_create_uint64,
                      config_field_destroy, config_field_set_uint64);
  config_field_create(config, CONFIG_FIELD_TOKEN_BOARD_LAYOUT,
                      ARG_TOKEN_BOARD_LAYOUT, config_field_create_board_layout,
                      config_field_destroy_board_layout,
                      config_field_set_board_layout);
  config_field_create(config, CONFIG_FIELD_TOKEN_GAME_VARIANT,
                      ARG_TOKEN_GAME_VARIANT, config_field_create_uint64,
                      config_field_destroy, config_field_set_game_variant);
  config_field_create(config, CONFIG_FIELD_TOKEN_EQUITY_MARGIN,
                      ARG_TOKEN_EQUITY_MARGIN, config_field_create_double,
                      config_field_destroy, config_field_set_double);
  config_field_create(config, CONFIG_FIELD_TOKEN_WIN_PCTS, ARG_TOKEN_WIN_PCT,
                      config_field_create_win_pct, config_field_destroy_win_pct,
                      config_field_set_win_pct);
  config_field_create(config, CONFIG_FIELD_TOKEN_NUM_PLAYS,
                      ARG_TOKEN_NUMBER_OF_PLAYS, config_field_create_uint64,
                      config_field_destroy, config_field_set_uint64);
  config_field_create(config, CONFIG_FIELD_TOKEN_PLIES, ARG_TOKEN_PLIES,
                      config_field_create_uint64, config_field_destroy,
                      config_field_set_uint64);
  config_field_create(config, CONFIG_FIELD_TOKEN_MAX_ITERATIONS,
                      ARG_TOKEN_MAX_ITERATIONS, config_field_create_uint64,
                      config_field_destroy, config_field_set_uint64);
  config_field_create(config, CONFIG_FIELD_TOKEN_STOP_COND_PCT,
                      ARG_TOKEN_STOP_COND_PCT, config_field_create_double,
                      config_field_destroy, config_field_set_double);
  config_field_create(config, CONFIG_FIELD_TOKEN_USE_GAME_PAIRS,
                      ARG_TOKEN_GAME_PAIRS, config_field_create_bool,
                      config_field_destroy, config_field_set_bool);
  config_field_create(config, CONFIG_FIELD_TOKEN_SEED, ARG_TOKEN_RANDOM_SEED,
                      config_field_create_uint64, config_field_destroy,
                      config_field_set_uint64);
  config_field_create(config, CONFIG_FIELD_TOKEN_THREAD_CONTROL,
                      // Arg token is unused for the specialized
                      // config_field_set_thread_control function
                      0, config_field_create_thread_control,
                      config_field_destroy_thread_control,
                      config_field_set_thread_control);
  config_field_create(config, CONFIG_FIELD_TOKEN_EXEC_MODE, ARG_TOKEN_EXEC_MODE,
                      config_field_create_uint64, config_field_destroy,
                      config_field_set_game_variant);

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
  for (int i = 0; i < NUMBER_OF_CONFIG_FIELD_TOKENS; i++) {
    config->fields[i]->destroy_func(config->fields[i]);
  }

  for (int i = 0; i < NUMBER_OF_ARG_TOKENS; i++) {
    parsed_arg_destroy(config->pargs[i]);
  }

  game_destroy(config->game);
  move_list_destroy(config->move_list);
  sim_results_destroy(config->sim_results);
  inference_results_destroy(config->inference_results);
  autoplay_results_destroy(config->autoplay_results);
  conversion_results_destroy(config->conversion_results);
  error_status_destroy(config->error_status);
}

config_load_status_t config_load_data(Config *config) {
  config_load_status_t config_load_status = CONFIG_LOAD_STATUS_SUCCESS;
  for (int i = 0; i < NUMBER_OF_CONFIG_FIELD_TOKENS; i++) {
    ConfigField *config_field = config->fields[i];
    config_load_status =
        config_field->load_parg_func(config, i, config_field->arg_token);
    if (config_load_status != CONFIG_LOAD_STATUS_SUCCESS) {
      break;
    }
  }
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
  if (config->exec_parg) {
    config->exec_parg->exec_func(config);
  }
}

char *config_get_execute_status(Config *config) {
  char *status = NULL;
  if (config->exec_parg) {
    status = config->exec_parg->status_func(config);
  }
  return status;
}