#ifndef VALUE_NET_DEFS_H
#define VALUE_NET_DEFS_H

// The Macondo transformer value net's input row and tokens (macondo-nn-tf,
// see value_net.h). Its width and depth come from each net's manifest
// (ValueNetShape).
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
  // The head's hidden width when the manifest does not give one.
  VALUE_NET_DEFAULT_HEAD_HIDDEN = 128,
  // Bounds on a manifest's sizes.
  VALUE_NET_MAX_MODEL_DIM = 4096,
  VALUE_NET_MAX_LAYERS = 64,
  VALUE_NET_MAX_FF_MULT = 16,
  // softmax(heads.wdl) is [loss, draw, win].
  VALUE_NET_WDL = 3,
  // Most rows per GPU call. Each teacher row (192 wide, 8 layers) needs
  // about 4 MB of activations, so 256 rows take about 1 GB; larger requests
  // run in chunks. Unbounded batches of 1,024+ rows exhausted a 16 GB
  // machine.
  VALUE_NET_MAX_GPU_ROWS = 256,
};

#endif
