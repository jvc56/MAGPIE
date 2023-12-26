#ifndef INFERENCE_STRING_H
#define INFERENCE_STRING_H

#include "../ent/rack.h"

#include "../impl/inference.h"

#include "../util/string_util.h"

void print_ucgi_inference_current_rack(uint64_t current_rack_index,
                                       ThreadControl *thread_control);
void print_ucgi_inference_total_racks_evaluated(uint64_t total_racks_evaluated,
                                                ThreadControl *thread_control);
void print_ucgi_inference(const LetterDistribution *ld,
                          InferenceResults *inference_results,
                          ThreadControl *thread_control);

void string_builder_add_inference(const LetterDistribution *ld,
                                  InferenceResults *inference_results,
                                  const Rack *target_played_tiles,
                                  StringBuilder *inference_string);

#endif
