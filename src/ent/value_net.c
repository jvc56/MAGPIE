#include "value_net.h"

#include "../compat/endian_io.h"
#include "../def/value_net_defs.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  // Partial sums per dot product, so the compiler can vectorize it.
  VALUE_NET_DOT_LANES = 8,
};

#define VALUE_NET_LAYER_NORM_EPS 1e-5F
#define VALUE_NET_INV_SQRT2 0.70710678118654752440F

typedef struct ValueNetBlock {
  const float *ln1_weight;
  const float *ln1_bias;
  const float *qkv_weight;
  const float *qkv_bias;
  const float *proj_weight;
  const float *proj_bias;
  const float *ln2_weight;
  const float *ln2_bias;
  const float *fc1_weight;
  const float *fc1_bias;
  const float *fc2_weight;
  const float *fc2_bias;
} ValueNetBlock;

struct ValueNet {
  float *weights;
  size_t num_weights;
  char *manifest;
  ValueNetShape shape;
  const float *pos_emb;
  const float *tile_emb;
  const float *cls;
  const float *square_proj_weight;
  const float *square_proj_bias;
  const float *tile_proj_weight;
  const float *tile_proj_bias;
  const float *game_proj_weight;
  const float *game_proj_bias;
  ValueNetBlock *blocks;
  const float *ln_f_weight;
  const float *ln_f_bias;
  const float *fc1_weight;
  const float *fc1_bias;
  const float *spread_weight;
  const float *spread_bias;
  const float *wdl_weight;
  const float *wdl_bias;
};

// Finds "name": "<name>" in the manifest and reads the offset_floats and
// count fields that follow it. Returns false when it is missing.
static bool manifest_find(const char *manifest, const char *name,
                          size_t *offset, size_t *count) {
  char *key = get_formatted_string("\"name\": \"%s\"", name);
  const char *entry = strstr(manifest, key);
  free(key);
  if (entry == NULL) {
    return false;
  }
  const char *offset_key = strstr(entry, "\"offset_floats\":");
  const char *count_key = strstr(entry, "\"count\":");
  if (offset_key == NULL || count_key == NULL) {
    return false;
  }
  char *end = NULL;
  const unsigned long long parsed_offset =
      strtoull(offset_key + strlen("\"offset_floats\":"), &end, 10);
  if (end == offset_key + strlen("\"offset_floats\":")) {
    return false;
  }
  const unsigned long long parsed_count =
      strtoull(count_key + strlen("\"count\":"), &end, 10);
  if (end == count_key + strlen("\"count\":")) {
    return false;
  }
  *offset = (size_t)parsed_offset;
  *count = (size_t)parsed_count;
  return true;
}

// Reads "key": <integer> from the manifest's hparams object. Returns false
// when it is missing.
static bool manifest_hparam(const char *manifest, const char *key,
                            int *value) {
  const char *hparams = strstr(manifest, "\"hparams\"");
  if (hparams == NULL) {
    return false;
  }
  const char *close = strchr(hparams, '}');
  char *quoted = get_formatted_string("\"%s\":", key);
  const char *entry = strstr(hparams, quoted);
  const size_t quoted_length = strlen(quoted);
  free(quoted);
  if (entry == NULL || (close != NULL && entry > close)) {
    return false;
  }
  char *end = NULL;
  const long parsed = strtol(entry + quoted_length, &end, 10);
  if (end == entry + quoted_length) {
    return false;
  }
  *value = (int)parsed;
  return true;
}

