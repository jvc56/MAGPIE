#ifndef PAT_VOL_H
#define PAT_VOL_H

#include "../ent/game.h"
#include "../ent/move.h"
#include "../ent/pat.h"
#include "../ent/win_pct.h"

// Experimental volatility rerank (see PAT_VOL_MODEL_ROW_PREFIX). Given the
// mover's equity-sorted move list, generated under policy (whose own PAT
// terms are already in each move's equity), returns the index of the move
// the rerank chooses among those within window points of the top.
//
// Mode "ce": equity - vol_scale * 0.5 * kappa * sigma2_delta, capped above
// at vol_cap, where kappa is -U''/U' of the default win/spread utility at
// the post-move margin and sigma2_delta is minus the vol model's term.
//
// Mode "eu": the move's expected utility at the vol model's horizon,
// E[U(margin + equity + noise)] with noise ~ N(0, sigma2), sigma2 the vol
// model's absolute second moment (vol_intercept minus its term), U the
// default win/spread utility with the mover on turn, integrated by
// Gauss-Hermite quadrature over the win percentage table.
//
// ctx is scratch for the vol model's evaluation context.
int pat_vol_choose(const Game *game, const MoveList *move_list,
                   const PATWeights *policy, const WinPct *win_pcts,
                   PATEvalContext *ctx, double window);

#endif
