#include <stdbool.h>
#include <stdlib.h>

#include "../def/autoplay_defs.h"
#include "../def/config_defs.h"
#include "../def/error_status_defs.h"
#include "../def/game_defs.h"
#include "../def/inference_defs.h"
#include "../def/simmer_defs.h"

#include "../util/log.h"
#include "../util/util.h"

#include "error_status.h"

struct ErrorStatus {
  error_status_t type;
  int code;
};

void set_error_status(ErrorStatus *error_status, error_status_t type,
                      int code) {
  error_status->code = code;
  error_status->type = type;
}

ErrorStatus *create_error_status() {
  ErrorStatus *error_status = malloc_or_die(sizeof(ErrorStatus));
  set_error_status(error_status, ERROR_STATUS_TYPE_NONE, 0);
  return error_status;
}

void destroy_error_status(ErrorStatus *error_status) { free(error_status); }

bool is_successful_error_code(error_status_t error_status_type,
                              int error_code) {
  bool is_success = false;
  switch (error_status_type) {
  case ERROR_STATUS_TYPE_NONE:
    is_success = true;
    break;
  case ERROR_STATUS_TYPE_SIM:
    is_success = error_code == (int)SIM_STATUS_SUCCESS;
    break;
  case ERROR_STATUS_TYPE_INFER:
    is_success = error_code == (int)INFERENCE_STATUS_SUCCESS;
    break;
  case ERROR_STATUS_TYPE_AUTOPLAY:
    is_success = error_code == (int)AUTOPLAY_STATUS_SUCCESS;
    break;
  case ERROR_STATUS_TYPE_CONFIG_LOAD:
    is_success = error_code == (int)CONFIG_LOAD_STATUS_SUCCESS;
    break;
  case ERROR_STATUS_TYPE_CGP_LOAD:
    is_success = error_code == (int)CGP_PARSE_STATUS_SUCCESS;
    break;
  }
  return is_success;
}

void log_warn_if_failed(const ErrorStatus *error_status) {
  if (error_status->type == ERROR_STATUS_TYPE_NONE) {
    return;
  }
  const char *error_type_string = "";
  switch (error_status->type) {
  case ERROR_STATUS_TYPE_NONE:
    log_fatal("no error to warn");
    break;
  case ERROR_STATUS_TYPE_SIM:
    error_type_string = "sim";
    break;
  case ERROR_STATUS_TYPE_INFER:
    error_type_string = "inference";
    break;
  case ERROR_STATUS_TYPE_AUTOPLAY:
    error_type_string = "autoplay";
    break;
  case ERROR_STATUS_TYPE_CONFIG_LOAD:
    error_type_string = "config load";
    break;
  case ERROR_STATUS_TYPE_CGP_LOAD:
    error_type_string = "cgp load";
    break;
  }
  log_warn("error: %s finished with code %d", error_type_string,
           error_status->code);
}
