#ifndef PEG_STRING_H
#define PEG_STRING_H

#include "../ent/game.h"
#include "../impl/peg.h"
#include <stdbool.h>

// Render a PegResult as a human-readable ranking table. When show_outcomes is
// true and result->per_scenario is populated, appends a per-scenario outcomes
// column (W/T/L per draw) for the best candidate. When poll is non-NULL and
// the solve is still running, renders a live cross-depth merged view instead of
// the stored result (pass NULL for final/show callers). Caller frees.
char *peg_result_get_string(const PegResult *result, const Game *game,
                            bool show_outcomes, PegPoll *poll);

#endif // PEG_STRING_H
