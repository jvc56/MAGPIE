#ifndef PAT_INFERENCE_H
#define PAT_INFERENCE_H

#include "../ent/equity.h"
#include "../ent/game.h"
#include "../ent/move.h"
#include "../ent/rack.h"
#include "../util/io_util.h"
#include <stdbool.h>
#include <stdint.h>

// A fixed board/model/score/bag position; only the on-turn rack may change.
// The caller owns game and keeps it alive until this context is destroyed.
// Board, observer rack, bag, scores, lexicon and loaded PAT weights/settings
// must stay fixed. Candidate racks must fit the remaining tile population.
// Uses the loaded PAT model (even if normal play has PAT disabled), honoring
// its disabled-class mask. Each worker needs its own context.
typedef struct PATInference PATInference;
typedef enum {
  PAT_INFERENCE_EXACT,
  PAT_INFERENCE_ZERO_REGRET,
  PAT_INFERENCE_REJECTED,
} pat_inference_resolution_t;
typedef struct PATInferenceEvaluation {
  Equity observed;
  // Exact best equity for EXACT; observed for ZERO_REGRET; a lower bound
  // strictly above observed + margin for REJECTED. Never treat a rejected
  // bound as an exact regret when using a wider margin.
  Equity best;
  pat_inference_resolution_t resolution;
} PATInferenceEvaluation;
typedef struct PATInferenceStats {
  uint64_t racks;
  uint64_t exact_searches;
  uint64_t zero_regrets;
  uint64_t rejected;
} PATInferenceStats;

PATInference *pat_inference_create(Game *game, bool use_census,
                                   ErrorStack *errors);
void pat_inference_destroy(PATInference *ctx);
// observed must be a legal placement from the current candidate rack and
// must be the same move for every evaluation with this context.
PATInferenceEvaluation pat_inference_evaluate(PATInference *ctx,
                                              const Move *observed,
                                              const Rack *leave, Equity margin);
PATInferenceStats pat_inference_get_stats(const PATInference *ctx);
#endif
