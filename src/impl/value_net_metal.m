#include "value_net_metal.h"

#include "../compat/cpthread.h"
#include "../def/value_net_defs.h"
#include "../ent/value_net.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include <stdbool.h>
#include <stdlib.h>

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <MetalPerformanceShadersGraph/MetalPerformanceShadersGraph.h>

// The whole forward pass is one MPSGraph built once from the weights (as
// constants), with a dynamic batch dimension; each evaluation feeds the
// input rows and reads value and spread back.
struct ValueNetMetal {
  // Retained Objective-C objects, held through bridged pointers so the
  // struct stays plain C.
  void *device;
  void *queue;
  void *graph;
  void *board_input;
  void *scalars_input;
  void *value_output;
  void *spread_output;
  MPSDataType data_type;
  cpthread_mutex_t mutex;
};

// Builds a constant from count floats, converted to the graph's type,
// optionally transposed from [rows, cols] to [cols, rows].
static MPSGraphTensor *metal_constant(MPSGraph *graph, const float *values,
                                      NSArray<NSNumber *> *shape,
                                      MPSDataType data_type) {
  size_t count = 1;
  for (NSNumber *dim in shape) {
    count *= (size_t)dim.unsignedLongLongValue;
  }
  NSData *data = nil;
  if (data_type == MPSDataTypeFloat16) {
    __fp16 *halves = malloc_or_die(sizeof(__fp16) * count);
    for (size_t idx = 0; idx < count; idx++) {
      halves[idx] = (__fp16)values[idx];
    }
    data = [NSData dataWithBytesNoCopy:halves
                                length:sizeof(__fp16) * count
                          freeWhenDone:YES];
  } else {
    data = [NSData dataWithBytes:values length:sizeof(float) * count];
  }
  return [graph constantWithData:data shape:shape dataType:data_type];
}

// The weight of a Linear layer, [out, in], as an [in, out] matrix for
// x * W.
static MPSGraphTensor *metal_linear_weight(MPSGraph *graph, const float *values,
                                           int out_dim, int in_dim,
                                           MPSDataType data_type) {
  float *transposed = malloc_or_die(sizeof(float) * (size_t)out_dim * in_dim);
  for (int out_idx = 0; out_idx < out_dim; out_idx++) {
    for (int in_idx = 0; in_idx < in_dim; in_idx++) {
      transposed[((size_t)in_idx * out_dim) + out_idx] =
          values[((size_t)out_idx * in_dim) + in_idx];
    }
  }
  MPSGraphTensor *tensor =
      metal_constant(graph, transposed, @[ @(in_dim), @(out_dim) ], data_type);
  free(transposed);
  return tensor;
}

typedef struct MetalBuilder {
  MPSGraph *graph;
  const ValueNet *net;
  MPSDataType data_type;
  bool ok;
  ErrorStack *error_stack;
} MetalBuilder;

static const float *builder_tensor(MetalBuilder *builder, const char *name,
                                   size_t count) {
  const float *tensor = value_net_get_tensor(builder->net, name, count);
  if (tensor == NULL && builder->ok) {
    builder->ok = false;
    error_stack_push(builder->error_stack,
                     ERROR_STATUS_VALUE_NET_UNEXPECTED_TENSOR,
                     get_formatted_string("value net has no tensor %s of %zu "
                                          "floats",
                                          name, count));
  }
  return tensor;
}

// x W^T + b for the named Linear layer, on x's last axis.
static MPSGraphTensor *builder_linear(MetalBuilder *builder, MPSGraphTensor *x,
                                      const char *prefix, int in_dim,
                                      int out_dim) {
  char *weight_name = get_formatted_string("%s.weight", prefix);
  char *bias_name = get_formatted_string("%s.bias", prefix);
  const float *weight =
      builder_tensor(builder, weight_name, (size_t)out_dim * in_dim);
  const float *bias = builder_tensor(builder, bias_name, (size_t)out_dim);
  free(weight_name);
  free(bias_name);
  if (weight == NULL || bias == NULL) {
    return x;
  }
  MPSGraph *graph = builder->graph;
  MPSGraphTensor *product = [graph
      matrixMultiplicationWithPrimaryTensor:x
                            secondaryTensor:metal_linear_weight(
                                                graph, weight, out_dim, in_dim,
                                                builder->data_type)
                                       name:nil];
  return [graph
      additionWithPrimaryTensor:product
                secondaryTensor:metal_constant(graph, bias, @[ @(out_dim) ],
                                               builder->data_type)
                           name:nil];
}

