#include <stdlib.h>

#include "autoplay.h"
#include "command.h"
#include "config.h"
#include "error_status.h"
#include "game.h"
#include "infer.h"
#include "log.h"
#include "sim.h"
#include "thread_control.h"
#include "ucgi_print.h"
#include "util.h"

#define UCGI_COMMAND_STRING "ucgi"
#define QUIT_COMMAND_STRING "quit"
#define STOP_COMMAND_STRING "stop"
#define FILE_COMMAND_STRING "file"

typedef enum {
  COMMAND_MODE_CONSOLE,
  COMMAND_MODE_UCGI,
} command_mode_t;

CommandVars *create_command_vars(FILE *outfile) {
  CommandVars *command_vars = malloc_or_die(sizeof(CommandVars));
  command_vars->command = NULL;
  command_vars->config = NULL;
  command_vars->game = NULL;
  command_vars->simmer = NULL;
  command_vars->inference = NULL;
  command_vars->autoplay_results = NULL;
  command_vars->error_status = create_error_status(ERROR_STATUS_TYPE_NONE, 0);
  command_vars->thread_control = create_thread_control(outfile);
  command_vars->outfile = outfile;
  return command_vars;
}

void destroy_command_vars(CommandVars *command_vars) {
  // Caller needs to handle the outfile
  if (command_vars->config) {
    destroy_config(command_vars->config);
  }
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
  destroy_error_status(command_vars->error_status);
  destroy_thread_control(command_vars->thread_control);
  free(command_vars);
}