// Fills net->shape from the manifest's hparams.
static void value_net_read_shape(ValueNet *net, const char *manifest,
                                 ErrorStack *error_stack) {
  ValueNetShape *shape = &net->shape;
  int ff_mult = 0;
  if (!manifest_hparam(manifest, "d_model", &shape->model_dim) ||
      !manifest_hparam(manifest, "layers", &shape->layers) ||
      !manifest_hparam(manifest, "heads", &shape->heads) ||
      !manifest_hparam(manifest, "ff_mult", &ff_mult)) {
    error_stack_push(error_stack, ERROR_STATUS_VALUE_NET_MALFORMED_MANIFEST,
                     string_duplicate("value net manifest needs hparams "
                                      "d_model, layers, heads and ff_mult"));
    return;
  }
  if (!manifest_hparam(manifest, "hidden", &shape->head_hidden)) {
    shape->head_hidden = VALUE_NET_DEFAULT_HEAD_HIDDEN;
  }
  if (shape->model_dim < 1 || shape->model_dim > VALUE_NET_MAX_MODEL_DIM ||
      shape->layers < 1 || shape->layers > VALUE_NET_MAX_LAYERS ||
      shape->heads < 1 || shape->model_dim % shape->heads != 0 ||
      ff_mult < 1 || ff_mult > VALUE_NET_MAX_FF_MULT ||
      shape->head_hidden < 1 ||
      shape->head_hidden > VALUE_NET_MAX_MODEL_DIM) {
    error_stack_push(
        error_stack, ERROR_STATUS_VALUE_NET_MALFORMED_MANIFEST,
        get_formatted_string("value net hparams out of range: d_model %d, "
                             "layers %d, heads %d, ff_mult %d, hidden %d",
                             shape->model_dim, shape->layers, shape->heads,
                             ff_mult, shape->head_hidden));
    return;
  }
  shape->head_dim = shape->model_dim / shape->heads;
  shape->ff_dim = ff_mult * shape->model_dim;
}

// Points *tensor at the named tensor, which must hold expected floats.
static void value_net_bind(ValueNet *net, const char *manifest,
                           const char *name, size_t expected,
                           const float **tensor, ErrorStack *error_stack) {
  if (!error_stack_is_empty(error_stack)) {
    return;
  }
  size_t offset = 0;
  size_t count = 0;
  if (!manifest_find(manifest, name, &offset, &count)) {
    error_stack_push(
        error_stack, ERROR_STATUS_VALUE_NET_MALFORMED_MANIFEST,
        get_formatted_string("value net manifest has no tensor %s", name));
    return;
  }
  if (count != expected || offset + count > net->num_weights) {
    error_stack_push(
        error_stack, ERROR_STATUS_VALUE_NET_UNEXPECTED_TENSOR,
        get_formatted_string("value net tensor %s has %zu floats at offset "
                             "%zu; expected %zu within %zu",
                             name, count, offset, expected, net->num_weights));
    return;
  }
  *tensor = net->weights + offset;
}

static void value_net_bind_block(ValueNet *net, const char *manifest, int layer,
                                 ErrorStack *error_stack) {
  ValueNetBlock *block = &net->blocks[layer];
  const size_t dim = (size_t)net->shape.model_dim;
  const size_t ff_dim = (size_t)net->shape.ff_dim;
  const struct {
    const char *suffix;
    size_t count;
    const float **tensor;
  } tensors[] = {
      {"ln1.weight", dim, &block->ln1_weight},
      {"ln1.bias", dim, &block->ln1_bias},
      {"qkv.weight", 3 * dim * dim, &block->qkv_weight},
      {"qkv.bias", 3 * dim, &block->qkv_bias},
      {"proj.weight", dim * dim, &block->proj_weight},
      {"proj.bias", dim, &block->proj_bias},
      {"ln2.weight", dim, &block->ln2_weight},
      {"ln2.bias", dim, &block->ln2_bias},
      {"fc1.weight", ff_dim * dim, &block->fc1_weight},
      {"fc1.bias", ff_dim, &block->fc1_bias},
      {"fc2.weight", dim * ff_dim, &block->fc2_weight},
      {"fc2.bias", dim, &block->fc2_bias},
  };
  for (size_t tensor_idx = 0; tensor_idx < sizeof(tensors) / sizeof(*tensors);
       tensor_idx++) {
    char *name =
        get_formatted_string("blocks.%d.%s", layer, tensors[tensor_idx].suffix);
    value_net_bind(net, manifest, name, tensors[tensor_idx].count,
                   tensors[tensor_idx].tensor, error_stack);
    free(name);
  }
}