// LayerNorm over the last axis (eps 1e-5) with the named weight and bias.
static MPSGraphTensor *builder_layer_norm(MetalBuilder *builder,
                                          MPSGraphTensor *x,
                                          const char *prefix) {
  char *weight_name = get_formatted_string("%s.weight", prefix);
  char *bias_name = get_formatted_string("%s.bias", prefix);
  const float *weight =
      builder_tensor(builder, weight_name, VALUE_NET_MODEL_DIM);
  const float *bias = builder_tensor(builder, bias_name, VALUE_NET_MODEL_DIM);
  free(weight_name);
  free(bias_name);
  if (weight == NULL || bias == NULL) {
    return x;
  }
  MPSGraph *graph = builder->graph;
  NSArray<NSNumber *> *axes = @[ @(-1) ];
  MPSGraphTensor *mean = [graph meanOfTensor:x axes:axes name:nil];
  MPSGraphTensor *variance = [graph varianceOfTensor:x
                                          meanTensor:mean
                                                axes:axes
                                                name:nil];
  MPSGraphTensor *epsilon = [graph constantWithScalar:1e-5
                                             dataType:builder->data_type];
  MPSGraphTensor *inverse = [graph
      reciprocalSquareRootWithTensor:[graph additionWithPrimaryTensor:variance
                                                   secondaryTensor:epsilon
                                                              name:nil]
                             name:nil];
  MPSGraphTensor *normed = [graph
      multiplicationWithPrimaryTensor:[graph subtractionWithPrimaryTensor:x
                                                          secondaryTensor:mean
                                                                     name:nil]
                      secondaryTensor:inverse
                                 name:nil];
  MPSGraphTensor *scaled = [graph
      multiplicationWithPrimaryTensor:normed
                      secondaryTensor:metal_constant(
                                          graph, weight,
                                          @[ @(VALUE_NET_MODEL_DIM) ],
                                          builder->data_type)
                                 name:nil];
  return [graph
      additionWithPrimaryTensor:scaled
                secondaryTensor:metal_constant(graph, bias,
                                               @[ @(VALUE_NET_MODEL_DIM) ],
                                               builder->data_type)
                           name:nil];
}

// Exact-erf GELU.
static MPSGraphTensor *builder_gelu(MetalBuilder *builder, MPSGraphTensor *x) {
  MPSGraph *graph = builder->graph;
  const MPSDataType type = builder->data_type;
  MPSGraphTensor *scaled = [graph
      multiplicationWithPrimaryTensor:x
                      secondaryTensor:[graph constantWithScalar:M_SQRT1_2
                                                       dataType:type]
                                 name:nil];
  MPSGraphTensor *one_plus_erf = [graph
      additionWithPrimaryTensor:[graph erfWithTensor:scaled name:nil]
                secondaryTensor:[graph constantWithScalar:1.0 dataType:type]
                           name:nil];
  MPSGraphTensor *half_x = [graph
      multiplicationWithPrimaryTensor:x
                      secondaryTensor:[graph constantWithScalar:0.5
                                                       dataType:type]
                                 name:nil];
  return [graph multiplicationWithPrimaryTensor:half_x
                                secondaryTensor:one_plus_erf
                                           name:nil];
}

