#ifndef PEG_STRING_H
#define PEG_STRING_H

#include "../ent/game.h"
#include "../impl/peg.h"

// Render a PegResult as a human-readable ranking table. Caller frees.
char *peg_result_get_string(const PegResult *result, const Game *game);

#endif // PEG_STRING_H
