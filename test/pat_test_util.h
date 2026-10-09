#ifndef PAT_TEST_UTIL_H
#define PAT_TEST_UTIL_H

#include "../src/ent/equity.h"
#include "../src/ent/game.h"
#include "../src/ent/move.h"
#include "../src/ent/pat.h"
#include "../src/ent/pat_eval.h"
#include "../src/ent/rack.h"

// Creates a temporary data directory with a strategy/ subdirectory and
// returns the path (owned by the caller).
char *create_temp_pat_data_dir(void);
// Writes contents to <data_dir>/strategy/<pat_name>.pat.
void write_pat_file_contents(const char *data_dir, const char *pat_name,
                             const char *contents);
// A zeroed PATWeights with its lexicon tables prepared from the game's
// data, ready for evaluation (an unprepared model is refused; see
// PATWeights.prepared).
PATWeights *pat_test_create_prepared(const char *name, const Game *game);
// Asserts pat_eval_move_penalty_capped's contract against the exact term
// at floors around it: the exact term when that reaches the floor,
// otherwise a value from the exact term up to but below the floor.
void pat_test_assert_capped_penalty(const PATEvalContext *pat_eval_ctx,
                                    const Move *move, const Rack *leave,
                                    Equity exact);

#endif