// One pre-norm transformer block on x [B, tokens, dim].
static MPSGraphTensor *builder_block(MetalBuilder *builder, MPSGraphTensor *x,
                                     int layer) {
  MPSGraph *graph = builder->graph;
  char *prefix = get_formatted_string("blocks.%d.ln1", layer);
  MPSGraphTensor *normed = builder_layer_norm(builder, x, prefix);
  free(prefix);
  prefix = get_formatted_string("blocks.%d.qkv", layer);
  MPSGraphTensor *qkv = builder_linear(builder, normed, prefix,
                                       VALUE_NET_MODEL_DIM,
                                       3 * VALUE_NET_MODEL_DIM);
  free(prefix);
  // [B, T, 3, heads, head_dim] -> [3, B, heads, T, head_dim]
  MPSGraphTensor *split =
      [graph reshapeTensor:qkv
                 withShape:@[
                   @(-1), @(VALUE_NET_TOKENS), @3, @(VALUE_NET_HEADS),
                   @(VALUE_NET_HEAD_DIM)
                 ]
                      name:nil];
  MPSGraphTensor *heads = [graph transposeTensor:split
                                     permutation:@[ @2, @0, @3, @1, @4 ]
                                            name:nil];
  MPSGraphTensor *parts[3];
  for (int part = 0; part < 3; part++) {
    MPSGraphTensor *slice = [graph sliceTensor:heads
                                     dimension:0
                                         start:part
                                        length:1
                                          name:nil];
    parts[part] = [graph squeezeTensor:slice axis:0 name:nil];
  }
  MPSGraphTensor *keys_t = [graph transposeTensor:parts[1]
                                        dimension:2
                                    withDimension:3
                                             name:nil];
  MPSGraphTensor *scores =
      [graph matrixMultiplicationWithPrimaryTensor:parts[0]
                                   secondaryTensor:keys_t
                                              name:nil];
  scores = [graph
      multiplicationWithPrimaryTensor:scores
                      secondaryTensor:
                          [graph constantWithScalar:1.0 / sqrt(VALUE_NET_HEAD_DIM)
                                           dataType:builder->data_type]
                                 name:nil];
  MPSGraphTensor *weights = [graph softMaxWithTensor:scores axis:-1 name:nil];
  MPSGraphTensor *attended =
      [graph matrixMultiplicationWithPrimaryTensor:weights
                                   secondaryTensor:parts[2]
                                              name:nil];
  // [B, heads, T, head_dim] -> [B, T, dim]
  attended = [graph transposeTensor:attended dimension:1 withDimension:2 name:nil];
  attended = [graph
      reshapeTensor:attended
          withShape:@[ @(-1), @(VALUE_NET_TOKENS), @(VALUE_NET_MODEL_DIM) ]
               name:nil];
  prefix = get_formatted_string("blocks.%d.proj", layer);
  x = [graph additionWithPrimaryTensor:x
                       secondaryTensor:builder_linear(builder, attended, prefix,
                                                      VALUE_NET_MODEL_DIM,
                                                      VALUE_NET_MODEL_DIM)
                                  name:nil];
  free(prefix);
  prefix = get_formatted_string("blocks.%d.ln2", layer);
  normed = builder_layer_norm(builder, x, prefix);
  free(prefix);
  prefix = get_formatted_string("blocks.%d.fc1", layer);
  MPSGraphTensor *hidden = builder_gelu(
      builder, builder_linear(builder, normed, prefix, VALUE_NET_MODEL_DIM,
                              VALUE_NET_FF_DIM));
  free(prefix);
  prefix = get_formatted_string("blocks.%d.fc2", layer);
  x = [graph additionWithPrimaryTensor:x
                       secondaryTensor:builder_linear(builder, hidden, prefix,
                                                      VALUE_NET_FF_DIM,
                                                      VALUE_NET_MODEL_DIM)
                                  name:nil];
  free(prefix);
  return x;
}

