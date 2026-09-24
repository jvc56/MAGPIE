#ifndef WORD_PLUS_FLOATER_MAKER_H
#define WORD_PLUS_FLOATER_MAKER_H

#include "../ent/kwg.h"
#include "../ent/word_info_table.h"
#include "../util/io_util.h"

// Populate positional masks using the complete KWG DAWG and this WIT's
// terminal IDs, covering every length-2..4 word.
// Existing positional masks are replaced, and cleared on failure. Alphabets
// beyond machine letter 26 keep ordinary WIT only, without an error.
void make_word_plus_floater_from_kwg(const KWG *kwg, WordInfoTable *wit,
                                     ErrorStack *error_stack);

#endif
