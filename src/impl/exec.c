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

#include "../ent/error_status.h"
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

void execute_command_and_set_mode_stopped(Config *config) {
  config_execute_command(config);
  error_status_log_warn_if_failed(config_get_error_status(config));
  thread_control_set_mode_stopped(config_get_thread_control(config));
}

void *execute_command_thread_worker(void *uncasted_config) {
  Config *config = (Config *)uncasted_config;
  execute_command_and_set_mode_stopped(config);
  return NULL;
}

void execute_command_sync_or_async(Config *config, const char *command,
                                   bool sync) {
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
  config_load_status_t config_load_status =
      config_load_command(config, command);
  set_or_clear_error_status(config_get_error_status(config),
                            ERROR_STATUS_TYPE_CONFIG_LOAD,
                            (int)config_load_status);
  if (config_load_status != CONFIG_LOAD_STATUS_SUCCESS) {
    error_status_log_warn_if_failed(config_get_error_status(config));
    thread_control_set_mode_stopped(thread_control);
    return;
  }

  if (sync) {
    execute_command_and_set_mode_stopped(config);
  } else {
    pthread_t cmd_execution_thread;
    pthread_create(&cmd_execution_thread, NULL, execute_command_thread_worker,
                   config);
    pthread_detach(cmd_execution_thread);
  }
}

void execute_command_sync(Config *config, const char *command) {
  execute_command_sync_or_async(config, command, true);
}

void execute_command_async(Config *config, const char *command) {
  execute_command_sync_or_async(config, command, false);
}

void process_ucgi_command(Config *config, const char *command) {
  // Assume cmd is already trimmed of whitespace
  ThreadControl *thread_control = config_get_thread_control(config);
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
    execute_command_async(config, command);
  }
}

void command_scan_loop(Config *config, const char *initial_command_string) {
  execute_command_sync(config, initial_command_string);
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

    if (strings_equal(input, QUIT_COMMAND_STRING)) {
      break;
    }

    if (is_string_empty_or_null(input)) {
      continue;
    }

    switch (exec_mode) {
    case EXEC_MODE_CONSOLE:
      execute_command_sync(config, input);
      break;
    case EXEC_MODE_UCGI:
      process_ucgi_command(config, input);
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

void process_command(int argc, char *argv[]) {
  log_set_level(LOG_WARN);
  Config *config = config_create_default();
  char *initial_command_string = create_command_from_args(argc, argv);
  command_scan_loop(config, initial_command_string);
  free(initial_command_string);
  config_destroy(config);
  caches_destroy();
}