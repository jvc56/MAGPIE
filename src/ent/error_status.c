#include "error_status.h"

#include <stdbool.h>
#include <stdlib.h>

#include "../def/autoplay_defs.h"
#include "../def/config_defs.h"
#include "../def/error_status_defs.h"
#include "../def/game_defs.h"
#include "../def/gen_defs.h"
#include "../def/inference_defs.h"
#include "../def/kwg_defs.h"
#include "../def/leave_gen_defs.h"
#include "../def/simmer_defs.h"
#include "../def/validated_move_defs.h"

#include "../util/log.h"
#include "../util/util.h"

struct ErrorStatus {
  error_status_t type;
  int code;
};

error_status_t error_status_get_type(ErrorStatus *error_status) {
  return error_status->type;
}

int error_status_get_code(ErrorStatus *error_status) {
  return error_status->code;
}

void error_status_set_type_and_code(ErrorStatus *error_status,
                                    error_status_t type, int code) {
  error_status->code = code;
  error_status->type = type;
}

ErrorStatus *error_status_create(void) {
  ErrorStatus *error_status = malloc_or_die(sizeof(ErrorStatus));
  error_status_set_type_and_code(error_status, ERROR_STATUS_TYPE_NONE, 0);
  return error_status;
}

void error_status_destroy(ErrorStatus *error_status) {
  if (!error_status) {
    return;
  }
  free(error_status);
}

bool error_status_is_success(error_status_t error_status_type, int error_code) {
  bool is_success = false;
  switch (error_status_type) {
  case ERROR_STATUS_TYPE_NONE:
    is_success = true;
    break;
  case ERROR_STATUS_TYPE_MOVE_GEN:
    is_success = error_code == (int)GEN_STATUS_SUCCESS;
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
  case ERROR_STATUS_TYPE_CONVERT:
    is_success = error_code == (int)CONVERT_STATUS_SUCCESS;
    break;
  case ERROR_STATUS_TYPE_CONFIG_LOAD:
    is_success = error_code == (int)CONFIG_LOAD_STATUS_SUCCESS;
    break;
  case ERROR_STATUS_TYPE_CGP_LOAD:
    is_success = error_code == (int)CGP_PARSE_STATUS_SUCCESS;
    break;
  case ERROR_STATUS_TYPE_MOVE_VALIDATION:
    is_success = error_code == (int)MOVE_VALIDATION_STATUS_SUCCESS;
    break;
  case ERROR_STATUS_TYPE_LEAVE_GEN:
    is_success = error_code == (int)LEAVE_GEN_STATUS_SUCCESS;
    break;
  }
  return is_success;
}

void set_or_clear_error_status(ErrorStatus *error_status,
                               error_status_t error_status_type,
                               int error_code) {
  if (error_status_is_success(error_status_type, error_code)) {
    error_status_set_type_and_code(error_status, ERROR_STATUS_TYPE_NONE, 0);
  } else {
    error_status_set_type_and_code(error_status, error_status_type, error_code);
  }
}

bool error_status_get_success(const ErrorStatus *error_status) {
  return error_status_is_success(error_status->type, error_status->code);
}

void error_status_log_warn_if_failed(const ErrorStatus *error_status) {
  if (error_status->type == ERROR_STATUS_TYPE_NONE) {
    return;
  }
  const char *error_type_string = "";
  switch (error_status->type) {
  case ERROR_STATUS_TYPE_NONE:
    log_fatal("no error to warn");
    break;
  case ERROR_STATUS_TYPE_MOVE_GEN:
    error_type_string = "move generation";
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
  case ERROR_STATUS_TYPE_CONVERT:
    error_type_string = "convert";
    break;
  case ERROR_STATUS_TYPE_CONFIG_LOAD:
    error_type_string = "config load";
    break;
  case ERROR_STATUS_TYPE_CGP_LOAD:
    error_type_string = "cgp load";
    break;
  case ERROR_STATUS_TYPE_MOVE_VALIDATION:
    error_type_string = "move validation";
    break;
  case ERROR_STATUS_TYPE_LEAVE_GEN:
    error_type_string = "leave generation";
    break;
  }
  log_warn("error: %s finished with code %d", error_type_string,
           error_status->code);
}
