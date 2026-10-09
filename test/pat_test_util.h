#ifndef PAT_TEST_UTIL_H
#define PAT_TEST_UTIL_H

#include "../src/ent/game.h"
#include "../src/ent/pat.h"

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

#endif
