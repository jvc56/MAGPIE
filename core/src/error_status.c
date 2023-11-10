#include <stdlib.h>

#include "error_status.h"
#include "log.h"
#include "string_util.h"
#include "util.h"

void set_error_status(ErrorStatus *error_status, error_status_t type,
                      int code) {
  error_status->code = code;
  error_status->type = type;
}

ErrorStatus *create_error_status(error_status_t type, int code) {
  ErrorStatus *error_status = malloc_or_die(sizeof(ErrorStatus));
  set_error_status(error_status, type, code);
  return error_status;
}

void destroy_error_status(ErrorStatus *error_status) { free(error_status); }

void log_warn_if_failed(ErrorStatus *error_status) {
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
