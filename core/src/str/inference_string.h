#ifndef INFERENCE_STRING_H
#define INFERENCE_STRING_H

#include "../ent/inference.h"
#include "../ent/rack.h"

#include "../util/string_util.h"

void print_ucgi_inference_current_rack(uint64_t current_rack_index,
                                       ThreadControl *thread_control);
void print_ucgi_inference_total_racks_evaluated(uint64_t total_racks_evaluated,
                                                ThreadControl *thread_control);
void print_ucgi_inference(const Inference *inference,
                          ThreadControl *thread_control);

void string_builder_add_inference(const Inference *inference,
                                  const Rack *actual_tiles_played,
                                  StringBuilder *inference_string);

#endif