// Reads the whole weight blob as little-endian floats.
static float *value_net_read_weights(const char *path, size_t *num_weights,
                                     ErrorStack *error_stack) {
  FILE *stream = fopen_safe(path, "rb", error_stack);
  if (stream == NULL) {
    return NULL;
  }
  long size = -1;
  if (fseek(stream, 0, SEEK_END) == 0) {
    size = ftell(stream);
  }
  if (size <= 0 || size % (long)sizeof(uint32_t) != 0 ||
      fseek(stream, 0, SEEK_SET) != 0) {
    (void)fclose(stream);
    error_stack_push(error_stack, ERROR_STATUS_VALUE_NET_MISSING_FILE,
                     get_formatted_string("value net weights %s are not a "
                                          "whole number of floats",
                                          path));
    return NULL;
  }
  const size_t count = (size_t)size / sizeof(uint32_t);
  uint32_t *bits = malloc_or_die(sizeof(uint32_t) * count);
  const bool read = fread_le_uint32s(bits, count, stream);
  (void)fclose(stream);
  if (!read) {
    free(bits);
    error_stack_push(
        error_stack, ERROR_STATUS_VALUE_NET_MISSING_FILE,
        get_formatted_string("could not read value net weights %s", path));
    return NULL;
  }
  static_assert(sizeof(float) == sizeof(uint32_t), "float must be 32 bits");
  float *weights = malloc_or_die(sizeof(float) * count);
  memcpy(weights, bits, sizeof(float) * count);
  free(bits);
  *num_weights = count;
  return weights;
}

ValueNet *value_net_create(const char *dir, ErrorStack *error_stack) {
  char *weights_path = get_formatted_string("%s/weights.f32", dir);
  char *manifest_path = get_formatted_string("%s/manifest.json", dir);
  ValueNet *net = calloc_or_die(1, sizeof(ValueNet));
  net->weights =
      value_net_read_weights(weights_path, &net->num_weights, error_stack);
  char *manifest = NULL;
  if (error_stack_is_empty(error_stack)) {
    manifest = get_string_from_file(manifest_path, error_stack);
  }
  free(weights_path);
  free(manifest_path);
  if (!error_stack_is_empty(error_stack)) {
    free(manifest);
    value_net_destroy(net);
    return NULL;
  }
  value_net_read_shape(net, manifest, error_stack);
  if (!error_stack_is_empty(error_stack)) {
    free(manifest);
    value_net_destroy(net);
    return NULL;
  }
  const size_t dim = (size_t)net->shape.model_dim;
  const size_t hidden = (size_t)net->shape.head_hidden;
  net->blocks = calloc_or_die((size_t)net->shape.layers, sizeof(ValueNetBlock));
  value_net_bind(net, manifest, "pos_emb", VALUE_NET_SQUARES * dim,
                 &net->pos_emb, error_stack);
  value_net_bind(net, manifest, "tile_emb", VALUE_NET_TILE_TYPES * dim,
                 &net->tile_emb, error_stack);
  value_net_bind(net, manifest, "cls", dim, &net->cls, error_stack);
  value_net_bind(net, manifest, "square_proj.weight", dim * VALUE_NET_PLANES,
                 &net->square_proj_weight, error_stack);
  value_net_bind(net, manifest, "square_proj.bias", dim, &net->square_proj_bias,
                 error_stack);
  value_net_bind(net, manifest, "tile_proj.weight", dim * 2,
                 &net->tile_proj_weight, error_stack);
  value_net_bind(net, manifest, "tile_proj.bias", dim, &net->tile_proj_bias,
                 error_stack);
  value_net_bind(net, manifest, "game_proj.weight", dim * VALUE_NET_SCALARS,
                 &net->game_proj_weight, error_stack);
  value_net_bind(net, manifest, "game_proj.bias", dim, &net->game_proj_bias,
                 error_stack);
  for (int layer = 0; layer < net->shape.layers; layer++) {
    value_net_bind_block(net, manifest, layer, error_stack);
  }
  value_net_bind(net, manifest, "ln_f.weight", dim, &net->ln_f_weight,
                 error_stack);
  value_net_bind(net, manifest, "ln_f.bias", dim, &net->ln_f_bias, error_stack);
  value_net_bind(net, manifest, "fc1.weight", hidden * dim, &net->fc1_weight,
                 error_stack);
  value_net_bind(net, manifest, "fc1.bias", hidden, &net->fc1_bias,
                 error_stack);
  value_net_bind(net, manifest, "heads.spread.weight", hidden,
                 &net->spread_weight, error_stack);
  value_net_bind(net, manifest, "heads.spread.bias", 1, &net->spread_bias,
                 error_stack);
  value_net_bind(net, manifest, "heads.wdl.weight", VALUE_NET_WDL * hidden,
                 &net->wdl_weight, error_stack);
  value_net_bind(net, manifest, "heads.wdl.bias", VALUE_NET_WDL, &net->wdl_bias,
                 error_stack);
  net->manifest = manifest;
  if (!error_stack_is_empty(error_stack)) {
    value_net_destroy(net);
    return NULL;
  }
  return net;
}

