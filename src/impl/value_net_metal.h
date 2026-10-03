#ifndef VALUE_NET_METAL_H
#define VALUE_NET_METAL_H

#include "../ent/value_net.h"
#include "../util/io_util.h"
#include <stdbool.h>

// The value net (value_net.h) on the GPU through Metal Performance Shaders
// Graph. Built on macOS (VALUE_NET_METAL); elsewhere value_net_metal_create
// reports it as unavailable.
typedef struct ValueNetMetal ValueNetMetal;

// Uploads net's weights; half_precision computes in float16 (inputs,
// weights and activations), else float32. At most concurrency evaluations
// run at once (0 for 2), each with its own copy of the graph. net may be
// destroyed afterwards.
ValueNetMetal *value_net_metal_create(const ValueNet *net, bool half_precision,
                                      int concurrency, ErrorStack *error_stack);
void value_net_metal_destroy(ValueNetMetal *metal);

// How many evaluations run at once (see value_net_metal_create).
int value_net_metal_get_concurrency(const ValueNetMetal *metal);

// As value_net_evaluate_cpu; safe to call from several threads, which run
// up to the ValueNetMetal's concurrency at once and otherwise wait. Each
// call runs in chunks of at most VALUE_NET_MAX_GPU_ROWS rows.
void value_net_metal_evaluate(ValueNetMetal *metal, int rows,
                              const float *board, const float *scalars,
                              float *value, float *spread);

#endif
