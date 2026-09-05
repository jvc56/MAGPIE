#include "cmd_api.h"

#include "../compat/cpthread.h"
#include "../def/cpthread_defs.h"
#include "../def/thread_control_defs.h"
#include "../ent/thread_control.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "config.h"
#include "exec.h"
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>

static_assert(MAGPIE_THREAD_STATUS_UNINITIALIZED ==
                  (int)THREAD_CONTROL_STATUS_UNINITIALIZED,
              "magpie_thread_status must mirror thread_control_status_t");
static_assert(MAGPIE_THREAD_STATUS_STARTED ==
                  (int)THREAD_CONTROL_STATUS_STARTED,
              "magpie_thread_status must mirror thread_control_status_t");
static_assert(MAGPIE_THREAD_STATUS_USER_INTERRUPT ==
                  (int)THREAD_CONTROL_STATUS_USER_INTERRUPT,
              "magpie_thread_status must mirror thread_control_status_t");
static_assert(MAGPIE_THREAD_STATUS_FINISHED ==
                  (int)THREAD_CONTROL_STATUS_FINISHED,
              "magpie_thread_status must mirror thread_control_status_t");

struct Magpie {
  Config *config;
  ErrorStack *error;
  char *output;
  cpthread_t async_thread;
  bool async_thread_joinable;
  cmd_exit_code last_exit_code;
};

Magpie *magpie_create_with_options(const char *data_paths,
                                   const char *settings_filename,
                                   bool use_wmp) {
  Magpie *mp = malloc_or_die(sizeof(Magpie));
  mp->error = error_stack_create();
  const ConfigArgs args = {.data_paths = data_paths,
                           .settings_filename = settings_filename,
                           .use_wmp = use_wmp};
  mp->config = config_create(&args, mp->error);
  mp->output = empty_string();
  mp->async_thread_joinable = false;
  mp->last_exit_code = MAGPIE_SUCCESS;
  return mp;
}

Magpie *magpie_create(const char *data_paths) {
  return magpie_create_with_options(data_paths, NULL, false);
}

// Joins a finished async worker thread if one exists. Returns true if a
// worker is still running, in which case no other command may start and
// the caller must not touch the error stack or output.
static bool async_command_is_active(Magpie *mp) {
  if (!mp->async_thread_joinable) {
    return false;
  }
  const thread_control_status_t status =
      thread_control_get_status(config_get_thread_control(mp->config));
  if (status == THREAD_CONTROL_STATUS_FINISHED) {
    cpthread_join(mp->async_thread);
    mp->async_thread_joinable = false;
    return false;
  }
  return true;
}

void magpie_destroy(Magpie *mp) {
  if (!mp) {
    return;
  }
  if (mp->async_thread_joinable) {
    magpie_stop_current_command(mp);
    cpthread_join(mp->async_thread);
    mp->async_thread_joinable = false;
  }
  config_destroy(mp->config);
  error_stack_destroy(mp->error);
  free(mp->output);
  free(mp);
}

static cmd_exit_code exit_code_from_error_stack(const Magpie *mp,
                                                bool command_ran) {
  if (error_stack_is_empty(mp->error)) {
    return MAGPIE_SUCCESS;
  }
  if (command_ran) {
    return MAGPIE_ERROR;
  }
  return MAGPIE_DID_NOT_RUN;
}

static cmd_exit_code run_sync_with_output_format(Magpie *mp,
                                                 const char *command,
                                                 bool human_readable) {
  if (async_command_is_active(mp)) {
    return MAGPIE_DID_NOT_RUN;
  }
  free(mp->output);
  mp->output = NULL;
  if (!mp->config) {
    mp->output = empty_string();
    error_stack_push(
        mp->error, ERROR_STATUS_CMD_API_UNINITIALIZED,
        string_duplicate("cannot run command: magpie creation failed"));
    mp->last_exit_code = MAGPIE_DID_NOT_RUN;
    return mp->last_exit_code;
  }
  config_set_human_readable(mp->config, human_readable);
  const bool command_ran =
      run_str_api_command(mp->config, mp->error, command, &mp->output);
  if (!mp->output) {
    mp->output = empty_string();
  }
  mp->last_exit_code = exit_code_from_error_stack(mp, command_ran);
  return mp->last_exit_code;
}

