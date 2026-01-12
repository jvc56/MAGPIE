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

void inference_reset_cutoff_stats(void);
void inference_get_cutoff_stats(uint64_t *total_calls, uint64_t *triggered);
void inference_print_cutoff_stats(void);

#endif