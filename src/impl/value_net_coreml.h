#ifndef VALUE_NET_COREML_H
#define VALUE_NET_COREML_H

#include "../util/io_util.h"

// The value net on the Neural Engine through CoreML: a CoreML build of the
// net from the same weights, with a fixed batch size (tools/value_net/
// build_ane.py writes one in the Neural Engine's preferred layout),
// evaluated in chunks of that batch with several chunks in flight at once,
// which the Neural Engine needs for its throughput.
// Built on macOS (VALUE_NET_METAL); elsewhere value_net_coreml_create
// reports it as unavailable.
typedef struct ValueNetCoreML ValueNetCoreML;

// Compiles and loads the .mlpackage at path for the CPU and Neural Engine;
// at most concurrency chunks run at once (0 for 6).
ValueNetCoreML *value_net_coreml_create(const char *path, int concurrency,
                                        ErrorStack *error_stack);
void value_net_coreml_destroy(ValueNetCoreML *coreml);

// As value_net_evaluate_cpu without spread; safe to call from several
// threads at once.
void value_net_coreml_evaluate(ValueNetCoreML *coreml, int rows,
                               const float *board, const float *scalars,
                               float *value);

#endif
