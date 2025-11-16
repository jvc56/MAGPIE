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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/poll.h>
#include <unistd.h>

typedef enum {
  ASYNC_STOP_COMMAND_TOKEN,
  ASYNC_STATUS_COMMAND_TOKEN,
  NUMBER_OF_ASYNC_COMMAND_TOKENS,
} async_token_t;

// The order of these strings must match the order of the tokens
static const char *async_command_strings[] = {
    "stop",
    "status",
};

typedef struct AsyncCommandInputArgs {
  int *pipefds;
  struct pollfd *fds;
  Config *config;
} AsyncCommandInputArgs;

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
  error_stack_print_and_reset(error_stack);
}

void *execute_async_command_worker(void *uncasted_args) {
  Config *config = (Config *)uncasted_args;
  // Create another error stack so this asynchronous command doesn't
  // interfere with the synchronous error stack on the main thread.
  ErrorStack *error_stack_async = error_stack_create();
  execute_command_and_set_status_finished(config, error_stack_async);
  error_stack_destroy(error_stack_async);
  return NULL;
}

bool load_command_sync(Config *config, ErrorStack *error_stack,
                       const char *command) {
  ThreadControl *thread_control = config_get_thread_control(config);
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

// Returns the async token of the command, or NUMBER_OF_ASYNC_COMMAND_TOKENS
// if the command was not recognized.
// Clears the string builder at the beginning of the function
async_token_t parse_async_command(const char *command, StringBuilder *sb) {
  async_token_t token = NUMBER_OF_ASYNC_COMMAND_TOKENS;
  string_builder_clear(sb);
  for (int i = 0; i < NUMBER_OF_ASYNC_COMMAND_TOKENS; i++) {
    const char *token_to_eval_string = async_command_strings[i];
    if (has_iprefix(command, token_to_eval_string)) {
      if (token == NUMBER_OF_ASYNC_COMMAND_TOKENS) {
        token = i;
      } else if (string_builder_length(sb) == 0) {
        string_builder_add_formatted_string(
            sb, "ambiguous async command '%s' could be: %s, %s", command,
            async_command_strings[token], token_to_eval_string);
      } else {
        string_builder_add_formatted_string(sb, ", %s", token_to_eval_string);
      }
    }
  }
  return token;
}

void *execute_async_input_worker(void *uncasted_args) {
  AsyncCommandInputArgs *args = (AsyncCommandInputArgs *)uncasted_args;
  Config *config = args->config;
  ThreadControl *thread_control = config_get_thread_control(config);
  ErrorStack *error_stack = error_stack_create();
  StringBuilder *err_msg_sb = string_builder_create();
  char *input = NULL;
  while (thread_control_get_status(thread_control) ==
         THREAD_CONTROL_STATUS_STARTED) {
    int ret = poll(args->fds, 2, -1); // wait indefinitely
    if (ret == -1) {
      perror("poll");
      log_fatal("unexpected error while polling in async input worker");
    }
    free(input);
    input = NULL;
    if (args->fds[0].revents) {
      input = read_line_from_stream_in();
    } else {
      // Since there are only 2 pipes, fds[1] has necessarily been written to,
      // indicating that the async command has finished and we can stop
      // listening for async inputs
      break;
    }

    async_token_t input_token = parse_async_command(input, err_msg_sb);
    if (string_builder_length(err_msg_sb) > 0) {
      error_stack_push(error_stack, ERROR_STATUS_CONFIG_LOAD_AMBIGUOUS_COMMAND,
                       string_builder_dump(err_msg_sb, NULL));
      error_stack_print_and_reset(error_stack);
    } else if (input_token == NUMBER_OF_ASYNC_COMMAND_TOKENS) {
      error_stack_push(
          error_stack, ERROR_STATUS_CONFIG_LOAD_UNRECOGNIZED_ARG,
          get_formatted_string("unrecognized async command '%s'", input));
      error_stack_print_and_reset(error_stack);
    } else {
      switch (input_token) {
      case ASYNC_STOP_COMMAND_TOKEN:
        thread_control_set_status(thread_control,
                                  THREAD_CONTROL_STATUS_USER_INTERRUPT);
        break;
      case ASYNC_STATUS_COMMAND_TOKEN:;
        char *status_str = config_get_execute_status(config);
        thread_control_print(config_get_thread_control(config), status_str);
        free(status_str);
        break;
      case NUMBER_OF_ASYNC_COMMAND_TOKENS:
        log_fatal("found unexpected async command token");
        break;
      }
    }
  }
  free(input);
  string_builder_destroy(err_msg_sb);
  error_stack_destroy(error_stack);
  return NULL;
}

// Blocks until the async command is finished
void execute_command_async(Config *config, ErrorStack *error_stack,
                           const char *command) {
  if (!load_command_sync(config, error_stack, command)) {
    return;
  }

  int pipefds[2];
  if (pipe(pipefds) == -1) {
    perror("pipe");
    log_fatal("failed to create pipe for async command");
  }

  cpthread_t cmd_execution_thread;
  cpthread_create(&cmd_execution_thread, execute_async_command_worker, config);

  // FIXME: wrap pollfd
  struct pollfd fds[2];
  fds[0].fd = fileno(get_stream_in());
  fds[0].events = POLLIN;
  fds[1].fd = pipefds[0];
  fds[1].events = POLLIN;

  AsyncCommandInputArgs async_args;
  async_args.pipefds = pipefds;
  async_args.fds = fds;
  async_args.config = config;

  cpthread_t cmd_input_thread;
  cpthread_create(&cmd_input_thread, execute_async_input_worker, &async_args);

  ThreadControl *thread_control = config_get_thread_control(config);
  while (thread_control_get_status(thread_control) ==
         THREAD_CONTROL_STATUS_STARTED) {
    thread_control_wait_for_status_change(thread_control);
    if (thread_control_get_status(thread_control) ==
        THREAD_CONTROL_STATUS_FINISHED) {
      if (write(pipefds[1], "x", 1) == -1) {
        log_fatal("failed to write to async command input pipe");
      }
    }
  }

  cpthread_join(cmd_execution_thread);

  // Do to race conditions, the async input thread might still
  // be blocked on polling, so send a signal here to wake it up
  if (write(pipefds[1], "x", 1) == -1) {
    log_fatal("final write to async command input pipe failed");
  }

  cpthread_join(cmd_input_thread);
  close(pipefds[0]);
  close(pipefds[1]);
}

void save_config_settings(const Config *config, ErrorStack *error_stack) {
  if (!config_get_save_settings(config)) {
    return;
  }
  StringBuilder *sb = string_builder_create();
  config_add_settings_to_string_builder(config, sb);
  write_string_to_file(CONFIG_SETTINGS_FILENAME_WITH_EXTENSION, "w",
                       string_builder_peek(sb), error_stack);
  string_builder_destroy(sb);
}

void load_config_settings(Config *config, ErrorStack *error_stack) {
  // if file does not exist, just return
  if (access(CONFIG_SETTINGS_FILENAME_WITH_EXTENSION, F_OK) != 0) {
    return;
  }
  char *settings_string = get_string_from_file(
      CONFIG_SETTINGS_FILENAME_WITH_EXTENSION, error_stack);
  StringSplitter *settings_split_by_newline =
      split_string_by_newline(settings_string, true);
  const int num_lines =
      string_splitter_get_number_of_items(settings_split_by_newline);
  for (int i = 0; i < num_lines; i++) {
    const char *line = string_splitter_get_item(settings_split_by_newline, i);
    execute_command_sync(config, error_stack, line);
    if (!error_stack_is_empty(error_stack)) {
      break;
    }
  }
  string_splitter_destroy(settings_split_by_newline);
  free(settings_string);
}

void sync_command_scan_loop(Config *config, ErrorStack *error_stack,
                            const char *initial_command_string) {
  // To suppress 'finished' for internal load settings commands
  config_set_loaded_settings(config, false);
  load_config_settings(config, error_stack);
  config_set_loaded_settings(config, true);
  if (!error_stack_is_empty(error_stack)) {
    error_stack_print_and_reset(error_stack);
    return;
  }
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
    const char *prompt_text = "";
    if (config_get_show_prompt(config)) {
      prompt_text = "magpie> ";
    }
    free(input);
    input = linenoise(prompt_text);
    if (!input) {
      // NULL input indicates an EOF
      break;
    }

    trim_whitespace(input);

    if (is_string_empty_or_null(input)) {
      continue;
    }

    if (strings_iequal("quit", input)) {
      break;
    }

    if (strings_iequal(input,
                       async_command_strings[ASYNC_STOP_COMMAND_TOKEN])) {
      error_stack_push(
          error_stack, ERROR_STATUS_COMMAND_NOTHING_TO_STOP,
          string_duplicate("no currently running command to stop"));
    } else {
      linenoiseHistoryAdd(input);
      switch (config_get_exec_mode(config)) {
      case EXEC_MODE_SYNC:
        execute_command_sync(config, error_stack, input);
        break;
      case EXEC_MODE_ASYNC:
        execute_command_async(config, error_stack, input);
        break;
      case EXEC_MODE_UNKNOWN:
        log_fatal("attempted to execute command in unknown mode");
        break;
      }
    }
    if (error_stack_is_empty(error_stack)) {
      save_config_settings(config, error_stack);
    } else {
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
    sync_command_scan_loop(config, error_stack, initial_command_string);
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
