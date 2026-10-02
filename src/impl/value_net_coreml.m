#include "value_net_coreml.h"

#include "../def/value_net_defs.h"
#include "../util/io_util.h"
#include "../util/string_util.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#import <CoreML/CoreML.h>
#import <Foundation/Foundation.h>

enum {
  VALUE_NET_COREML_DEFAULT_CONCURRENCY = 6,
};

enum {
  VALUE_NET_COREML_MAX_INSTANCES = 16,
};

struct ValueNetCoreML {
  // One loaded model per concurrent request: CoreML runs one prediction at
  // a time per model, and the Neural Engine needs several in flight.
  // Retained through bridged pointers so the struct stays plain C.
  void *models[VALUE_NET_COREML_MAX_INSTANCES];
  // The model's fixed batch size (its board input's first dimension).
  int batch;
  int concurrency;
};

static void coreml_push_error(ErrorStack *error_stack, const char *what,
                              NSError *error) {
  error_stack_push(error_stack, ERROR_STATUS_VALUE_NET_BACKEND_UNAVAILABLE,
                   get_formatted_string(
                       "%s: %s", what,
                       error != nil ? error.localizedDescription.UTF8String
                                    : "unknown error"));
}

ValueNetCoreML *value_net_coreml_create(const char *path, int concurrency,
                                        ErrorStack *error_stack) {
  @autoreleasepool {
    NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path]];
    NSError *error = nil;
    NSURL *compiled = [MLModel compileModelAtURL:url error:&error];
    if (compiled == nil) {
      coreml_push_error(error_stack, "could not compile the CoreML value net",
                        error);
      return NULL;
    }
    MLModelConfiguration *config = [MLModelConfiguration new];
    config.computeUnits = MLComputeUnitsCPUAndNeuralEngine;
    MLModel *model = [MLModel modelWithContentsOfURL:compiled
                                       configuration:config
                                               error:&error];
    const int instances =
        concurrency > 0 && concurrency <= VALUE_NET_COREML_MAX_INSTANCES
            ? concurrency
            : VALUE_NET_COREML_DEFAULT_CONCURRENCY;
    if (model == nil) {
      coreml_push_error(error_stack, "could not load the CoreML value net",
                        error);
      return NULL;
    }
    MLFeatureDescription *board_description =
        model.modelDescription.inputDescriptionsByName[@"board"];
    MLFeatureDescription *scalars_description =
        model.modelDescription.inputDescriptionsByName[@"scalars"];
    NSArray<NSNumber *> *shape =
        board_description.multiArrayConstraint.shape;
    if (board_description == nil || scalars_description == nil ||
        shape.count != 3 || shape[1].intValue != VALUE_NET_PLANES ||
        shape[2].intValue != VALUE_NET_SQUARES ||
        model.modelDescription.outputDescriptionsByName[@"value"] == nil) {
      error_stack_push(
          error_stack, ERROR_STATUS_VALUE_NET_UNEXPECTED_TENSOR,
          string_duplicate("the CoreML value net needs inputs board "
                           "[batch, 85, 225] and scalars [batch, 72] and "
                           "output value [batch]"));
      return NULL;
    }
    ValueNetCoreML *coreml = calloc_or_die(1, sizeof(ValueNetCoreML));
    coreml->models[0] = (__bridge_retained void *)model;
    for (int instance = 1; instance < instances; instance++) {
      MLModel *copy = [MLModel modelWithContentsOfURL:compiled
                                        configuration:config
                                                error:&error];
      if (copy == nil) {
        coreml_push_error(error_stack, "could not load the CoreML value net",
                          error);
        coreml->concurrency = instance;
        value_net_coreml_destroy(coreml);
        return NULL;
      }
      coreml->models[instance] = (__bridge_retained void *)copy;
    }
    coreml->batch = shape[0].intValue;
    coreml->concurrency = instances;
    return coreml;
  }
}

void value_net_coreml_destroy(ValueNetCoreML *coreml) {
  if (coreml == NULL) {
    return;
  }
  for (int instance = 0; instance < coreml->concurrency; instance++) {
    (void)(__bridge_transfer id)coreml->models[instance];
  }
  free(coreml);
}

