#ifndef INFERENCE_STRING_H
#define INFERENCE_STRING_H

#include "../ent/rack.h"
#include "../util/string_util.h"

void print_ucgi_inference_current_rack(uint64_t current_rack_index,
                                       ThreadControl *thread_control);
void print_ucgi_inference_total_racks_evaluated(uint64_t total_racks_evaluated,
                                                ThreadControl *thread_control);
void string_builder_add_inference(StringBuilder *inference_string,
                                  InferenceResults *inference_results,
                                  const LetterDistribution *ld,
                                  int max_num_leaves_to_display,
                                  bool use_ucgi_format);
char *inference_result_get_string(InferenceResults *inference_results,
                                  const LetterDistribution *ld,
                                  int max_num_leaves_to_display,
                                  bool use_ucgi_format);

#endif
