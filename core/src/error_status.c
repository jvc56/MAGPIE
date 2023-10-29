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
  return get_formatted_string("Error Status: %d\nError Type:   %d\n",
                              error_status->type, error_status->code);
}
