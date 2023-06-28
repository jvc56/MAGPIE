#ifndef INFERENCE_PRINT_H
#define INFERENCE_PRINT_H

#include "../src/infer.h"
#include "../src/rack.h"

void print_inference(Inference *inference, Rack *actual_tiles_played,
                     int number_of_threads);

#endif