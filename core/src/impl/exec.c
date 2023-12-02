#include <stdlib.h>

#include "autoplay_results.h"
#include "command.h"
#include "config.h"
#include "error_status.h"
#include "game.h"
#include "infer.h"
#include "log.h"
#include "sim.h"
#include "thread_control.h"
#include "ucgi_print.h"
#include "../util/util.h"

#define UCGI_COMMAND_STRING "ucgi"
#define QUIT_COMMAND_STRING "quit"
#define STOP_COMMAND_STRING "stop"
#define FILE_COMMAND_STRING "file"

struct CommandVars {
  char *command;
  Config *config;
  Game *game;
  Simmer *simmer;
  Inference *inference;
  AutoplayResults *autoplay_results;
  ErrorStatus *error_status;
};

Game *get_game(const CommandVars *cmd_vars) { return cmd_vars->game; }

CommandVars *create_command_vars() {
  CommandVars *command_vars = malloc_or_die(sizeof(CommandVars));
  command_vars->command = NULL;
  command_vars->game = NULL;
  command_vars->simmer = NULL;
  command_vars->inference = NULL;
  command_vars->autoplay_results = NULL;
  command_vars->config = create_default_config();
  command_vars->error_status = create_error_status();
  return command_vars;
}

void destroy_command_vars(CommandVars *command_vars) {
  if (command_vars->game) {
    destroy_game(command_vars->game);
  }
  if (command_vars->simmer) {
    destroy_simmer(command_vars->simmer);
  }
  if (command_vars->inference) {
    destroy_inference(command_vars->inference);
  }
  if (command_vars->autoplay_results) {
    destroy_autoplay_results(command_vars->autoplay_results);
  }
  destroy_config(command_vars->config);
  destroy_error_status(command_vars->error_status);
  free(command_vars->command);
  free(command_vars);
}

void set_command(CommandVars *command_vars, const char *command) {
  free(command_vars->command);
  command_vars->command = string_duplicate(command);
}

