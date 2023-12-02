#ifndef INFERENCE_STRING_H
#define INFERENCE_STRING_H

#include "../ent/inference.h"
#include "../ent/rack.h"

#include "string_util.h"

void string_builder_add_inference(const Inference *inference,
                                  const Rack *actual_tiles_played,
                                  StringBuilder *inference_string);

#endif
