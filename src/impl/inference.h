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

void infer_args_fill(InferenceArgs *args, int num_plays, Equity eq_margin,
                     GameHistory *game_history, const Game *game,
                     int num_threads, int print_interval,
                     ThreadControl *thread_control, bool use_game_history,
                     int target_index, Equity target_score, int target_num_exch,
                     Rack *target_played_tiles, Rack *target_known_rack,
                     Rack *nontarget_known_rack);

void inference_ctx_destroy(InferenceCtx *ctx);

void infer(InferenceArgs *args, InferenceCtx **ctx, InferenceResults *results,
           ErrorStack *error_stack);
void infer_without_ctx(InferenceArgs *args, InferenceResults *results,
                       ErrorStack *error_stack);
#endif