// Returns NULL and prints a warning if a search is ongoing or some other error
// occurred
char *command_search_status(CommandVars *command_vars, bool should_halt) {
  if (!command_vars) {
    log_warn("The command variables struct has not been initialized.");
    return NULL;
  }

  ThreadControl *thread_control = command_vars->config->thread_control;

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
  switch (command_vars->config->command_type) {
  case COMMAND_TYPE_SIM:
    if (!command_vars->simmer) {
      log_warn("Simmer has not been initialized.");
      return NULL;
    }
    // FIXME: need an option for ucgi vs. human readable
    // since the command module is an abstraction layer
    // above UCGI.
    status_string = ucgi_sim_stats(command_vars->game, command_vars->simmer, 1);
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

void update_or_create_game(const Config *config, CommandVars *command_vars) {
  if (!command_vars->game) {
    command_vars->game = create_game(config);
  } else {
    update_game(config, command_vars->game);
  }
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

void execute_sim(const Config *config, CommandVars *command_vars) {
  if (!command_vars->simmer) {
    command_vars->simmer = create_simmer(config);
  }
  sim_status_t status =
      simulate(config, command_vars->game, command_vars->simmer);
  set_or_clear_error_status(command_vars->error_status, ERROR_STATUS_TYPE_SIM,
                            (int)status);
}

void execute_autoplay(const Config *config, CommandVars *command_vars) {
  if (!command_vars->autoplay_results) {
    command_vars->autoplay_results = create_autoplay_results();
  }
  autoplay_status_t status = autoplay(config, command_vars->autoplay_results);
  set_or_clear_error_status(command_vars->error_status,
                            ERROR_STATUS_TYPE_AUTOPLAY, (int)status);
}

void execute_infer(const Config *config, CommandVars *command_vars) {
  if (!command_vars->inference) {
    command_vars->inference = create_inference();
  }
  inference_status_t status =
      infer(config, command_vars->game, command_vars->inference);
  set_or_clear_error_status(command_vars->error_status, ERROR_STATUS_TYPE_INFER,
                            (int)status);
}

void execute_command(CommandVars *command_vars) {
  // This function assumes that the config
  // is already loaded

  // Once the config is loaded, we should regard it as
  // read-only. We create a new const pointer to enforce this.
  const Config *config = command_vars->config;

  // If the lexicons aren't loaded, this is
  // guaranteed to be a set options
  // command and the rest of the function
  // will be a no-op.
  if (config->lexicons_loaded) {
    update_or_create_game(config, command_vars);

    if (config->command_set_cgp) {
      cgp_parse_status_t cgp_parse_status =
          load_cgp(command_vars->game, config->cgp);
      set_or_clear_error_status(command_vars->error_status,
                                ERROR_STATUS_TYPE_CGP_LOAD,
                                (int)cgp_parse_status);
      if (cgp_parse_status != CGP_PARSE_STATUS_SUCCESS) {
        return;
      }
    }
  }

  switch (config->command_type) {
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
    execute_sim(config, command_vars);
    break;
  case COMMAND_TYPE_AUTOPLAY:
    execute_autoplay(config, command_vars);
    break;
  case COMMAND_TYPE_INFER:
    execute_infer(config, command_vars);
    break;
  }
}

void execute_command_and_set_mode_stopped(CommandVars *command_vars) {
  execute_command(command_vars);
  log_warn_if_failed(command_vars->error_status);
  set_mode_stopped(command_vars->config->thread_control);
}

void *execute_command_thread_worker(void *uncasted_command_vars) {
  CommandVars *command_vars = (CommandVars *)uncasted_command_vars;
  execute_command_and_set_mode_stopped(command_vars);
  return NULL;
}

void execute_command_sync_or_async(CommandVars *command_vars,
                                   const char *command, bool sync) {
  if (!set_mode_searching(command_vars->config->thread_control)) {
    log_warn("still searching");
    return;
  }

  set_command(command_vars, command);

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
      load_config(command_vars->config, command_vars->command);
  set_or_clear_error_status(command_vars->error_status,
                            ERROR_STATUS_TYPE_CONFIG_LOAD,
                            (int)config_load_status);
  if (config_load_status != CONFIG_LOAD_STATUS_SUCCESS) {
    log_warn_if_failed(command_vars->error_status);
    set_mode_stopped(command_vars->config->thread_control);
    return;
  }

  if (sync) {
    execute_command_and_set_mode_stopped(command_vars);
  } else {
    pthread_t cmd_execution_thread;
    pthread_create(&cmd_execution_thread, NULL, execute_command_thread_worker,
                   command_vars);
    pthread_detach(cmd_execution_thread);
  }
}

void execute_command_sync(CommandVars *command_vars, const char *command) {
  execute_command_sync_or_async(command_vars, command, true);
}

void execute_command_async(CommandVars *command_vars, const char *command) {
  execute_command_sync_or_async(command_vars, command, false);
}

void process_ucgi_command(CommandVars *command_vars, const char *command) {
  // Assume cmd is already trimmed of whitespace
  ThreadControl *thread_control = command_vars->config->thread_control;
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
    execute_command_async(command_vars, command);
  }
}

bool continue_on_coldstart(const Config *config) {
  return config->command_type == COMMAND_TYPE_SET_OPTIONS ||
         config->command_type == COMMAND_TYPE_LOAD_CGP ||
         config->command_set_infile || config->command_set_exec_mode;
}

void command_scan_loop(CommandVars *command_vars,
                       const char *initial_command_string) {
  execute_command_sync(command_vars, initial_command_string);
  if (!continue_on_coldstart(command_vars->config)) {
    return;
  }
  char *input = NULL;
  while (1) {
    exec_mode_t exec_mode = command_vars->config->exec_mode;

    FileHandler *infile = command_vars->config->thread_control->infile;

    if (exec_mode == EXEC_MODE_CONSOLE &&
        strings_equal(STDIN_FILENAME, get_file_handler_filename(infile))) {
      print_to_outfile(command_vars->config->thread_control, "magpie>");
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
      execute_command_sync(command_vars, input);
      break;
    case EXEC_MODE_UCGI:
      process_ucgi_command(command_vars, input);
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
  CommandVars *command_vars = create_command_vars();
  char *initial_command_string = create_command_from_args(argc, argv);
  command_scan_loop(command_vars, initial_command_string);
  free(initial_command_string);
  destroy_command_vars(command_vars);
}