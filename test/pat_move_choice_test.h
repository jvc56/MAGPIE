#ifndef PAT_MOVE_CHOICE_TEST_H
#define PAT_MOVE_CHOICE_TEST_H

#include "../src/ent/pat.h"
#include "../src/impl/config.h"
#include <stdint.h>

// One side of a paired move-choice comparison: how the mover picks a move
// at a position. pat is the weights move generation runs under;
// degrade_margin > 0 deliberately picks the best move at least that far
// below the top (a degradation control); overlap_correction != 0 reranks
// the exhaustive list by equity - correction * narrow_overlap(resulting
// board) (see pat_overlap_narrow_measure).
typedef struct PATMoveChooser {
  const char *label;
  const PATWeights *pat;
  double degrade_margin;
  double overlap_correction;
} PATMoveChooser;

typedef struct PATMoveChoiceResult {
  int positions_considered;
  int disagreements;
  double mean;
  double se;
  // Decomposition of the paired world differences d_ir (position i, world
  // r): within is the mean across positions of the sample variance across
  // worlds; between is var(position means) - within / R.
  double within_variance;
  double between_variance;
} PATMoveChoiceResult;

// The champion's own equity player config, with the board loaded.
Config *pat_move_choice_config_create(void);

// num_worlds paired reference worlds per disagreement (at most
// PAT_MOVE_CHOICE_MAX_WORLDS).
void pat_move_choice_compare(Config *config, const PATMoveChooser *baseline,
                             const PATMoveChooser *candidate,
                             uint64_t seed_base, int num_positions,
                             int num_worlds, PATMoveChoiceResult *result_out);

#define PAT_MOVE_CHOICE_MAX_WORLDS 300
// What every comparison used before the decomposition was measured.
#define PAT_MOVE_CHOICE_DEFAULT_WORLDS 30

void pat_move_choice_compare_shard(Config *config,
                                   const PATMoveChooser *baseline,
                                   const PATMoveChooser *candidate,
                                   uint64_t seed_base, int num_positions,
                                   int num_worlds, int shard, int num_shards,
                                   PATMoveChoiceResult *result_out);
void pat_move_choice_run_spec(const char *spec);

void test_pat_move_choice_controls(void);
void test_pat_move_choice_targeted_controls(void);
void test_pat_train_runtime_parity(void);
void test_pat_overlap_step3_dev(void);
void test_pat_overlap_step3_confirm(void);

#endif