cmd_exit_code magpie_run_sync(Magpie *mp, const char *command) {
  return run_sync_with_output_format(mp, command, false);
}

cmd_exit_code magpie_run_sync_human_readable(Magpie *mp, const char *command) {
  return run_sync_with_output_format(mp, command, true);
}

static void *async_command_worker(void *arg) {
  Magpie *mp = arg;
  const bool command_ran =
      config_run_str_api_command(mp->config, mp->error, &mp->output);
  if (!mp->output) {
    mp->output = empty_string();
  }
  mp->last_exit_code = exit_code_from_error_stack(mp, command_ran);
  return NULL;
}

static cmd_exit_code run_async_with_output_format(Magpie *mp,
                                                  const char *command,
                                                  bool human_readable) {
  if (async_command_is_active(mp)) {
    return MAGPIE_DID_NOT_RUN;
  }
  free(mp->output);
  mp->output = NULL;
  if (!mp->config) {
    mp->output = empty_string();
    error_stack_push(
        mp->error, ERROR_STATUS_CMD_API_UNINITIALIZED,
        string_duplicate("cannot run command: magpie creation failed"));
    mp->last_exit_code = MAGPIE_DID_NOT_RUN;
    return mp->last_exit_code;
  }
  config_set_human_readable(mp->config, human_readable);
  // Load synchronously so parse and data-loading errors are reported to
  // the caller immediately; only execution happens on the worker thread.
  // The command string is fully consumed by loading, so the worker does
  // not need a copy of it.
  if (!load_command_sync(mp->config, mp->error, command)) {
    mp->output = empty_string();
    mp->last_exit_code = MAGPIE_DID_NOT_RUN;
    return mp->last_exit_code;
  }
  cpthread_create(&mp->async_thread, async_command_worker, mp);
  mp->async_thread_joinable = true;
  return MAGPIE_SUCCESS;
}

cmd_exit_code magpie_run_async(Magpie *mp, const char *command) {
  return run_async_with_output_format(mp, command, false);
}

cmd_exit_code magpie_run_async_human_readable(Magpie *mp, const char *command) {
  return run_async_with_output_format(mp, command, true);
}

cmd_exit_code magpie_await(Magpie *mp) {
  if (mp->async_thread_joinable) {
    cpthread_join(mp->async_thread);
    mp->async_thread_joinable = false;
  }
  return mp->last_exit_code;
}

bool magpie_has_error(const Magpie *mp) {
  return !error_stack_is_empty(mp->error);
}

char *magpie_get_and_clear_error(Magpie *mp) {
  char *error_string = error_stack_get_string_and_reset(mp->error);
  if (!error_string) {
    return empty_string();
  }
  return error_string;
}

char *magpie_get_last_command_status_message(Magpie *mp) {
  if (!mp->config) {
    return empty_string();
  }
  char *status = config_get_execute_status(mp->config);
  if (!status) {
    return empty_string();
  }
  return status;
}

char *magpie_get_last_command_output(const Magpie *mp) {
  return string_duplicate(mp->output);
}

// Semantically a mutation even though the compiler can't see one through
// the const getter for ThreadControl.
// cppcheck-suppress constParameterPointer
void magpie_stop_current_command(Magpie *mp) {
  if (!mp->config) {
    return;
  }
  ThreadControl *tc = config_get_thread_control(mp->config);
  thread_control_set_status(tc, THREAD_CONTROL_STATUS_USER_INTERRUPT);
}

magpie_thread_status magpie_get_thread_status(const Magpie *mp) {
  if (!mp->config) {
    return MAGPIE_THREAD_STATUS_UNINITIALIZED;
  }
  ThreadControl *tc = config_get_thread_control(mp->config);
  return (magpie_thread_status)thread_control_get_status(tc);
}

void magpie_free_string(char *str) { free(str); }
