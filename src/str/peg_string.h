#ifndef PEG_STRING_H
#define PEG_STRING_H

#include "../ent/game.h"
#include "../impl/peg.h"
#include <stdbool.h>

// Render a PegResult as a human-readable ranking table. When show_outcomes is
// true and result->per_scenario is populated, appends a per-scenario outcomes
// column (W/T/L per draw) for the best candidate. Caller frees.
char *peg_result_get_string(const PegResult *result, const Game *game,
                            bool show_outcomes);

// Render the live candidate ranking (no stage table) — used to stream the
// evolving ranking during a halving stage. Columns are depth/rank/move/win%/
// spread/time, numbers right-aligned; depth is the stage's
// "<fidelity_plies>-ply" label so each chart shows which stage it is. The move
// column is sized to fit every move in stage_moves[0..n_stage_moves), and the
// numeric columns from all current entries, so widths stay stable as candidates
// land. When only_last_row is set, just the final (worst) entry's row is
// emitted (to append a new bottom row); otherwise the header and every row are
// emitted. Caller frees.
char *peg_result_get_ranking_string(const PegResult *result, const Game *game,
                                    const Move *stage_moves, int n_stage_moves,
                                    int fidelity_plies, bool only_last_row);

#endif // PEG_STRING_H
