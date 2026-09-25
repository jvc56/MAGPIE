#ifndef WIN_PCT_SMOOTHER_H
#define WIN_PCT_SMOOTHER_H

#include "../ent/win_pct_counts.h"
#include "../util/io_util.h"
#include "../util/string_util.h"

// Smooths a recorded win percentage table (see win_pct_counts.h).
//
// States with tiles in the bag change slowly with the bag size, so each bag
// size's histogram is pooled with its neighbors' under a triangular kernel
// (weight bandwidth + 1 - distance), weighted by how much data each holds.
// Small bags change quickly, so bag sizes below a floor are left alone and
// never pooled. Each bag-empty state is its own kind of endgame, so instead
// its histogram gains prior_weight pseudo-observations distributed like its
// neighboring rack-size states pooled, which matters only where data is thin.
//
// The two samples in the table come from independent games, so the parameters
// that best predict one sample from the other, smoothed, also best predict
// the truth. Both directions are scored by squared win% error over a range of
// spreads and by squared error of the mean swing, weighted by the predicted
// sample's observations. The chosen parameters smooth the pooled samples,
// returned as sample 0. The cross-validation table goes to report.
WinPctCounts *win_pct_smooth(const WinPctCounts *counts, StringBuilder *report,
                             ErrorStack *error_stack);

#endif