static void builder_build(MetalBuilder *builder, MPSGraphTensor *board,
                          MPSGraphTensor *scalars, MPSGraphTensor **value,
                          MPSGraphTensor **spread) {
  MPSGraph *graph = builder->graph;
  const MPSDataType type = builder->data_type;
  const int dim = VALUE_NET_MODEL_DIM;
  // Square tokens: [B, planes, squares] -> [B, squares, planes] -> proj.
  MPSGraphTensor *square_inputs = [graph transposeTensor:board
                                               dimension:1
                                           withDimension:2
                                                    name:nil];
  MPSGraphTensor *squares = builder_linear(builder, square_inputs,
                                           "square_proj", VALUE_NET_PLANES, dim);
  const float *pos_emb =
      builder_tensor(builder, "pos_emb", (size_t)VALUE_NET_SQUARES * dim);
  // Tile tokens: [B, 27, 2] from the leave and unseen scalars.
  MPSGraphTensor *leave = [graph sliceTensor:scalars
                                   dimension:1
                                       start:0
                                      length:VALUE_NET_TILE_TYPES
                                        name:nil];
  MPSGraphTensor *unseen = [graph sliceTensor:scalars
                                    dimension:1
                                        start:VALUE_NET_TILE_TYPES
                                       length:VALUE_NET_TILE_TYPES
                                         name:nil];
  MPSGraphTensor *tile_inputs = [graph stackTensors:@[ leave, unseen ]
                                               axis:2
                                               name:nil];
  MPSGraphTensor *tiles =
      builder_linear(builder, tile_inputs, "tile_proj", 2, dim);
  const float *tile_emb =
      builder_tensor(builder, "tile_emb", (size_t)VALUE_NET_TILE_TYPES * dim);
  const float *cls = builder_tensor(builder, "cls", (size_t)dim);
  MPSGraphTensor *game = builder_linear(builder, scalars, "game_proj",
                                        VALUE_NET_SCALARS, dim);
  if (!builder->ok) {
    return;
  }
  squares = [graph
      additionWithPrimaryTensor:squares
                secondaryTensor:metal_constant(graph, pos_emb,
                                               @[ @(VALUE_NET_SQUARES), @(dim) ],
                                               type)
                           name:nil];
  tiles = [graph
      additionWithPrimaryTensor:tiles
                secondaryTensor:metal_constant(
                                    graph, tile_emb,
                                    @[ @(VALUE_NET_TILE_TYPES), @(dim) ], type)
                           name:nil];
  game = [graph reshapeTensor:game withShape:@[ @(-1), @1, @(dim) ] name:nil];
  // The cls token, broadcast over the batch: zeros shaped like the game
  // token plus the constant.
  MPSGraphTensor *cls_token = [graph
      additionWithPrimaryTensor:[graph multiplicationWithPrimaryTensor:game
                                                       secondaryTensor:
                                                           [graph
                                                               constantWithScalar:
                                                                   0.0
                                                                         dataType:
                                                                             type]
                                                                  name:nil]
                secondaryTensor:metal_constant(graph, cls, @[ @1, @(dim) ],
                                               type)
                           name:nil];
  MPSGraphTensor *x = [graph concatTensors:@[ cls_token, squares, tiles, game ]
                                 dimension:1
                                      name:nil];
  for (int layer = 0; layer < VALUE_NET_LAYERS; layer++) {
    x = builder_block(builder, x, layer);
  }
  MPSGraphTensor *first = [graph sliceTensor:x
                                   dimension:1
                                       start:0
                                      length:1
                                        name:nil];
  first = [graph reshapeTensor:first withShape:@[ @(-1), @(dim) ] name:nil];
  MPSGraphTensor *normed = builder_layer_norm(builder, first, "ln_f");
  MPSGraphTensor *hidden = [graph
      reLUWithTensor:builder_linear(builder, normed, "fc1", dim,
                                    VALUE_NET_HEAD_HIDDEN)
                name:nil];
  MPSGraphTensor *logits = builder_linear(builder, hidden, "heads.wdl",
                                          VALUE_NET_HEAD_HIDDEN, VALUE_NET_WDL);
  MPSGraphTensor *probabilities = [graph softMaxWithTensor:logits
                                                      axis:-1
                                                      name:nil];
  MPSGraphTensor *loss = [graph sliceTensor:probabilities
                                  dimension:1
                                      start:0
                                     length:1
                                       name:nil];
  MPSGraphTensor *win = [graph sliceTensor:probabilities
                                 dimension:1
                                     start:2
                                    length:1
                                      name:nil];
  *value = [graph castTensor:[graph subtractionWithPrimaryTensor:win
                                                 secondaryTensor:loss
                                                            name:nil]
                      toType:MPSDataTypeFloat32
                        name:nil];
  *spread = [graph
      castTensor:[graph tanhWithTensor:builder_linear(builder, hidden,
                                                      "heads.spread",
                                                      VALUE_NET_HEAD_HIDDEN, 1)
                                  name:nil]
          toType:MPSDataTypeFloat32
            name:nil];
}

ValueNetMetal *value_net_metal_create(const ValueNet *net, bool half_precision,
                                      ErrorStack *error_stack) {
  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (device == nil) {
      error_stack_push(error_stack, ERROR_STATUS_VALUE_NET_BACKEND_UNAVAILABLE,
                       string_duplicate("no Metal device"));
      return NULL;
    }
    MPSGraph *graph = [MPSGraph new];
    const MPSDataType type =
        half_precision ? MPSDataTypeFloat16 : MPSDataTypeFloat32;
    MPSGraphTensor *board_input = [graph
        placeholderWithShape:@[
          @(-1), @(VALUE_NET_PLANES), @(VALUE_NET_SQUARES)
        ]
                    dataType:MPSDataTypeFloat32
                        name:@"board"];
    MPSGraphTensor *scalars_input =
        [graph placeholderWithShape:@[ @(-1), @(VALUE_NET_SCALARS) ]
                           dataType:MPSDataTypeFloat32
                               name:@"scalars"];
    MetalBuilder builder = {
        .graph = graph,
        .net = net,
        .data_type = type,
        .ok = true,
        .error_stack = error_stack,
    };
    MPSGraphTensor *value = nil;
    MPSGraphTensor *spread = nil;
    builder_build(&builder,
                  [graph castTensor:board_input toType:type name:nil],
                  [graph castTensor:scalars_input toType:type name:nil],
                  &value, &spread);
    if (!builder.ok) {
      return NULL;
    }
    ValueNetMetal *metal = calloc_or_die(1, sizeof(ValueNetMetal));
    metal->device = (__bridge_retained void *)device;
    metal->queue = (__bridge_retained void *)[device newCommandQueue];
    metal->graph = (__bridge_retained void *)graph;
    metal->board_input = (__bridge_retained void *)board_input;
    metal->scalars_input = (__bridge_retained void *)scalars_input;
    metal->value_output = (__bridge_retained void *)value;
    metal->spread_output = (__bridge_retained void *)spread;
    metal->data_type = type;
    cpthread_mutex_init(&metal->mutex);
    return metal;
  }
}

