#ifndef PAT_FILE_H
#define PAT_FILE_H

#include "../util/io_util.h"
#include "pat.h"

// Reading and writing PAT weight files (see PAT_MAGIC_PREFIX and the row
// prefixes in pat_defs.h).

// Loads weights from data/strategy/<pat_name>.pat. Returns NULL and pushes to
// error_stack on failure.
PATWeights *pat_create(const char *data_paths, const char *pat_name,
                       ErrorStack *error_stack);
// Writes the weights to data/strategy/<pat_name>.pat.
void pat_write(const PATWeights *pat, const char *data_paths,
               const char *pat_name, ErrorStack *error_stack);

#endif
