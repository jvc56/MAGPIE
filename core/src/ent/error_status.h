#ifndef ERROR_STATUS_H
#define ERROR_STATUS_H

#include "../def/error_status_defs.h"

struct ErrorStatus;
typedef struct ErrorStatus ErrorStatus;

ErrorStatus *create_error_status();
void destroy_error_status(ErrorStatus *error_status);

void set_error_status(ErrorStatus *error_status, error_status_t type, int code);
void log_warn_if_failed(const ErrorStatus *error_status);
bool is_successful_error_code(error_status_t error_status_type, int error_code);
#endif
