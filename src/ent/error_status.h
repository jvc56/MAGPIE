#ifndef ERROR_STATUS_H
#define ERROR_STATUS_H

#include <stdbool.h>

#include "../def/error_status_defs.h"

typedef struct ErrorStatus ErrorStatus;

ErrorStatus *error_status_create();

error_status_t error_status_get_type(ErrorStatus *error_status);
int error_status_get_code(ErrorStatus *error_status);
void error_status_destroy(ErrorStatus *error_status);
void set_or_clear_error_status(ErrorStatus *error_status,
                               error_status_t error_status_type,
                               int error_code);
void error_status_set_type_and_code(ErrorStatus *error_status,
                                    error_status_t type, int code);
void error_status_log_warn_if_failed(const ErrorStatus *error_status);
bool error_status_get_success(const ErrorStatus *error_status);

#endif