void value_net_destroy(ValueNet *net) {
  if (net == NULL) {
    return;
  }
  free(net->weights);
  free(net->manifest);
  free(net->blocks);
  free(net);
}

const ValueNetShape *value_net_get_shape(const ValueNet *net) {
  return &net->shape;
}

const float *value_net_get_tensor(const ValueNet *net, const char *name,
                                  size_t count) {
  size_t offset = 0;
  size_t found = 0;
  if (!manifest_find(net->manifest, name, &offset, &found) || found != count ||
      offset + count > net->num_weights) {
    return NULL;
  }
  return net->weights + offset;
}

static float dot(const float *a, const float *b, int length) {
  float lanes[VALUE_NET_DOT_LANES] = {0};
  int idx = 0;
  for (; idx + VALUE_NET_DOT_LANES <= length; idx += VALUE_NET_DOT_LANES) {
    for (int lane = 0; lane < VALUE_NET_DOT_LANES; lane++) {
      lanes[lane] += a[idx + lane] * b[idx + lane];
    }
  }
  float sum = 0.0F;
  for (int lane = 0; lane < VALUE_NET_DOT_LANES; lane++) {
    sum += lanes[lane];
  }
  for (; idx < length; idx++) {
    sum += a[idx] * b[idx];
  }
  return sum;
}

// out[t] = in[t] W^T + bias for tokens rows, W stored [out_dim, in_dim].
static void linear(const float *in, int tokens, int in_dim, const float *weight,
                   const float *bias, int out_dim, float *out) {
  for (int token = 0; token < tokens; token++) {
    const float *row = in + ((size_t)token * in_dim);
    float *result = out + ((size_t)token * out_dim);
    for (int out_idx = 0; out_idx < out_dim; out_idx++) {
      result[out_idx] =
          bias[out_idx] + dot(row, weight + ((size_t)out_idx * in_dim), in_dim);
    }
  }
}

static void layer_norm(const float *in, int tokens, int width,
                       const float *weight, const float *bias, float *out) {
  for (int token = 0; token < tokens; token++) {
    const float *row = in + ((size_t)token * width);
    float *result = out + ((size_t)token * width);
    float mean = 0.0F;
    for (int dim = 0; dim < width; dim++) {
      mean += row[dim];
    }
    mean /= (float)width;
    float variance = 0.0F;
    for (int dim = 0; dim < width; dim++) {
      const float centered = row[dim] - mean;
      variance += centered * centered;
    }
    variance /= (float)width;
    const float scale = 1.0F / sqrtf(variance + VALUE_NET_LAYER_NORM_EPS);
    for (int dim = 0; dim < width; dim++) {
      result[dim] = ((row[dim] - mean) * scale * weight[dim]) + bias[dim];
    }
  }
}

static float gelu(float x) {
  return 0.5F * x * (1.0F + erff(x * VALUE_NET_INV_SQRT2));
}

// Scratch for one row's forward pass.
typedef struct ValueNetScratch {
  float *tokens;
  float *normed;
  float *qkv;
  float *attended;
  float *hidden;
  float *scores;
  float *keys;
  float *values;
  float *cls;
  float *head;
} ValueNetScratch;

