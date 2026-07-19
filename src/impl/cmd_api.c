#include "cmd_api.h"

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
};

Magpie *magpie_create(const char *data_paths) {
  Magpie *mp = malloc_or_die(sizeof(Magpie));
  mp->error = error_stack_create();
  const ConfigArgs args = {
      .data_paths = data_paths, .settings_filename = NULL, .use_wmp = false};
  mp->config = config_create(&args, mp->error);
  mp->output = empty_string();
  return mp;
}

void magpie_destroy(Magpie *mp) {
  if (!mp) {
    return;
  }
  config_destroy(mp->config);
  error_stack_destroy(mp->error);
  free(mp->output);
  free(mp);
}

static cmd_exit_code run_sync_with_output_format(Magpie *mp,
                                                 const char *command,
                                                 bool human_readable) {
  free(mp->output);
  mp->output = NULL;
  if (!mp->config) {
    mp->output = empty_string();
    error_stack_push(
        mp->error, ERROR_STATUS_CMD_API_UNINITIALIZED,
        string_duplicate("cannot run command: magpie creation failed"));
    return MAGPIE_DID_NOT_RUN;
  }
  config_set_human_readable(mp->config, human_readable);
  const bool command_ran =
      run_str_api_command(mp->config, mp->error, command, &mp->output);
  if (!mp->output) {
    mp->output = empty_string();
  }
  if (error_stack_is_empty(mp->error)) {
    return MAGPIE_SUCCESS;
  }
  if (command_ran) {
    return MAGPIE_ERROR;
  }
  return MAGPIE_DID_NOT_RUN;
}

cmd_exit_code magpie_run_sync(Magpie *mp, const char *command) {
  return run_sync_with_output_format(mp, command, false);
}

cmd_exit_code magpie_run_sync_human_readable(Magpie *mp, const char *command) {
  return run_sync_with_output_format(mp, command, true);
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