// Evaluates one chunk of at most coreml->batch rows (padded with copies of
// its first row) into value.
static void coreml_evaluate_chunk(const ValueNetCoreML *coreml, int instance,
                                  int rows, const float *board,
                                  const float *scalars, float *value) {
  @autoreleasepool {
    const int batch = coreml->batch;
    const size_t board_floats = (size_t)batch * VALUE_NET_BOARD_FLOATS;
    const size_t scalar_floats = (size_t)batch * VALUE_NET_SCALARS;
    float *board_in = malloc_or_die(sizeof(float) * board_floats);
    float *scalars_in = malloc_or_die(sizeof(float) * scalar_floats);
    memcpy(board_in, board,
           sizeof(float) * (size_t)rows * VALUE_NET_BOARD_FLOATS);
    memcpy(scalars_in, scalars,
           sizeof(float) * (size_t)rows * VALUE_NET_SCALARS);
    for (int pad_idx = rows; pad_idx < batch; pad_idx++) {
      memcpy(board_in + ((size_t)pad_idx * VALUE_NET_BOARD_FLOATS), board,
             sizeof(float) * VALUE_NET_BOARD_FLOATS);
      memcpy(scalars_in + ((size_t)pad_idx * VALUE_NET_SCALARS), scalars,
             sizeof(float) * VALUE_NET_SCALARS);
    }
    NSError *error = nil;
    MLMultiArray *board_array = [[MLMultiArray alloc]
        initWithDataPointer:board_in
                      shape:@[
                        @(batch), @(VALUE_NET_PLANES), @(VALUE_NET_SQUARES)
                      ]
                   dataType:MLMultiArrayDataTypeFloat32
                    strides:@[
                      @(VALUE_NET_BOARD_FLOATS), @(VALUE_NET_SQUARES), @1
                    ]
                deallocator:^(void *bytes) {
                  free(bytes);
                }
                      error:&error];
    MLMultiArray *scalars_array = [[MLMultiArray alloc]
        initWithDataPointer:scalars_in
                      shape:@[ @(batch), @(VALUE_NET_SCALARS) ]
                   dataType:MLMultiArrayDataTypeFloat32
                    strides:@[ @(VALUE_NET_SCALARS), @1 ]
                deallocator:^(void *bytes) {
                  free(bytes);
                }
                      error:&error];
    if (board_array == nil || scalars_array == nil) {
      log_fatal("could not wrap value net inputs for CoreML: %s",
                error.localizedDescription.UTF8String);
    }
    MLDictionaryFeatureProvider *inputs = [[MLDictionaryFeatureProvider alloc]
        initWithDictionary:@{
          @"board" : [MLFeatureValue featureValueWithMultiArray:board_array],
          @"scalars" :
              [MLFeatureValue featureValueWithMultiArray:scalars_array]
        }
                     error:&error];
    MLModel *model = (__bridge MLModel *)coreml->models[instance];
    id<MLFeatureProvider> outputs = [model predictionFromFeatures:inputs
                                                            error:&error];
    if (outputs == nil) {
      log_fatal("CoreML value net prediction failed: %s",
                error.localizedDescription.UTF8String);
    }
    MLMultiArray *values = [outputs featureValueForName:@"value"].multiArrayValue;
    const NSInteger stride = values.strides.lastObject.integerValue;
    if (values.dataType == MLMultiArrayDataTypeFloat16) {
      const __fp16 *halves = (const __fp16 *)values.dataPointer;
      for (int row = 0; row < rows; row++) {
        value[row] = (float)halves[row * stride];
      }
    } else if (values.dataType == MLMultiArrayDataTypeFloat32) {
      const float *floats = (const float *)values.dataPointer;
      for (int row = 0; row < rows; row++) {
        value[row] = floats[row * stride];
      }
    } else {
      for (int row = 0; row < rows; row++) {
        value[row] = values[row].floatValue;
      }
    }
  }
}

void value_net_coreml_evaluate(ValueNetCoreML *coreml, int rows,
                               const float *board, const float *scalars,
                               float *value) {
  if (rows <= 0) {
    return;
  }
  const int batch = coreml->batch;
  const int chunks = (rows + batch - 1) / batch;
  const int lanes = chunks < coreml->concurrency ? chunks : coreml->concurrency;
  // Lane k runs chunks k, k + lanes, ... on model instance k, so up to
  // lanes predictions are in flight and none shares an instance. Callers
  // on several threads share the instances; CoreML serializes each.
  dispatch_queue_t queue =
      dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0);
  dispatch_apply((size_t)lanes, queue, ^(size_t lane) {
    for (int chunk = (int)lane; chunk < chunks; chunk += lanes) {
      const int start = chunk * batch;
      const int count = rows - start < batch ? rows - start : batch;
      coreml_evaluate_chunk(
          coreml, (int)lane, count,
          board + ((size_t)start * VALUE_NET_BOARD_FLOATS),
          scalars + ((size_t)start * VALUE_NET_SCALARS), value + start);
    }
  });
}