char *command_search_status(CommandVars *command_vars, bool should_halt) {
  if (!command_vars) {
    log_warn("The command variables struct has not been initialized.");
    return NULL;
  }
  if (!command_vars->thread_control) {
    // log fatally since there should always be
    // a thread control
    log_fatal("Thread controller has not been initialized.");
  }

  int mode = get_mode(command_vars->thread_control);
  if (mode != MODE_SEARCHING) {
    log_warn("Not currently searching.");
    return NULL;
  }

  if (should_halt) {
    if (!halt(command_vars->thread_control, HALT_STATUS_USER_INTERRUPT)) {
      log_warn("Command already halted.");
    }
    wait_for_mode_stopped(command_vars->thread_control);
  }

  char *status_string = NULL;
  switch (command_vars->config->command_type) {
  case COMMAND_TYPE_UNKNOWN:
    log_warn("Unknown command type");
    break;
  case COMMAND_TYPE_SIM:
    if (!command_vars->simmer) {
      log_warn("Simmer has not been initialized.");
      return NULL;
    }
    // FIXME: need an option for ucgi vs. human readable
    // since the command module is an abstraction layer
    // above UCGI.
    status_string = ucgi_sim_stats(command_vars->simmer, command_vars->game, 1);
    break;
  case COMMAND_TYPE_AUTOPLAY:
    log_warn("autoplay status unimplemented");
    break;
  case COMMAND_TYPE_LOAD_CGP:
    log_warn("no status available for load cgp");
    break;
  case COMMAND_TYPE_SET_OPTIONS:
    log_warn("no status available for set options");
    break;
  case COMMAND_TYPE_INFER:
    log_warn("infer status unimplemented");
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

void execute_sim(CommandVars *command_vars, const Config *config) {
  if (!command_vars->simmer) {
    command_vars->simmer = create_simmer(config);
  }
  sim_status_t status = simulate(config, command_vars->thread_control,
                                 command_vars->simmer, command_vars->game);
  set_error_status(command_vars->error_status, ERROR_STATUS_TYPE_SIM,
                   (int)status);
}

void execute_autoplay(CommandVars *command_vars, const Config *config) {
  if (!command_vars->autoplay_results) {
    command_vars->autoplay_results = create_autoplay_results();
  }
  autoplay_status_t status = autoplay(config, command_vars->thread_control,
                                      command_vars->autoplay_results);
  set_error_status(command_vars->error_status, ERROR_STATUS_TYPE_AUTOPLAY,
                   (int)status);
}

void execute_infer(CommandVars *command_vars, const Config *config) {
  if (!command_vars->inference) {
    command_vars->inference = create_inference();
  }
  inference_status_t status =
      infer(config, command_vars->thread_control, command_vars->game,
            command_vars->inference);
  set_error_status(command_vars->error_status, ERROR_STATUS_TYPE_INFER,
                   (int)status);
}

void load_thread_control(CommandVars *command_vars, const Config *config) {
  command_vars->thread_control->number_of_threads = config->number_of_threads;
  command_vars->thread_control->print_info_interval =
      config->print_info_interval;
  command_vars->thread_control->check_stopping_condition_interval =
      config->check_stopping_condition_interval;
}

void execute_command(CommandVars *command_vars) {
  if (!command_vars->config) {
    command_vars->config = create_default_config();
  }

  config_load_status_t config_load_status =
      load_config(command_vars->config, command_vars->command);
  set_error_status(command_vars->error_status, ERROR_STATUS_TYPE_CONFIG_LOAD,
                   (int)config_load_status);
  if (config_load_status != CONFIG_LOAD_STATUS_SUCCESS) {
    return;
  }

  // Once the config is loaded, we should regard it as
  // read-only. We create a new const pointer to enforce this.
  const Config *config = command_vars->config;

  update_or_create_game(config, command_vars);

  if (config->command_set_cgp) {
    cgp_parse_status_t cgp_parse_status =
        load_cgp(command_vars->game, config->cgp);
    set_error_status(command_vars->error_status, ERROR_STATUS_TYPE_CGP_LOAD,
                     (int)cgp_parse_status);
    if (cgp_parse_status != CGP_PARSE_STATUS_SUCCESS) {
      return;
    }
  }

  load_thread_control(command_vars, config);
  switch (config->command_type) {
  case COMMAND_TYPE_UNKNOWN:
    set_error_status(command_vars->error_status, ERROR_STATUS_TYPE_CONFIG_LOAD,
                     (int)CONFIG_LOAD_STATUS_UNRECOGNIZED_COMMAND);
    break;
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
    execute_sim(command_vars, config);
    break;
  case COMMAND_TYPE_AUTOPLAY:
    execute_autoplay(command_vars, config);
    break;
  case COMMAND_TYPE_INFER:
    execute_infer(command_vars, config);
    break;
  }
}

void execute_command_and_set_mode_stopped(CommandVars *command_vars) {
  execute_command(command_vars);
  // FIXME: seems inelegant, find a better way to show errors
  if (command_vars->error_status->type != ERROR_STATUS_TYPE_NONE) {
    char *error_status_string =
        error_status_to_string(command_vars->error_status);
    print_to_file(command_vars->thread_control, error_status_string);
    free(error_status_string);
  }
  set_mode_stopped(command_vars->thread_control);
}

void *execute_command_thread_worker(void *uncasted_command_vars) {
  CommandVars *command_vars = (CommandVars *)uncasted_command_vars;
  execute_command_and_set_mode_stopped(command_vars);
  return NULL;
}

void execute_command_sync_or_async(CommandVars *command_vars, bool sync) {
  if (!set_mode_searching(command_vars->thread_control)) {
    log_warn("still searching");
    return;
  }
  unhalt(command_vars->thread_control);
  if (sync) {
    execute_command_and_set_mode_stopped(command_vars);
  } else {
    pthread_t cmd_execution_thread;
    pthread_create(&cmd_execution_thread, NULL, execute_command_thread_worker,
                   command_vars);
    pthread_detach(cmd_execution_thread);
  }
}

void execute_command_sync(CommandVars *command_vars) {
  execute_command_sync_or_async(command_vars, true);
}

void execute_command_async(CommandVars *command_vars) {
  execute_command_sync_or_async(command_vars, false);
}

void execute_single_command_sync(const char *command) {
  CommandVars *command_vars = create_command_vars(stdout);
  command_vars->command = command;
  execute_command_sync(command_vars);
  destroy_command_vars(command_vars);
}

void process_ucgi_command(CommandVars *command_vars) {
  // Assume cmd is already trimmed of whitespace
  if (strings_equal(command_vars->command, UCGI_COMMAND_STRING)) {
    // More of a formality to align with UCI
    print_to_file(command_vars->thread_control, "id name MAGPIE 0.1\nucgiok\n");
  } else if (strings_equal(command_vars->command, STOP_COMMAND_STRING)) {
    if (get_mode(command_vars->thread_control) == MODE_SEARCHING) {
      if (!halt(command_vars->thread_control, HALT_STATUS_USER_INTERRUPT)) {
        log_warn("Search already received stop signal but has not stopped.");
      }
    } else {
      log_info("There is no search to stop.");
    }
  } else {
    execute_command_async(command_vars);
  }
}

void command_scan_loop(command_mode_t command_mode_type) {
  CommandVars *command_vars = create_command_vars(stdout);
  char *input = NULL;
  size_t input_size = 0;
  ssize_t input_length;
  while (1) {
    if (command_mode_type == COMMAND_MODE_CONSOLE) {
      print_to_file(command_vars->thread_control, "magpie>");
    }
    input_length = getline(&input, &input_size, stdin);

    if (input_length == -1) {
      log_fatal("error reading input\n");
    }

    trim_whitespace(input);

    if (strings_equal(input, QUIT_COMMAND_STRING)) {
      break;
    }

    if (is_string_empty_or_null(input)) {
      continue;
    }

    command_vars->command = input;
    if (command_mode_type == COMMAND_MODE_UCGI) {
      process_ucgi_command(command_vars);
    } else if (command_mode_type == COMMAND_MODE_CONSOLE) {
      execute_command_sync(command_vars);
    }
  }
  destroy_command_vars(command_vars);
  free(input);
}

void execute_command_file_sync(const char *filename) {
  StringSplitter *commands = split_file_by_newline(filename);
  int number_of_commands = string_splitter_get_number_of_items(commands);

  CommandVars *command_vars = create_command_vars(stdout);

  for (int i = 0; i < number_of_commands; i++) {
    command_vars->command = string_splitter_get_item(commands, i);
    execute_command_sync(command_vars);
  }

  destroy_command_vars(command_vars);
  destroy_string_splitter(commands);
}

void process_command(int argc, char *argv[]) {
  if (argc == 1) {
    // Use console mode by default
    command_scan_loop(COMMAND_MODE_CONSOLE);
  } else if (argc == 2 && strings_equal(argv[1], UCGI_COMMAND_STRING)) {
    command_scan_loop(COMMAND_MODE_UCGI);
  } else if (argc == 3 && strings_equal(argv[1], FILE_COMMAND_STRING)) {
    execute_command_file_sync(argv[2]);
  } else {
    StringBuilder *command_string_builder = create_string_builder();
    for (int i = 1; i < argc; i++) {
      string_builder_add_formatted_string(command_string_builder, "%s ",
                                          argv[i]);
    }
    execute_single_command_sync(string_builder_peek(command_string_builder));
    destroy_string_builder(command_string_builder);
  }
}