#include "../ent/value_net.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include "value_net_metal.h"
#include <stdbool.h>

// Without Metal (VALUE_NET_METAL unset), the backend is never available;
// value_net_metal.m provides it on macOS.
#ifndef VALUE_NET_METAL

ValueNetMetal *value_net_metal_create(const ValueNet *net, bool half_precision,
                                      int concurrency,
                                      ErrorStack *error_stack) {
  (void)net;
  (void)half_precision;
  (void)concurrency;
  error_stack_push(
      error_stack, ERROR_STATUS_VALUE_NET_BACKEND_UNAVAILABLE,
      string_duplicate("this build has no Metal value net backend"));
  return NULL;
}

void value_net_metal_destroy(ValueNetMetal *metal) { (void)metal; }

int value_net_metal_get_concurrency(const ValueNetMetal *metal) {
  (void)metal;
  return 0;
}

void value_net_metal_evaluate(ValueNetMetal *metal, int rows,
                              const float *board, const float *scalars,
                              float *value, float *spread) {
  (void)metal;
  (void)rows;
  (void)board;
  (void)scalars;
  (void)value;
  (void)spread;
  log_fatal("value_net_metal_evaluate called without a Metal backend");
}

void value_net_metal_hidden(ValueNetMetal *metal, int rows, const float *board,
                            const float *scalars, float *hidden) {
  (void)metal;
  (void)rows;
  (void)board;
  (void)scalars;
  (void)hidden;
  log_fatal("value_net_metal_hidden called without a Metal backend");
}

#endif