// Multi-head self-attention over every token, no mask: scratch->attended
// receives the concatenated heads.
static void attention(const ValueNetShape *shape, ValueNetScratch *scratch) {
  const int model_dim = shape->model_dim;
  const int head_dim = shape->head_dim;
  const int width = 3 * model_dim;
  const float scale = 1.0F / sqrtf((float)head_dim);
  for (int head = 0; head < shape->heads; head++) {
    const int head_offset = head * head_dim;
    // Gather this head's keys and values contiguously.
    for (int token = 0; token < VALUE_NET_TOKENS; token++) {
      const float *row = scratch->qkv + ((size_t)token * width);
      memcpy(scratch->keys + ((size_t)token * head_dim),
             row + model_dim + head_offset, sizeof(float) * head_dim);
      memcpy(scratch->values + ((size_t)token * head_dim),
             row + (2 * model_dim) + head_offset, sizeof(float) * head_dim);
    }
    for (int query = 0; query < VALUE_NET_TOKENS; query++) {
      const float *query_row =
          scratch->qkv + ((size_t)query * width) + head_offset;
      float highest = -INFINITY;
      for (int key = 0; key < VALUE_NET_TOKENS; key++) {
        const float score =
            dot(query_row, scratch->keys + ((size_t)key * head_dim), head_dim) *
            scale;
        scratch->scores[key] = score;
        if (score > highest) {
          highest = score;
        }
      }
      float total = 0.0F;
      for (int key = 0; key < VALUE_NET_TOKENS; key++) {
        scratch->scores[key] = expf(scratch->scores[key] - highest);
        total += scratch->scores[key];
      }
      float *out =
          scratch->attended + ((size_t)query * model_dim) + head_offset;
      for (int dim = 0; dim < head_dim; dim++) {
        out[dim] = 0.0F;
      }
      for (int key = 0; key < VALUE_NET_TOKENS; key++) {
        const float weight = scratch->scores[key] / total;
        const float *value_row = scratch->values + ((size_t)key * head_dim);
        for (int dim = 0; dim < head_dim; dim++) {
          out[dim] += weight * value_row[dim];
        }
      }
    }
  }
}

// Builds the token embeddings for one input row.
static void embed(const ValueNet *net, const float *board, const float *scalars,
                  float *tokens) {
  const int dim = net->shape.model_dim;
  memcpy(tokens, net->cls, sizeof(float) * dim);
  float square_inputs[VALUE_NET_PLANES];
  for (int square = 0; square < VALUE_NET_SQUARES; square++) {
    for (int plane = 0; plane < VALUE_NET_PLANES; plane++) {
      square_inputs[plane] = board[(plane * VALUE_NET_SQUARES) + square];
    }
    float *token = tokens + ((size_t)(1 + square) * dim);
    linear(square_inputs, 1, VALUE_NET_PLANES, net->square_proj_weight,
           net->square_proj_bias, dim, token);
    const float *position = net->pos_emb + ((size_t)square * dim);
    for (int idx = 0; idx < dim; idx++) {
      token[idx] += position[idx];
    }
  }
  for (int tile = 0; tile < VALUE_NET_TILE_TYPES; tile++) {
    const float tile_inputs[2] = {scalars[tile],
                                  scalars[VALUE_NET_TILE_TYPES + tile]};
    float *token = tokens + ((size_t)(1 + VALUE_NET_SQUARES + tile) * dim);
    linear(tile_inputs, 1, 2, net->tile_proj_weight, net->tile_proj_bias, dim,
           token);
    const float *embedding = net->tile_emb + ((size_t)tile * dim);
    for (int idx = 0; idx < dim; idx++) {
      token[idx] += embedding[idx];
    }
  }
  linear(scalars, 1, VALUE_NET_SCALARS, net->game_proj_weight,
         net->game_proj_bias, dim,
         tokens + ((size_t)(VALUE_NET_TOKENS - 1) * dim));
}