void value_net_metal_destroy(ValueNetMetal *metal) {
  if (metal == NULL) {
    return;
  }
  // Transfer ownership back to ARC, which releases each object.
  (void)(__bridge_transfer id)metal->spread_output;
  (void)(__bridge_transfer id)metal->value_output;
  (void)(__bridge_transfer id)metal->scalars_input;
  (void)(__bridge_transfer id)metal->board_input;
  (void)(__bridge_transfer id)metal->graph;
  (void)(__bridge_transfer id)metal->queue;
  (void)(__bridge_transfer id)metal->device;
  free(metal);
}

// Evaluates one chunk of at most VALUE_NET_MAX_GPU_ROWS rows; the caller
// holds the mutex.
static void value_net_metal_evaluate_chunk(ValueNetMetal *metal, int rows,
                                           const float *board,
                                           const float *scalars, float *value,
                                           float *spread) {
  @autoreleasepool {
    id<MTLDevice> device = (__bridge id<MTLDevice>)metal->device;
    MPSGraph *graph = (__bridge MPSGraph *)metal->graph;
    MPSGraphTensor *board_input = (__bridge MPSGraphTensor *)metal->board_input;
    MPSGraphTensor *scalars_input =
        (__bridge MPSGraphTensor *)metal->scalars_input;
    MPSGraphTensor *value_output =
        (__bridge MPSGraphTensor *)metal->value_output;
    MPSGraphTensor *spread_output =
        (__bridge MPSGraphTensor *)metal->spread_output;
    MPSGraphDevice *graph_device = [MPSGraphDevice deviceWithMTLDevice:device];
    NSData *board_data =
        [NSData dataWithBytesNoCopy:(void *)board
                             length:sizeof(float) * (size_t)rows *
                                    VALUE_NET_BOARD_FLOATS
                       freeWhenDone:NO];
    NSData *scalars_data =
        [NSData dataWithBytesNoCopy:(void *)scalars
                             length:sizeof(float) * (size_t)rows *
                                    VALUE_NET_SCALARS
                       freeWhenDone:NO];
    MPSGraphTensorData *board_feed = [[MPSGraphTensorData alloc]
        initWithDevice:graph_device
                  data:board_data
                 shape:@[
                   @(rows), @(VALUE_NET_PLANES), @(VALUE_NET_SQUARES)
                 ]
              dataType:MPSDataTypeFloat32];
    MPSGraphTensorData *scalars_feed = [[MPSGraphTensorData alloc]
        initWithDevice:graph_device
                  data:scalars_data
                 shape:@[ @(rows), @(VALUE_NET_SCALARS) ]
              dataType:MPSDataTypeFloat32];
    NSDictionary<MPSGraphTensor *, MPSGraphTensorData *> *results =
        [graph runWithMTLCommandQueue:(__bridge id<MTLCommandQueue>)metal->queue
                                feeds:@{
                                  board_input : board_feed,
                                  scalars_input : scalars_feed
                                }
                        targetTensors:@[ value_output, spread_output ]
                     targetOperations:nil];
    if (value != NULL) {
      [[results[value_output] mpsndarray] readBytes:value strideBytes:nil];
    }
    if (spread != NULL) {
      [[results[spread_output] mpsndarray] readBytes:spread strideBytes:nil];
    }
  }
}

void value_net_metal_evaluate(ValueNetMetal *metal, int rows,
                              const float *board, const float *scalars,
                              float *value, float *spread) {
  cpthread_mutex_lock(&metal->mutex);
  for (int start = 0; start < rows; start += VALUE_NET_MAX_GPU_ROWS) {
    const int chunk = rows - start < VALUE_NET_MAX_GPU_ROWS
                          ? rows - start
                          : VALUE_NET_MAX_GPU_ROWS;
    value_net_metal_evaluate_chunk(
        metal, chunk, board + ((size_t)start * VALUE_NET_BOARD_FLOATS),
        scalars + ((size_t)start * VALUE_NET_SCALARS),
        value != NULL ? value + start : NULL,
        spread != NULL ? spread + start : NULL);
  }
  cpthread_mutex_unlock(&metal->mutex);
}
