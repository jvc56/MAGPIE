#include "exec.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>

#include "../def/autoplay_defs.h"
#include "../def/config_defs.h"
#include "../def/error_stack_defs.h"
#include "../def/exec_defs.h"
#include "../def/file_handler_defs.h"
#include "../def/game_defs.h"
#include "../def/inference_defs.h"
#include "../def/thread_control_defs.h"
#include "../def/validated_move_defs.h"

#include "../ent/error_stack.h"
#include "../ent/file_handler.h"
#include "../ent/game.h"
#include "../ent/sim_results.h"
#include "../ent/thread_control.h"
#include "../ent/validated_move.h"

#include "autoplay.h"
#include "cgp.h"
#include "config.h"
#include "convert.h"
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

// This struct is used to easily pass arguments to an asynchronous command

// Returns NULL and prints a warning if a search is ongoing or some other error
// occurred
char *command_search_status(Config *config, bool should_exit) {
  if (!config) {
    log_fatal("The command variables struct has not been initialized.");
  }

  ThreadControl *thread_control = config_get_thread_control(config);

  int mode = thread_control_get_mode(thread_control);
  if (mode != MODE_SEARCHING) {
    return string_duplicate(SEARCH_STATUS_FINISHED);
  }

  if (should_exit) {
    if (!thread_control_exit(thread_control, EXIT_STATUS_USER_INTERRUPT)) {
      log_warn("Command already exited.");
    }
    thread_control_wait_for_mode_stopped(thread_control);
  }

  return config_get_execute_status(config);
}

void execute_command_and_set_mode_stopped(CommandArgs *command_args) {
  config_execute_command(command_args->config, command_args->error_stack);
  error_stack_print(command_args->error_stack);
  thread_control_set_mode_stopped(
      config_get_thread_control(command_args->config));
}

void *execute_command_thread_worker(void *uncasted_args) {
  CommandArgs *args = (CommandArgs *)uncasted_args;
  execute_command_and_set_mode_stopped(args);
  return NULL;
}

void execute_command_sync_or_async(CommandArgs *command_args,
                                   const char *command, bool sync) {
  Config *config = command_args->config;
  ErrorStack *error_stack = command_args->error_stack;
  ThreadControl *thread_control = config_get_thread_control(config);
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
  error_stack_reset(error_stack);
  config_load_command(config, command, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print(error_stack);
    thread_control_set_mode_stopped(thread_control);
    return;
  }

  if (sync) {
    execute_command_and_set_mode_stopped(command_args);
  } else {
    pthread_t cmd_execution_thread;
    pthread_create(&cmd_execution_thread, NULL, execute_command_thread_worker,
                   command_args);
    pthread_detach(cmd_execution_thread);
  }
}

void execute_command_sync(CommandArgs *command_args, const char *command) {
  execute_command_sync_or_async(command_args, command, true);
}

void execute_command_async(CommandArgs *command_args, const char *command) {
  execute_command_sync_or_async(command_args, command, false);
}

void process_ucgi_command(CommandArgs *command_args, const char *command) {
  // Assume cmd is already trimmed of whitespace
  ThreadControl *thread_control =
      config_get_thread_control(command_args->config);
  if (strings_equal(command, UCGI_COMMAND_STRING)) {
    // More of a formality to align with UCI
    thread_control_print(thread_control, "id name MAGPIE 0.1\nucgiok\n");
  } else if (strings_equal(command, STOP_COMMAND_STRING)) {
    if (thread_control_get_mode(thread_control) == MODE_SEARCHING) {
      if (!thread_control_exit(thread_control, EXIT_STATUS_USER_INTERRUPT)) {
        log_warn("Search already received stop signal but has not stopped.");
      }
    } else {
      log_warn("There is no search to stop.");
    }
  } else {
    execute_command_async(command_args, command);
  }
}

void command_scan_loop(CommandArgs *command_args,
                       const char *initial_command_string) {
  Config *config = command_args->config;
  execute_command_sync(command_args, initial_command_string);
  if (!config_continue_on_coldstart(config)) {
    return;
  }
  ThreadControl *thread_control = config_get_thread_control(config);
  char *input = NULL;
  while (1) {
    exec_mode_t exec_mode = config_get_exec_mode(config);

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

    if (strings_iequal(input, QUIT_COMMAND_STRING)) {
      break;
    }

    if (is_string_empty_or_null(input)) {
      continue;
    }

    switch (exec_mode) {
    case EXEC_MODE_CONSOLE:
      execute_command_sync(command_args, input);
      break;
    case EXEC_MODE_UCGI:
      process_ucgi_command(command_args, input);
      break;
    case EXEC_MODE_UNKNOWN:
      log_fatal("attempted to execute command in unknown mode");
      break;
    }
  }
  free(input);
}

char *create_command_from_args(int argc, char *argv[]) {
  StringBuilder *command_string_builder = string_builder_create();
  for (int i = 1; i < argc; i++) {
    string_builder_add_formatted_string(command_string_builder, "%s ", argv[i]);
  }
  char *command_string = string_builder_dump(command_string_builder, NULL);
  string_builder_destroy(command_string_builder);
  return command_string;
}

void caches_destroy(void) {
  gen_destroy_cache();
  fileproxy_destroy_cache();
}

void process_command(int argc, char *argv[], FILE *errorout) {
  ErrorStack *error_stack = error_stack_create();
  error_stack_set_output(error_stack, errorout);
  Config *config = config_create_default(error_stack);
  if (error_stack_is_empty(error_stack)) {
    CommandArgs command_args = {
        .config = config,
        .error_stack = error_stack,
    };
    char *initial_command_string = create_command_from_args(argc, argv);
    command_scan_loop(&command_args, initial_command_string);
    free(initial_command_string);
    caches_destroy();
  } else {
    error_stack_print(error_stack);
  }
  config_destroy(config);
  if (errorout != stderr) {
    fflush(errorout);
    fclose(errorout);
  }
  error_stack_destroy(error_stack);
}