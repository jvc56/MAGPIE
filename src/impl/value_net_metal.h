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
// run at once (0 for 2; always 1 on GPUs other than Apple's), each with
// its own copy of the graph. net may be destroyed afterwards.
ValueNetMetal *value_net_metal_create(const ValueNet *net, bool half_precision,
                                      int concurrency, ErrorStack *error_stack);
void value_net_metal_destroy(ValueNetMetal *metal);

// Intel Macs can have two GPUs, an integrated Intel one and a discrete AMD
// one, and there the backend can run on either; Apple Silicon Macs have
// one GPU, so the choice exists only on Intel.
// (Keyed on the platform, not VALUE_NET_METAL, which test files built
// for release do not get.)
#if defined(__APPLE__) && defined(__x86_64__)
#define VALUE_NET_METAL_DEVICE_SELECTION

// From now on value_net_metal_create uses the GPU whose name contains name
// (ignoring case), or the system default when name is NULL. Returns false,
// with an error listing the GPUs, unless exactly one GPU matches (or, in a
// build without Metal, always unless name is NULL). Not thread-safe: call
// before creating backends.
bool value_net_metal_select_device(const char *name, ErrorStack *error_stack);
#endif

// How many evaluations run at once (see value_net_metal_create).
int value_net_metal_get_concurrency(const ValueNetMetal *metal);

// As value_net_evaluate_cpu; safe to call from several threads, which run
// up to the ValueNetMetal's concurrency at once and otherwise wait. Each
// call runs in chunks of at most VALUE_NET_MAX_GPU_ROWS rows.
void value_net_metal_evaluate(ValueNetMetal *metal, int rows,
                              const float *board, const float *scalars,
                              float *value, float *spread);

// As value_net_hidden_cpu (value_net.h), on the GPU.
void value_net_metal_hidden(ValueNetMetal *metal, int rows, const float *board,
                            const float *scalars, float *hidden);

#endif
