#include <stdlib.h>

#include "error_status.h"
#include "string_util.h"
#include "util.h"

void set_error_status(ErrorStatus *error_status, error_status_t type,
                      int code) {
  error_status->type = type;
  error_status->code = code;
}

ErrorStatus *create_error_status(error_status_t type, int code) {
  ErrorStatus *error_status = malloc_or_die(sizeof(ErrorStatus));
  set_error_status(error_status, type, code);
  return error_status;
}

void destroy_error_status(ErrorStatus *error_status) { free(error_status); }
