#ifndef TWS_DEFENSE_H
#define TWS_DEFENSE_H

#include "../def/tws_defense_defs.h"
#include "../util/io_util.h"
#include "board.h"
#include "equity.h"
#include "kwg.h"
#include "letter_distribution.h"
#include "move.h"
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
// Builds the per-letter flexibility table (the number of two-letter words
// containing each letter) from the lexicon. Used to approximate the hook
// and extension flexibility of squares whose real cross and extension sets
// do not exist yet because they are created by the move being evaluated.
// Must be called before the weights are used for evaluation; kwg may be
// NULL, which zeroes the table.
void twd_prepare_hook_flex(TWDWeights *twd, const KWG *kwg,
                           const LetterDistribution *ld);
// Returns the flexibility table entry for an (unblanked) machine letter.
int twd_get_hook_flex(const TWDWeights *twd, MachineLetter ml);

enum {
  // Standard boards have 8 TWS squares; exotic layouts get headroom. A
  // board with more uncovered TWS than this is deterministically truncated
  // to the first TWD_MAX_TWS in row-major order, identically in training
  // and evaluation.
  TWD_MAX_TWS = 32,
  TWD_MAX_SCAN_UNITS = TWD_MAX_TWS * 2,
};

// Per-position evaluation state, rebuilt by each movegen position load (and
// on the stack for validated moves). Holds the position-constant part of the
// defense term (pre_penalty, the penalty for the opponent's TWS access on
// the board as it stands) plus what the per-move delta needs to stay off
// the hot path: per scan unit (one TWS row or column walk), the baseline
// feature vector and the extent of squares the walk actually visited, so a
// candidate move rescans a unit only when it places a tile on or directly
// beside a square that walk could see, and rescans it exactly once (the
// baseline side is cached).
//
// The lanes pointer is only valid while the board is alive, untransposed,
// and unmutated, which holds for the duration of a move generation call and
// for a stack-scoped validated-move evaluation.
typedef struct TWDEvalContext {
  // NULL means the context is disabled and the defense term is zero.
  const TWDWeights *weights;
  const LetterDistribution *ld;
  const Square *lanes;
  Equity pre_penalty;
  int num_tws;
  uint8_t tws_rows[TWD_MAX_TWS];
  uint8_t tws_cols[TWD_MAX_TWS];
  // Units 2*i and 2*i+1 are TWS i's horizontal and vertical walks.
  int32_t unit_features[TWD_MAX_SCAN_UNITS][TWD_NUM_FEATURES];
  // Each unit's baseline contribution to pre_penalty (always <= 0), used
  // to bound a move's penalty from above without rescanning.
  Equity unit_penalty[TWD_MAX_SCAN_UNITS];
  // Bit u of unit_mask_by_row[r] is set when a fresh tile in row r could
  // affect unit u provided the move's column span also overlaps the unit
  // (and symmetrically for columns), so a candidate move's affected-unit
  // set is the AND of the OR of its rows' masks with the OR of its
  // columns' masks: a few operations per candidate on the movegen record
  // path, zero for the vast majority of moves.
  uint64_t unit_mask_by_row[BOARD_DIM];
  uint64_t unit_mask_by_col[BOARD_DIM];
} TWDEvalContext;

void twd_eval_context_disable(TWDEvalContext *twd_eval_ctx);
void twd_eval_context_load(TWDEvalContext *twd_eval_ctx,
                           const TWDWeights *weights, const Square *lanes,
                           const LetterDistribution *ld);
// Returns the defense term for the move: the penalty for the opponent's TWS
// access after the move is played, which is always <= 0. Returns 0 when the
// context is NULL or disabled. Non-placement moves return the position
// baseline (their play leaves the board unchanged).
Equity twd_eval_move_penalty(const TWDEvalContext *twd_eval_ctx,
                             const Move *move);
// Returns an upper bound on twd_eval_move_penalty for the move without any
// lane rescans: the baseline penalty minus the baseline contributions of
// the units the move can affect (each of which the move can at best zero
// out). Used to skip the exact computation for moves that cannot contend.
Equity twd_eval_move_penalty_bound(const TWDEvalContext *twd_eval_ctx,
                                   const Move *move);
// Extracts the feature vector for the board as it stands (no move overlay).
// Used for the context baseline and, exactly as-is, by the training loop on
// post-move boards. features must have TWD_NUM_FEATURES elements.
void twd_extract_features(const Square *lanes, const LetterDistribution *ld,
                          int32_t *features);

#endif
