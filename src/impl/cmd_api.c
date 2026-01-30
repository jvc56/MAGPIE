#include "cmd_api.h"

#include "../compat/linenoise.h"
#include "../ent/thread_control.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "config.h"
#include "exec.h"
#include <stdio.h>
#include <stdlib.h>

struct Magpie {
  Config *config;
  ErrorStack *error;
  char *output;
};

Magpie *magpie_create(const char *data_paths) {
  Magpie *ret = malloc_or_die(sizeof(Magpie));
  ret->error = error_stack_create();
  ConfigArgs args = {.data_paths = data_paths, .settings_filename = NULL};
  ret->config = config_create(&args, ret->error);
  ret->output = empty_string();
  return ret;
}

void magpie_destroy(Magpie *mp) {
  config_destroy(mp->config);
  error_stack_destroy(mp->error);
  free(mp->output);
  free(mp);
}

cmd_exit_code magpie_run_sync(Magpie *mp, const char *command) {
  free(mp->output);
  mp->output = NULL;

  // Capture stdout to memory stream (same approach as test/command_test.c)
  char *buffer = NULL;
  size_t size = 0;
  FILE *mem_stream = open_memstream(&buffer, &size);
  if (!mem_stream) {
    mp->output = empty_string();
    return MAGPIE_ERROR;
  }

  // Redirect stdout and linenoise output
  io_set_stream_out(mem_stream);
  linenoise_set_stream_out(mem_stream);

  // Execute command synchronously (same as shell does)
  execute_command_sync(mp->config, mp->error, command);

  // Restore stdout
  fflush(mem_stream);
  fclose(mem_stream);
  io_reset_stream_out();
  linenoise_set_stream_out(NULL);

  // Store captured output
  if (buffer) {
    mp->output = buffer;
  } else {
    mp->output = empty_string();
  }

  if (error_stack_is_empty(mp->error)) {
    return MAGPIE_SUCCESS;
  }
  return MAGPIE_ERROR;
}

char *magpie_get_and_clear_error(Magpie *mp) {
  return error_stack_get_string_and_reset(mp->error);
}

char *magpie_get_last_command_status_message(Magpie *mp) {
  return config_get_execute_status(mp->config);
}

char *magpie_get_last_command_output(const Magpie *mp) {
  return string_duplicate(mp->output);
}

void magpie_stop_current_command(Magpie *mp) {
  ThreadControl *tc = config_get_thread_control(mp->config);
  thread_control_set_status(tc, THREAD_CONTROL_STATUS_USER_INTERRUPT);
}

Config *magpie_get_config(Magpie *mp) { return mp->config; }
