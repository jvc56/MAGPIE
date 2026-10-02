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
// weights and activations), else float32. net may be destroyed afterwards.
ValueNetMetal *value_net_metal_create(const ValueNet *net, bool half_precision,
                                      ErrorStack *error_stack);
void value_net_metal_destroy(ValueNetMetal *metal);

// As value_net_evaluate_cpu. Calls are serialized: one evaluation runs at a
// time per ValueNetMetal.
void value_net_metal_evaluate(ValueNetMetal *metal, int rows,
                              const float *board, const float *scalars,
                              float *value, float *spread);

#endif
