#ifndef ERROR_STATUS_H
#define ERROR_STATUS_H

typedef enum {
  ERROR_STATUS_TYPE_NONE,
  ERROR_STATUS_TYPE_CONFIG_LOAD,
  ERROR_STATUS_TYPE_CGP_LOAD,
  ERROR_STATUS_TYPE_SIM,
  ERROR_STATUS_TYPE_INFER,
  ERROR_STATUS_TYPE_AUTOPLAY,
} error_status_t;

typedef struct ErrorStatus {
  error_status_t type;
  int code;
} ErrorStatus;

void set_error_status(ErrorStatus *error_status, error_status_t type, int code);
ErrorStatus *create_error_status(error_status_t type, int code);
void destroy_error_status(ErrorStatus *error_status);
void log_warn_if_failed(const ErrorStatus *error_status);

#endif
