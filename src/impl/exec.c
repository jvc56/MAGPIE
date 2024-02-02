#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>

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

#include "../ent/config.h"
#include "../ent/error_status.h"
#include "../ent/exec_state.h"
#include "../ent/file_handler.h"
#include "../ent/game.h"
#include "../ent/sim_results.h"
#include "../ent/thread_control.h"
#include "../ent/validated_move.h"

#include "autoplay.h"
#include "gameplay.h"
#include "inference.h"
#include "move_gen.h"
#include "simmer.h"

#include "../str/game_string.h"
#include "../str/move_string.h"
#include "../str/sim_string.h"

#include "../util/fileproxy.h"
#include "../util/log.h"
#include "../util/string_util.h"

#define UCGI_COMMAND_STRING "ucgi"
#define QUIT_COMMAND_STRING "quit"
#define STOP_COMMAND_STRING "stop"
#define FILE_COMMAND_STRING "file"

// Returns NULL and prints a warning if a search is ongoing or some other error
// occurred
char *command_search_status(ExecState *exec_state, bool should_halt) {
  if (!exec_state) {
    log_fatal("The command variables struct has not been initialized.");
  }

  ThreadControl *thread_control =
      config_get_thread_control(exec_state_get_config(exec_state));

  int mode = thread_control_get_mode(thread_control);
  if (mode != MODE_SEARCHING) {
    return string_duplicate(SEARCH_STATUS_FINISHED);
  }

  if (should_halt) {
    if (!thread_control_halt(thread_control, HALT_STATUS_USER_INTERRUPT)) {
      log_warn("Command already halted.");
    }
    thread_control_wait_for_mode_stopped(thread_control);
  }

  char *status_string = NULL;
  SimResults *sim_results = NULL;

  switch (config_get_command_type(exec_state_get_config(exec_state))) {
  case COMMAND_TYPE_SIM:
    sim_results = exec_state_get_sim_results(exec_state);
    if (!sim_results) {
      log_warn("Simmer has not been initialized.");
      return NULL;
    }
    status_string = ucgi_sim_stats(exec_state_get_game(exec_state), sim_results,
                                   thread_control, true);
    break;
  case COMMAND_TYPE_GEN:
    status_string = string_duplicate("movegen status unimplemented");
    break;
  case COMMAND_TYPE_AUTOPLAY:
    status_string = string_duplicate("autoplay status unimplemented");
    break;
  case COMMAND_TYPE_LOAD_CGP:
    status_string = string_duplicate("no status available for load cgp");
    break;
  case COMMAND_TYPE_SET_OPTIONS:
    status_string = string_duplicate("no status available for set options");
    break;
  case COMMAND_TYPE_INFER:
    status_string = string_duplicate("infer status unimplemented");
    break;
  }
  return status_string;
}

void set_or_clear_error_status(ErrorStatus *error_status,
                               error_status_t error_status_type,
                               int error_code) {
  if (error_status_is_success(error_status_type, error_code)) {
    error_status_set_type_and_code(error_status, ERROR_STATUS_TYPE_NONE, 0);
  } else {
    error_status_set_type_and_code(error_status, error_status_type, error_code);
  }
}

void execute_gen(const Config *config, ExecState *exec_state) {
  exec_state_recreate_move_list(
      exec_state, config_get_num_plays(exec_state_get_config(exec_state)));
  Game *game = exec_state_get_game(exec_state);
  MoveList *ml = exec_state_get_move_list(exec_state);
  generate_moves_for_game(game, 0, ml);
  print_ucgi_static_moves(game, ml, config_get_thread_control(config));
  set_or_clear_error_status(exec_state_get_error_status(exec_state),
                            ERROR_STATUS_TYPE_SIM, (int)GEN_STATUS_SUCCESS);
}

void execute_sim(const Config *config, ExecState *exec_state) {
  sim_status_t status = simulate(config, exec_state_get_game(exec_state),
                                 exec_state_get_move_list(exec_state),
                                 exec_state_get_sim_results(exec_state));
  set_or_clear_error_status(exec_state_get_error_status(exec_state),
                            ERROR_STATUS_TYPE_SIM, (int)status);
}

void execute_autoplay(const Config *config, ExecState *exec_state) {
  autoplay_status_t status =
      autoplay(config, exec_state_get_autoplay_results(exec_state));
  set_or_clear_error_status(exec_state_get_error_status(exec_state),
                            ERROR_STATUS_TYPE_AUTOPLAY, (int)status);
}

void execute_infer(const Config *config, ExecState *exec_state) {
  inference_status_t status =
      infer(config, exec_state_get_game(exec_state),
            exec_state_get_inference_results(exec_state));
  set_or_clear_error_status(exec_state_get_error_status(exec_state),
                            ERROR_STATUS_TYPE_INFER, (int)status);
}

