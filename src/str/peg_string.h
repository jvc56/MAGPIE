#ifndef PEG_STRING_H
#define PEG_STRING_H

#include "../ent/game.h"
#include "../impl/peg.h"
#include <stdbool.h>

// Render a PegResult as a human-readable ranking table. When show_outcomes is
// true, the graded table gets a per-play outcomes column (W/L draws). When poll
// is non-NULL and the solve is still running, renders a live cross-depth merged
// view instead of the stored result (pass NULL for final/show callers).
//
// Outcomes-column wrapping: out_width is the maximum whole-line width (<= 0 =
// unbounded, no wrapping); the column wraps at token boundaries, indented under
// its header. out_max_lines caps the wrapped lines per cell (0 = unlimited);
// truncated cells end with "...". When non-NULL, trunc_note is printed just
// above the "weighted bag orderings" line (used to point at the file holding
// the full chart). When out_truncated is non-NULL, *out_truncated is set true
// if any cell was truncated. Caller frees the returned string.
char *peg_result_get_string(const PegResult *result, const Game *game,
                            bool show_outcomes, PegPoll *poll, int out_width,
                            int out_max_lines, const char *trunc_note,
                            bool *out_truncated);

// Render one candidate's per-ordering W/L/T rows as the compact outcomes-cell
// string (e.g. "W: DH/Ax2 DR/Hx2"): the mover's drawn tiles as a sorted
// multiset prefix, then the bag remainder (a multiset, or "/"-segmented when
// its orderings split across buckets). Only the shorter of the win/loss lists
// is shown; the per-token "xN" weights sum to the win/loss columns. Exposed for
// unit testing; production reaches it via peg_result_get_string. Caller frees.
char *peg_build_outcomes_string_rows(const PegPerScenario *rows, int n_rows);

#endif // PEG_STRING_H
