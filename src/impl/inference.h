#ifndef INFERENCE_H
#define INFERENCE_H

#include "../def/inference_defs.h"
#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/game_history.h"
#include "../ent/inference_args.h"
#include "../ent/inference_results.h"
#include "../ent/rack.h"
#include "../ent/thread_control.h"
#include "../util/io_util.h"

typedef struct InferenceCtx InferenceCtx;

void inference_ctx_destroy(InferenceCtx *ctx);

void infer(InferenceArgs *args, InferenceCtx **ctx, InferenceResults *results,
           ErrorStack *error_stack);
void infer_without_ctx(InferenceArgs *args, InferenceResults *results,
                       ErrorStack *error_stack);

// Helpers used by simmed_inference.c to populate InferenceResults.
uint64_t get_number_of_draws_for_rack(const Rack *bag_as_rack,
                                      const Rack *rack);
void record_valid_leave(const Rack *rack, InferenceResults *results,
                        inference_stat_t inference_stat_type,
                        double current_leave_value,
                        uint64_t number_of_draws_for_leave);

#endif