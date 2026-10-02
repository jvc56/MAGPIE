#ifndef VALUE_NET_DEFS_H
#define VALUE_NET_DEFS_H

// The shape of the Macondo transformer value net (macondo-nn-tf, see
// value_net.h): its input row and its layers.
enum {
  VALUE_NET_BOARD_DIM = 15,
  VALUE_NET_SQUARES = VALUE_NET_BOARD_DIM * VALUE_NET_BOARD_DIM,
  VALUE_NET_PLANES = 85,
  VALUE_NET_BOARD_FLOATS = VALUE_NET_PLANES * VALUE_NET_SQUARES,
  VALUE_NET_SCALARS = 72,
  // Tile tokens: one per tile type, blank first, then A..Z.
  VALUE_NET_TILE_TYPES = 27,
  // cls, one per square, one per tile type, one game token.
  VALUE_NET_TOKENS = 1 + VALUE_NET_SQUARES + VALUE_NET_TILE_TYPES + 1,
  VALUE_NET_MODEL_DIM = 192,
  VALUE_NET_LAYERS = 8,
  VALUE_NET_HEADS = 6,
  VALUE_NET_HEAD_DIM = VALUE_NET_MODEL_DIM / VALUE_NET_HEADS,
  VALUE_NET_FF_DIM = 4 * VALUE_NET_MODEL_DIM,
  VALUE_NET_HEAD_HIDDEN = 128,
  // softmax(heads.wdl) is [loss, draw, win].
  VALUE_NET_WDL = 3,
  // Most rows per GPU call. Each row needs about 4 MB of activations
  // (attention scores alone are heads x tokens x tokens floats), so 256
  // rows take about 1 GB; larger requests run in chunks. Unbounded batches
  // of 1,024+ rows exhausted a 16 GB machine.
  VALUE_NET_MAX_GPU_ROWS = 256,
};

#endif