static void value_net_evaluate_row(const ValueNet *net, const float *board,
                                   const float *scalars,
                                   ValueNetScratch *scratch, float *value,
                                   float *spread) {
  const ValueNetShape *shape = &net->shape;
  const int tokens = VALUE_NET_TOKENS;
  const int dim = shape->model_dim;
  const int ff_dim = shape->ff_dim;
  const int hidden = shape->head_hidden;
  embed(net, board, scalars, scratch->tokens);
  for (int layer = 0; layer < shape->layers; layer++) {
    const ValueNetBlock *block = &net->blocks[layer];
    layer_norm(scratch->tokens, tokens, dim, block->ln1_weight,
               block->ln1_bias, scratch->normed);
    linear(scratch->normed, tokens, dim, block->qkv_weight, block->qkv_bias,
           3 * dim, scratch->qkv);
    attention(shape, scratch);
    linear(scratch->attended, tokens, dim, block->proj_weight, block->proj_bias,
           dim, scratch->normed);
    for (int idx = 0; idx < tokens * dim; idx++) {
      scratch->tokens[idx] += scratch->normed[idx];
    }
    layer_norm(scratch->tokens, tokens, dim, block->ln2_weight,
               block->ln2_bias, scratch->normed);
    linear(scratch->normed, tokens, dim, block->fc1_weight, block->fc1_bias,
           ff_dim, scratch->hidden);
    for (int idx = 0; idx < tokens * ff_dim; idx++) {
      scratch->hidden[idx] = gelu(scratch->hidden[idx]);
    }
    linear(scratch->hidden, tokens, ff_dim, block->fc2_weight, block->fc2_bias,
           dim, scratch->normed);
    for (int idx = 0; idx < tokens * dim; idx++) {
      scratch->tokens[idx] += scratch->normed[idx];
    }
  }
  float *cls = scratch->cls;
  layer_norm(scratch->tokens, 1, dim, net->ln_f_weight, net->ln_f_bias, cls);
  float *head = scratch->head;
  linear(cls, 1, dim, net->fc1_weight, net->fc1_bias, hidden, head);
  for (int idx = 0; idx < hidden; idx++) {
    if (head[idx] < 0.0F) {
      head[idx] = 0.0F;
    }
  }
  if (value != NULL) {
    float logits[VALUE_NET_WDL];
    linear(head, 1, hidden, net->wdl_weight, net->wdl_bias, VALUE_NET_WDL,
           logits);
    float highest = logits[0];
    for (int idx = 1; idx < VALUE_NET_WDL; idx++) {
      if (logits[idx] > highest) {
        highest = logits[idx];
      }
    }
    float probabilities[VALUE_NET_WDL];
    float total = 0.0F;
    for (int idx = 0; idx < VALUE_NET_WDL; idx++) {
      probabilities[idx] = expf(logits[idx] - highest);
      total += probabilities[idx];
    }
    // [loss, draw, win]
    *value = (probabilities[2] - probabilities[0]) / total;
  }
  if (spread != NULL) {
    float raw = 0.0F;
    linear(head, 1, hidden, net->spread_weight, net->spread_bias, 1, &raw);
    *spread = tanhf(raw);
  }
}

void value_net_evaluate_cpu(const ValueNet *net, int rows, const float *board,
                            const float *scalars, float *value, float *spread) {
  const size_t tokens = VALUE_NET_TOKENS;
  const size_t dim = (size_t)net->shape.model_dim;
  const size_t head_dim = (size_t)net->shape.head_dim;
  ValueNetScratch scratch = {
      .tokens = malloc_or_die(sizeof(float) * tokens * dim),
      .normed = malloc_or_die(sizeof(float) * tokens * dim),
      .qkv = malloc_or_die(sizeof(float) * tokens * 3 * dim),
      .attended = malloc_or_die(sizeof(float) * tokens * dim),
      .hidden =
          malloc_or_die(sizeof(float) * tokens * (size_t)net->shape.ff_dim),
      .scores = malloc_or_die(sizeof(float) * tokens),
      .keys = malloc_or_die(sizeof(float) * tokens * head_dim),
      .values = malloc_or_die(sizeof(float) * tokens * head_dim),
      .cls = malloc_or_die(sizeof(float) * dim),
      .head = malloc_or_die(sizeof(float) * (size_t)net->shape.head_hidden),
  };
  for (int row = 0; row < rows; row++) {
    value_net_evaluate_row(net, board + ((size_t)row * VALUE_NET_BOARD_FLOATS),
                           scalars + ((size_t)row * VALUE_NET_SCALARS),
                           &scratch, value != NULL ? value + row : NULL,
                           spread != NULL ? spread + row : NULL);
  }
  free(scratch.tokens);
  free(scratch.normed);
  free(scratch.qkv);
  free(scratch.attended);
  free(scratch.hidden);
  free(scratch.scores);
  free(scratch.keys);
  free(scratch.values);
  free(scratch.cls);
  free(scratch.head);
}
