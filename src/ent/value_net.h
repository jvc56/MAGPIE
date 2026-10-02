#ifndef VALUE_NET_H
#define VALUE_NET_H

#include "../util/io_util.h"

// The Macondo transformer value net (macondo-nn-tf): it scores the position
// after a candidate move, from the mover's side, as value = P(win) - P(loss)
// and spread = tanh(spread / 130). See value_net_features.h for its input
// row and value_net_defs.h for its shape.
//
// Weights come from a directory holding weights.f32 (every tensor as one
// little-endian float32 blob, row-major) and manifest.json (each tensor's
// name, shape and offset in floats), as Macondo's export-raw.py writes them.
typedef struct ValueNet ValueNet;

ValueNet *value_net_create(const char *dir, ErrorStack *error_stack);
void value_net_destroy(ValueNet *net);

// The float32 weights and the named tensor's offset into them, for
// backends that upload the weights themselves.
const float *value_net_get_weights(const ValueNet *net);
size_t value_net_get_num_weights(const ValueNet *net);

// Evaluates rows rows on the CPU in float32: board holds rows x
// VALUE_NET_BOARD_FLOATS floats (plane-major, then row, then column) and
// scalars rows x VALUE_NET_SCALARS. value and spread (either may be NULL)
// receive one float per row.
void value_net_evaluate_cpu(const ValueNet *net, int rows, const float *board,
                            const float *scalars, float *value, float *spread);

#endif