move_validation_status_t update_move_list(ExecState *exec_state,
                                          const char *moves) {
  Game *game = exec_state_get_game(exec_state);
  int player_on_turn_index = game_get_player_on_turn_index(game);

  ValidatedMoves *new_validated_moves =
      validated_moves_create(game, player_on_turn_index, moves, true, false);

  move_validation_status_t move_validation_status =
      validated_moves_get_validation_status(new_validated_moves);

  if (move_validation_status == MOVE_VALIDATION_STATUS_SUCCESS) {
    const LetterDistribution *ld = game_get_ld(game);
    const Board *board = game_get_board(game);
    StringBuilder *phonies_sb = create_string_builder();
    int number_of_new_moves =
        validated_moves_get_number_of_moves(new_validated_moves);
    for (int i = 0; i < number_of_new_moves; i++) {
      char *phonies_formed = validated_moves_get_phonies_string(
          game_get_ld(game), new_validated_moves, i);
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
    exec_state_init_move_list(exec_state, number_of_new_moves);
    validated_moves_add_to_move_list(new_validated_moves,
                                     exec_state_get_move_list(exec_state));
  }

  validated_moves_destroy(new_validated_moves);

  return move_validation_status;
}

void execute_command(ExecState *exec_state) {
  // This function assumes that the config
  // is already loaded

  // Once the config is loaded, we should regard it as
  // read-only. We create a new const pointer to enforce this.
  const Config *config = exec_state_get_config(exec_state);

  if (config_get_ld(config)) {
    exec_state_init_game(exec_state);

    // Update the game with the cgp, if
    // it was specified in the config
    const char *cgp = config_get_cgp(config);
    if (cgp) {
      Game *game = exec_state_get_game(exec_state);
      // First duplicate the game so that potential
      // cgp parse failures don't corrupt it.
      Game *game_dupe = game_duplicate(game);
      cgp_parse_status_t dupe_cgp_parse_status = game_load_cgp(game_dupe, cgp);
      set_or_clear_error_status(exec_state_get_error_status(exec_state),
                                ERROR_STATUS_TYPE_CGP_LOAD,
                                (int)dupe_cgp_parse_status);
      game_destroy(game_dupe);
      if (dupe_cgp_parse_status != CGP_PARSE_STATUS_SUCCESS) {
        return;
      } else {
        // Now that the duplicate game has been successfully loaded
        // with the cgp, load the actual game. A cgp parse failure
        // here should be impossible (since the duplicated game
        // was parsed without error) and is treated as a
        // catastrophic error.
        cgp_parse_status_t cgp_parse_status = game_load_cgp(game, cgp);
        if (cgp_parse_status != CGP_PARSE_STATUS_SUCCESS) {
          log_fatal("unexpected cgp load failure for: %s", cgp);
        }
      }
    }

    // Update the validated move list
    // with new moves, if specified
    const char *moves = config_get_moves(config);
    if (moves) {
      move_validation_status_t move_validation_status =
          update_move_list(exec_state, moves);

      if (move_validation_status != MOVE_VALIDATION_STATUS_SUCCESS) {
        set_or_clear_error_status(exec_state_get_error_status(exec_state),
                                  ERROR_STATUS_TYPE_MOVE_VALIDATION,
                                  (int)move_validation_status);
        return;
      }
    }
  }

  switch (config_get_command_type(config)) {
  case COMMAND_TYPE_SET_OPTIONS:
    // this operation is just for loading the config
    // so the execution is a no-op
    break;
  case COMMAND_TYPE_LOAD_CGP:
    // Any command can potentially load
    // a CGP, so it is handled generically
    // above. No further processing is necessary.
    break;
  case COMMAND_TYPE_GEN:
    execute_gen(config, exec_state);
    break;
  case COMMAND_TYPE_SIM:
    execute_sim(config, exec_state);
    break;
  case COMMAND_TYPE_AUTOPLAY:
    execute_autoplay(config, exec_state);
    break;
  case COMMAND_TYPE_INFER:
    execute_infer(config, exec_state);
    break;
  }
}

void execute_command_and_set_mode_stopped(ExecState *exec_state) {
  execute_command(exec_state);
  error_status_log_warn_if_failed(exec_state_get_error_status(exec_state));
  thread_control_set_mode_stopped(
      config_get_thread_control(exec_state_get_config(exec_state)));
}

void *execute_command_thread_worker(void *uncasted_exec_state) {
  ExecState *exec_state = (ExecState *)uncasted_exec_state;
  execute_command_and_set_mode_stopped(exec_state);
  return NULL;
}

void execute_command_sync_or_async(ExecState *exec_state, const char *command,
                                   bool sync) {
  ThreadControl *thread_control =
      config_get_thread_control(exec_state_get_config(exec_state));
  if (!thread_control_set_mode_searching(thread_control)) {
    log_warn("still searching");
    return;
  }

  // Loading the config should always be
  // done synchronously to prevent deadlock
  // since the config load
  // needs to lock the infile FileHandler
  // to potentially change it but the
  // getline to read the next input
  // also locks the in FileHandler
  // Loading the config is relatively
  // fast so humans shouldn't notice anything
  config_load_status_t config_load_status =
      config_load(exec_state_get_config(exec_state), command);
  set_or_clear_error_status(exec_state_get_error_status(exec_state),
                            ERROR_STATUS_TYPE_CONFIG_LOAD,
                            (int)config_load_status);
  if (config_load_status != CONFIG_LOAD_STATUS_SUCCESS) {
    error_status_log_warn_if_failed(exec_state_get_error_status(exec_state));
    thread_control_set_mode_stopped(thread_control);
    return;
  }

  if (sync) {
    execute_command_and_set_mode_stopped(exec_state);
  } else {
    pthread_t cmd_execution_thread;
    pthread_create(&cmd_execution_thread, NULL, execute_command_thread_worker,
                   exec_state);
    pthread_detach(cmd_execution_thread);
  }
}

void execute_command_sync(ExecState *exec_state, const char *command) {
  execute_command_sync_or_async(exec_state, command, true);
}

void execute_command_async(ExecState *exec_state, const char *command) {
  execute_command_sync_or_async(exec_state, command, false);
}

void process_ucgi_command(ExecState *exec_state, const char *command) {
  // Assume cmd is already trimmed of whitespace
  ThreadControl *thread_control =
      config_get_thread_control(exec_state_get_config(exec_state));
  if (strings_equal(command, UCGI_COMMAND_STRING)) {
    // More of a formality to align with UCI
    thread_control_print(thread_control, "id name MAGPIE 0.1\nucgiok\n");
  } else if (strings_equal(command, STOP_COMMAND_STRING)) {
    if (thread_control_get_mode(thread_control) == MODE_SEARCHING) {
      if (!thread_control_halt(thread_control, HALT_STATUS_USER_INTERRUPT)) {
        log_warn("Search already received stop signal but has not stopped.");
      }
    } else {
      log_warn("There is no search to stop.");
    }
  } else {
    execute_command_async(exec_state, command);
  }
}

bool config_continue_on_coldstart(const Config *config) {
  command_t command_type = config_get_command_type(config);
  return command_type == COMMAND_TYPE_SET_OPTIONS ||
         command_type == COMMAND_TYPE_LOAD_CGP ||
         config_get_command_set_infile(config) ||
         config_get_command_set_exec_mode(config);
}

void command_scan_loop(ExecState *exec_state,
                       const char *initial_command_string) {
  execute_command_sync(exec_state, initial_command_string);
  if (!config_continue_on_coldstart(exec_state_get_config(exec_state))) {
    return;
  }
  ThreadControl *thread_control =
      config_get_thread_control(exec_state_get_config(exec_state));
  char *input = NULL;
  while (1) {
    exec_mode_t exec_mode =
        config_get_exec_mode(exec_state_get_config(exec_state));

    FileHandler *infile = thread_control_get_infile(thread_control);

    if (exec_mode == EXEC_MODE_CONSOLE &&
        strings_equal(STDIN_FILENAME, file_handler_get_filename(infile))) {
      thread_control_print(thread_control, "magpie>");
    }

    free(input);

    input = file_handler_get_line(infile);
    if (!input) {
      // NULL input indicates an EOF
      break;
    }

    trim_whitespace(input);

    if (strings_equal(input, QUIT_COMMAND_STRING)) {
      break;
    }

    if (is_string_empty_or_null(input)) {
      continue;
    }

    switch (exec_mode) {
    case EXEC_MODE_CONSOLE:
      execute_command_sync(exec_state, input);
      break;
    case EXEC_MODE_UCGI:
      process_ucgi_command(exec_state, input);
      break;
    }
  }
  free(input);
}

char *create_command_from_args(int argc, char *argv[]) {
  StringBuilder *command_string_builder = create_string_builder();
  for (int i = 1; i < argc; i++) {
    string_builder_add_formatted_string(command_string_builder, "%s ", argv[i]);
  }
  char *command_string = string_builder_dump(command_string_builder, NULL);
  destroy_string_builder(command_string_builder);
  return command_string;
}

void destroy_caches() {
  gen_destroy_cache();
  fileproxy_destroy_cache();
}

void process_command(int argc, char *argv[]) {
  log_set_level(LOG_WARN);
  ExecState *exec_state = exec_state_create();
  char *initial_command_string = create_command_from_args(argc, argv);
  command_scan_loop(exec_state, initial_command_string);
  free(initial_command_string);
  exec_state_destroy(exec_state);
  destroy_caches();
}