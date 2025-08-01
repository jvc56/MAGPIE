#include "cmd_api.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "config.h"
#include "exec.h"
#include <stdlib.h>

struct Magpie {
  Config *config;
  ErrorStack *error;
  char *output;
};

Magpie *magpie_create(const char *data_paths) {
  Magpie *ret = malloc_or_die(sizeof(Magpie));
  ret->error = error_stack_create();
  ret->config = config_create_default_with_data_paths(ret->error, data_paths);
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
  bool ret = run_str_api_command(mp->config, mp->error, command, &mp->output);
  if (error_stack_is_empty(mp->error)) {
    return MAGPIE_SUCCESS;
  }
  if (ret) {
    return MAGPIE_ERROR;
  }
  return MAGPIE_DID_NOT_RUN;
}

char *magpie_get_and_clear_error(Magpie *mp) {
  return error_stack_get_string_and_reset(mp->error);
}

char *magpie_get_last_command_status_message(Magpie *mp) {
  return config_get_execute_status(mp->config);
}

char *magpie_get_last_command_output(Magpie *mp) {
  return string_duplicate(mp->output);
}
