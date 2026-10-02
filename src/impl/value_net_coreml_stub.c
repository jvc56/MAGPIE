#include "../util/io_util.h"
#include "../util/string_util.h"
#include "value_net_coreml.h"

// Without Apple frameworks (VALUE_NET_METAL unset), the backend is never
// available; value_net_coreml.m provides it on macOS.
#ifndef VALUE_NET_METAL

ValueNetCoreML *value_net_coreml_create(const char *path, int concurrency,
                                        ErrorStack *error_stack) {
  (void)path;
  (void)concurrency;
  error_stack_push(
      error_stack, ERROR_STATUS_VALUE_NET_BACKEND_UNAVAILABLE,
      string_duplicate("this build has no CoreML value net backend"));
  return NULL;
}

void value_net_coreml_destroy(ValueNetCoreML *coreml) { (void)coreml; }

void value_net_coreml_evaluate(ValueNetCoreML *coreml, int rows,
                               const float *board, const float *scalars,
                               float *value, float *spread) {
  (void)coreml;
  (void)rows;
  (void)board;
  (void)scalars;
  (void)value;
  (void)spread;
  log_fatal("value_net_coreml_evaluate called without a CoreML backend");
}

#endif
