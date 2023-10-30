#include <stdlib.h>

#include "error_status.h"
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

char *error_status_to_string(ErrorStatus *error_status) {
  const char *error_type_string;
  switch (error_status->type) {
  case ERROR_STATUS_TYPE_NONE:
    error_type_string = "unknown command";
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
  return get_formatted_string("error: %s finished with code %d\n",
                              error_type_string, error_status->code);
}
