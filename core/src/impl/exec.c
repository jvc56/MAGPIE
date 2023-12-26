#include <stdlib.h>

#include "../def/autoplay_defs.h"
#include "../def/file_handler_defs.h"
#include "../def/inference_defs.h"

#include "../util/log.h"
#include "../util/util.h"

#include "../str/sim_string.h"
#include "../util/string_util.h"

#include "../ent/autoplay_results.h"
#include "../ent/config.h"
#include "../ent/error_status.h"
#include "../ent/exec_state.h"
#include "../ent/file_handler.h"
#include "../ent/game.h"
#include "../ent/letter_distribution.h"
#include "../ent/thread_control.h"

#include "autoplay.h"
#include "inference.h"
#include "simmer.h"

#define UCGI_COMMAND_STRING "ucgi"
#define QUIT_COMMAND_STRING "quit"
#define STOP_COMMAND_STRING "stop"
#define FILE_COMMAND_STRING "file"

// Returns NULL and prints a warning if a search is ongoing or some other error
// occurred
char *command_search_status(ExecState *exec_state, bool should_halt) {
  if (!exec_state) {
    log_warn("The command variables struct has not been initialized.");
    return NULL;
  }

  ThreadControl *thread_control =
      config_get_thread_control(exec_state_get_config(exec_state));

  int mode = get_mode(thread_control);
  if (mode != MODE_SEARCHING) {
    log_warn("Not currently searching.");
    return NULL;
  }

  if (should_halt) {
    if (!halt(thread_control, HALT_STATUS_USER_INTERRUPT)) {
      log_warn("Command already halted.");
    }
    wait_for_mode_stopped(thread_control);
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
    // FIXME: need an option for ucgi vs. human readable
    // since the command module is an abstraction layer
    // above UCGI.
    status_string = ucgi_sim_stats(exec_state_get_game(exec_state), sim_results,
                                   thread_control, true);
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

// This function recreates fields of the command vars
// which have dynamically sized allocations that can
// change based on the config.
void recreate_game(const Config *config, ExecState *exec_state) {
  if (exec_state_get_game(exec_state)) {
    destroy_game(exec_state_get_game(exec_state));
  }
  exec_state_set_game(exec_state, create_game(config));
}

void set_or_clear_error_status(ErrorStatus *error_status,
                               error_status_t error_status_type,
                               int error_code) {
  if (is_successful_error_code(error_status_type, error_code)) {
    set_error_status(error_status, ERROR_STATUS_TYPE_NONE, 0);
  } else {
    set_error_status(error_status, error_status_type, error_code);
  }
}

void execute_sim(const Config *config, ExecState *exec_state) {
  SimResults *sim_results = exec_state_get_sim_results(exec_state);
  sim_status_t status =
      simulate(config, exec_state_get_game(exec_state), &sim_results);
  // The sim results could have been a NULL pointer, so we
  // have to set the potentially newly created sim_results here.
  exec_state_set_sim_results(exec_state, sim_results);
  set_or_clear_error_status(exec_state_get_error_status(exec_state),
                            ERROR_STATUS_TYPE_SIM, (int)status);
}

void execute_autoplay(const Config *config, ExecState *exec_state) {
  AutoplayResults *autoplay_results =
      exec_state_get_autoplay_results(exec_state);
  autoplay_status_t status = autoplay(config, &autoplay_results);
  exec_state_set_autoplay_results(exec_state, autoplay_results);
  set_or_clear_error_status(exec_state_get_error_status(exec_state),
                            ERROR_STATUS_TYPE_AUTOPLAY, (int)status);
}

void execute_infer(const Config *config, ExecState *exec_state) {
  InferenceResults *inference_results =
      exec_state_get_inference_results(exec_state);
  inference_status_t status =
      infer(config, exec_state_get_game(exec_state), &inference_results);
  exec_state_set_inference_results(exec_state, inference_results);
  set_or_clear_error_status(exec_state_get_error_status(exec_state),
                            ERROR_STATUS_TYPE_INFER, (int)status);
}

void execute_command(ExecState *exec_state) {
  // This function assumes that the config
  // is already loaded

  // Once the config is loaded, we should regard it as
  // read-only. We create a new const pointer to enforce this.
  const Config *config = exec_state_get_config(exec_state);

  // If the lexicons aren't loaded, this is
  // guaranteed to be a set options
  // command and the rest of the function
  // will be a no-op.
  if (config_get_lexicons_loaded(config)) {
    // FIXME: only reset if the lexicons or letter distribution changes
    recreate_game(config, exec_state);

    if (config_get_command_set_cgp(config)) {
      cgp_parse_status_t cgp_parse_status =
          load_cgp(exec_state_get_game(exec_state), config_get_cgp(config));
      set_or_clear_error_status(exec_state_get_error_status(exec_state),
                                ERROR_STATUS_TYPE_CGP_LOAD,
                                (int)cgp_parse_status);
      if (cgp_parse_status != CGP_PARSE_STATUS_SUCCESS) {
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
  log_warn_if_failed(exec_state_get_error_status(exec_state));
  set_mode_stopped(
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
  if (!set_mode_searching(thread_control)) {
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
      load_config(exec_state_get_config(exec_state), command);
  set_or_clear_error_status(exec_state_get_error_status(exec_state),
                            ERROR_STATUS_TYPE_CONFIG_LOAD,
                            (int)config_load_status);
  if (config_load_status != CONFIG_LOAD_STATUS_SUCCESS) {
    log_warn_if_failed(exec_state_get_error_status(exec_state));
    set_mode_stopped(thread_control);
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
    print_to_outfile(thread_control, "id name MAGPIE 0.1\nucgiok\n");
  } else if (strings_equal(command, STOP_COMMAND_STRING)) {
    if (get_mode(thread_control) == MODE_SEARCHING) {
      if (!halt(thread_control, HALT_STATUS_USER_INTERRUPT)) {
        log_warn("Search already received stop signal but has not stopped.");
      }
    } else {
      log_warn("There is no search to stop.");
    }
  } else {
    execute_command_async(exec_state, command);
  }
}

bool continue_on_coldstart(const Config *config) {
  command_t command_type = config_get_command_type(config);
  return command_type == COMMAND_TYPE_SET_OPTIONS ||
         command_type == COMMAND_TYPE_LOAD_CGP ||
         config_get_command_set_infile(config) ||
         config_get_command_set_exec_mode(config);
}

void command_scan_loop(ExecState *exec_state,
                       const char *initial_command_string) {
  execute_command_sync(exec_state, initial_command_string);
  if (!continue_on_coldstart(exec_state_get_config(exec_state))) {
    return;
  }
  ThreadControl *thread_control =
      config_get_thread_control(exec_state_get_config(exec_state));
  char *input = NULL;
  while (1) {
    exec_mode_t exec_mode =
        config_get_exec_mode(exec_state_get_config(exec_state));

    FileHandler *infile = get_infile(thread_control);

    if (exec_mode == EXEC_MODE_CONSOLE &&
        strings_equal(STDIN_FILENAME, get_file_handler_filename(infile))) {
      print_to_outfile(thread_control, "magpie>");
    }

    free(input);

    input = getline_from_file(infile);
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

void process_command(int argc, char *argv[]) {
  log_set_level(LOG_WARN);
  ExecState *exec_state = create_exec_state();
  char *initial_command_string = create_command_from_args(argc, argv);
  command_scan_loop(exec_state, initial_command_string);
  free(initial_command_string);
  destroy_exec_state(exec_state);
}