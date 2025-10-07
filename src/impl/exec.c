#include "exec.h"

#include "../compat/cpthread.h"
#include "../compat/linenoise.h"
#include "../def/config_defs.h"
#include "../def/cpthread_defs.h"
#include "../def/thread_control_defs.h"
#include "../ent/thread_control.h"
#include "../util/fileproxy.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "config.h"
#include "move_gen.h"
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>

#define UCGI_COMMAND_STRING "ucgi"
#define QUIT_COMMAND_STRING "quit"
#define STOP_COMMAND_STRING "stop"
#define FILE_COMMAND_STRING "file"

char *command_search_status(Config *config, bool should_exit) {
  if (!config) {
    log_fatal("config is unexpectedly null");
    // unreachable, but to silence static analyzer warnings
    return NULL;
  }

  ThreadControl *thread_control = config_get_thread_control(config);

  if (should_exit) {
    thread_control_set_status(thread_control,
                              THREAD_CONTROL_STATUS_USER_INTERRUPT);
  }

  return config_get_execute_status(config);
}

void execute_command_and_set_status_finished(Config *config,
                                             ErrorStack *error_stack) {
  config_execute_command(config, error_stack);
  thread_control_set_status(config_get_thread_control(config),
                            THREAD_CONTROL_STATUS_FINISHED);
}

void *execute_command_thread_worker(void *uncasted_args) {
  Config *config = (Config *)uncasted_args;
  // Create another error stack so this asynchronous command doesn't
  // interfere with the synchronous error stack on the main thread.
  ErrorStack *error_stack_async = error_stack_create();
  execute_command_and_set_status_finished(config, error_stack_async);
  error_stack_print_and_reset(error_stack_async);
  error_stack_destroy(error_stack_async);
  return NULL;
}

bool load_command_sync(Config *config, ErrorStack *error_stack,
                       const char *command) {
  ThreadControl *thread_control = config_get_thread_control(config);
  if (!thread_control_is_ready_for_new_command(thread_control)) {
    error_stack_push(
        error_stack, ERROR_STATUS_COMMAND_STILL_RUNNING,
        string_duplicate(
            "cannot execute a new command while the previous command "
            "is still running"));
    return false;
  }
  const bool reset_result =
      thread_control_set_status(thread_control, THREAD_CONTROL_STATUS_STARTED);
  assert(reset_result);
  (void)reset_result; // Suppress unused warning when NDEBUG is defined
  // Loading the config should always be done synchronously and then start the
  // execution asynchronously (if enabled)
  config_load_command(config, command, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    thread_control_set_status(config_get_thread_control(config),
                              THREAD_CONTROL_STATUS_FINISHED);
    return false;
  }

  return true;
}

void execute_command_sync(Config *config, ErrorStack *error_stack,
                          const char *command) {
  if (load_command_sync(config, error_stack, command)) {
    execute_command_and_set_status_finished(config, error_stack);
  }
}

bool run_str_api_command(Config *config, ErrorStack *error_stack,
                         const char *command, char **output) {
  if (load_command_sync(config, error_stack, command)) {
    return config_run_str_api_command(config, error_stack, output);
  }
  return false;
}

void execute_command_async(Config *config, ErrorStack *error_stack,
                           const char *command) {
  if (load_command_sync(config, error_stack, command)) {
    cpthread_t cmd_execution_thread;
    cpthread_create(&cmd_execution_thread, execute_command_thread_worker,
                    config);
    cpthread_detach(cmd_execution_thread);
  }
}

void process_ucgi_command(Config *config, ErrorStack *error_stack,
                          const char *command) {
  // Assume cmd is already trimmed of whitespace
  ThreadControl *thread_control = config_get_thread_control(config);
  if (strings_equal(command, UCGI_COMMAND_STRING)) {
    // More of a formality to align with UCI
    thread_control_print(thread_control, "id name MAGPIE 0.1\nucgiok\n");
  } else if (strings_equal(command, STOP_COMMAND_STRING)) {
    if (thread_control_is_started(thread_control)) {
      thread_control_set_status(thread_control,
                                THREAD_CONTROL_STATUS_USER_INTERRUPT);
    } else {
      error_stack_push(
          error_stack, ERROR_STATUS_COMMAND_NOTHING_TO_STOP,
          string_duplicate("no currently running command to stop"));
    }
  } else {
    execute_command_async(config, error_stack, command);
  }
}

void command_scan_loop(Config *config, ErrorStack *error_stack,
                       const char *initial_command_string) {
  execute_command_sync(config, error_stack, initial_command_string);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    return;
  }
  if (!config_continue_on_coldstart(config)) {
    return;
  }
  char *input = NULL;
  linenoiseHistorySetMaxLen(1000);
  while (1) {
    exec_mode_t exec_mode = config_get_exec_mode(config);

    const char *prompt = "";
    if (exec_mode == EXEC_MODE_CONSOLE) {
      prompt = "magpie> ";
    }

    free(input);

    input = linenoise(prompt);
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
    linenoiseHistoryAdd(input);
    switch (exec_mode) {
    case EXEC_MODE_CONSOLE:
      execute_command_sync(config, error_stack, input);
      break;
    case EXEC_MODE_UCGI:
      process_ucgi_command(config, error_stack, input);
      break;
    case EXEC_MODE_UNKNOWN:
      log_fatal("attempted to execute command in unknown mode");
      break;
    }
    if (!error_stack_is_empty(error_stack)) {
      error_stack_print_and_reset(error_stack);
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

void process_command_internal(int argc, char *argv[], const char *data_paths) {
  log_set_level(LOG_FATAL);
  ErrorStack *error_stack = error_stack_create();
  Config *config =
      config_create_default_with_data_paths(error_stack, data_paths);
  if (error_stack_is_empty(error_stack)) {
    char *initial_command_string = create_command_from_args(argc, argv);
    command_scan_loop(config, error_stack, initial_command_string);
    free(initial_command_string);
    caches_destroy();
  } else {
    error_stack_print_and_reset(error_stack);
  }
  config_destroy(config);
  error_stack_destroy(error_stack);
}

void process_command(int argc, char *argv[]) {
  process_command_internal(argc, argv, DEFAULT_DATA_PATHS);
}

void process_command_with_data_paths(int argc, char *argv[],
                                     const char *data_paths) {
  process_command_internal(argc, argv, data_paths);
}
