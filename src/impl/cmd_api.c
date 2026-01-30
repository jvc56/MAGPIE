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
  printf("[CMD_API] magpie_create entered with data_paths: %s\n", data_paths ? data_paths : "NULL"); fflush(stdout);
  Magpie *ret = malloc_or_die(sizeof(Magpie));
  printf("[CMD_API] malloc'd Magpie: %p\n", (void*)ret); fflush(stdout);
  ret->error = error_stack_create();
  printf("[CMD_API] error_stack_create returned: %p\n", (void*)ret->error); fflush(stdout);
  ConfigArgs args = {.data_paths = data_paths, .settings_filename = NULL};
  printf("[CMD_API] calling config_create\n"); fflush(stdout);
  ret->config = config_create(&args, ret->error);
  printf("[CMD_API] config_create returned: %p\n", (void*)ret->config); fflush(stdout);
  if (ret->config) {
    ThreadControl *tc = config_get_thread_control(ret->config);
    printf("[CMD_API] thread_control from new config: %p\n", (void*)tc); fflush(stdout);
  } else {
    printf("[CMD_API] config_create FAILED!\n"); fflush(stdout);
    // Check if there are errors
    if (!error_stack_is_empty(ret->error)) {
      char *err = error_stack_get_string_and_reset(ret->error);
      printf("[CMD_API] Error: %s\n", err ? err : "unknown"); fflush(stdout);
      free(err);
    }
  }
  ret->output = empty_string();
  printf("[CMD_API] magpie_create returning: %p\n", (void*)ret); fflush(stdout);
  return ret;
}

void magpie_destroy(Magpie *mp) {
  config_destroy(mp->config);
  error_stack_destroy(mp->error);
  free(mp->output);
  free(mp);
}

cmd_exit_code magpie_run_sync(Magpie *mp, const char *command) {
  printf("[CMD_API] magpie_run_sync entered\n"); fflush(stdout);
  printf("[CMD_API] command: %s\n", command); fflush(stdout);
  printf("[CMD_API] mp: %p\n", (void*)mp); fflush(stdout);
  printf("[CMD_API] mp->output: %p\n", (void*)mp->output); fflush(stdout);
  free(mp->output);
  printf("[CMD_API] freed mp->output\n"); fflush(stdout);
  mp->output = NULL;

  // Capture stdout to memory stream (same approach as test/command_test.c)
  char *buffer = NULL;
  size_t size = 0;
  printf("[CMD_API] About to call open_memstream\n"); fflush(stdout);
  FILE *mem_stream = open_memstream(&buffer, &size);
  printf("[CMD_API] open_memstream returned: %p\n", (void*)mem_stream); fflush(stdout);
  if (!mem_stream) {
    printf("[CMD_API] open_memstream failed!\n"); fflush(stdout);
    mp->output = empty_string();
    return MAGPIE_ERROR;
  }
  printf("[CMD_API] open_memstream succeeded\n"); fflush(stdout);

  // Redirect stdout and linenoise output
  printf("[CMD_API] calling io_set_stream_out\n"); fflush(stdout);
  io_set_stream_out(mem_stream);
  printf("[CMD_API] calling linenoise_set_stream_out\n"); fflush(stdout);
  linenoise_set_stream_out(mem_stream);
  printf("[CMD_API] stream redirects done\n"); fflush(stdout);

  // Execute command synchronously (same as shell does)
  printf("[CMD_API] mp->config: %p\n", (void*)mp->config); fflush(stdout);
  printf("[CMD_API] mp->error: %p\n", (void*)mp->error); fflush(stdout);
  if (mp->config) {
    ThreadControl *tc = config_get_thread_control(mp->config);
    printf("[CMD_API] config->thread_control: %p\n", (void*)tc); fflush(stdout);
  }
  printf("[CMD_API] calling execute_command_sync\n"); fflush(stdout);
  execute_command_sync(mp->config, mp->error, command);
  printf("[CMD_API] execute_command_sync returned\n"); fflush(stdout);

  // Restore stdout
  fflush(mem_stream);
  fclose(mem_stream);
  io_reset_stream_out();
  linenoise_set_stream_out(NULL);
  printf("[CMD_API] streams restored\n"); fflush(stdout);

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
