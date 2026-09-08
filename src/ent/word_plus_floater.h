#ifndef WORD_PLUS_FLOATER_H
#define WORD_PLUS_FLOATER_H

#include "../util/io_util.h"
#include "word_info_table.h"

// Write the WIT-owned positional masks in the dense M1 format. Construction
// and loading establish row ownership; writing preserves the original IDs.
// Publish by renaming a temporary sibling so failures preserve the old file.
void word_plus_floater_write_to_file(const WordInfoTable *wit,
                                     const char *filename,
                                     ErrorStack *error_stack);

#endif
