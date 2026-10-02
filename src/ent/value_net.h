#ifndef VALUE_NET_H
#define VALUE_NET_H

#include "../util/io_util.h"
#include <stddef.h>

// The Macondo transformer value net (macondo-nn-tf): it scores the position
// after a candidate move, from the mover's side, as value = P(win) - P(loss)
// and spread = tanh(spread / 130). See value_net_features.h for its input
// row and value_net_defs.h for its tokens.
//
// Weights come from a directory holding weights.f32 (every tensor as one
// little-endian float32 blob, row-major) and manifest.json (each tensor's
// name, shape and offset in floats), as Macondo's export-raw.py writes them.
typedef struct ValueNet ValueNet;

// The net's size, from manifest.json's hparams: d_model, layers, heads,
// ff_mult and optionally hidden (default 128). Macondo's
// macondo-nn-tf-nwl23s is 192 wide with 8 layers of 6 heads.
typedef struct ValueNetShape {
  int model_dim;
  int layers;
  int heads;
  int head_dim;
  int ff_dim;
  int head_hidden;
} ValueNetShape;

ValueNet *value_net_create(const char *dir, ErrorStack *error_stack);
void value_net_destroy(ValueNet *net);

const ValueNetShape *value_net_get_shape(const ValueNet *net);

// The named tensor (manifest name, row-major, Linear weights stored
// [out, in]), for backends that upload the weights themselves, or NULL
// when it is missing or does not hold count floats.
const float *value_net_get_tensor(const ValueNet *net, const char *name,
                                  size_t count);

// Evaluates rows rows on the CPU in float32: board holds rows x
// VALUE_NET_BOARD_FLOATS floats (plane-major, then row, then column) and
// scalars rows x VALUE_NET_SCALARS. value and spread (either may be NULL)
// receive one float per row.
void value_net_evaluate_cpu(const ValueNet *net, int rows, const float *board,
                            const float *scalars, float *value, float *spread);

#endif
