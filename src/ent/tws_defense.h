#ifndef TWS_DEFENSE_H
#define TWS_DEFENSE_H

#include "../def/tws_defense_defs.h"
#include "../util/io_util.h"
#include "equity.h"
#include <stddef.h>
#include <stdint.h>

// Trained TWS defense weights ("TWD"): a small vector of penalties applied to
// static move equity based on the opponent's post-move access to triple word
// squares. Every applied weight is <= 0. This sign convention is load-bearing:
// the defense term is deliberately omitted from the shadow equity upper bound
// (see static_eval_get_shadow_equity), which is only sound for terms that can
// never increase a move's equity. The loader and setters enforce it.
typedef struct TWDWeights TWDWeights;

// Loads weights from data/strategy/<twd_name>.twd. Returns NULL and pushes to
// error_stack on failure.
TWDWeights *twd_create(const char *data_paths, const char *twd_name,
                       ErrorStack *error_stack);
// Creates a weights object with every weight set to zero, which makes the
// defense term identically zero. Used to bootstrap training.
TWDWeights *twd_create_zeroed(const char *twd_name);
void twd_destroy(TWDWeights *twd);
const char *twd_get_name(const TWDWeights *twd);
Equity twd_get_weight(const TWDWeights *twd, int feature_index);
// weight must be <= 0.
void twd_set_weight(TWDWeights *twd, int feature_index, Equity weight);
// The mutation counter changes whenever the weights are rewritten in place
// (as the training loop does between generations) so any future cache keyed
// on this object can detect staleness.
uint64_t twd_get_mutation_counter(const TWDWeights *twd);
void twd_bump_mutation_counter(TWDWeights *twd);
// Writes the weights to data/strategy/<twd_name>.twd.
void twd_write(const TWDWeights *twd, const char *data_paths,
               const char *twd_name, ErrorStack *error_stack);
// Writes the canonical name of a feature index into buf (e.g. "hook_d2",
// "float_flex_d1", "tt_floater").
void twd_feature_name(int feature_index, char *buf, size_t buf_size);

#endif
