#ifndef PEG_STRING_H
#define PEG_STRING_H

#include "../ent/board.h"
#include "../ent/game.h"
#include "../ent/letter_distribution.h"
#include "../impl/peg.h"
#include <stdbool.h>

// Render a PegResult as a human-readable ranking table. When show_outcomes is
// true, the graded table gets a per-play outcomes column (W/L/T draws). When
// poll
// is non-NULL and the solve is still running, renders a live cross-depth merged
// view instead of the stored result (pass NULL for final/show callers).
//
// Outcomes-column wrapping: out_width is the maximum whole-line width (<= 0 =
// unbounded, no wrapping); the column wraps at token boundaries, indented under
// its header. out_max_lines caps the wrapped lines per cell (0 = unlimited);
// truncated cells end with "...". When non-NULL, trunc_note is printed just
// above the ranking (used to point at the file holding the full chart). When
// out_truncated is non-NULL, *out_truncated is set true if any cell was
// truncated.
//
// A finished result lists the ranking's first max_rows rows (0 = every play),
// then up to PEG_DISPLAY_MAX_PINNED_ROWS solved plays ranked below them, as
// status lists a running one's. Caller frees the returned string.
char *peg_result_get_string(const PegResult *result, const Game *game,
                            bool show_outcomes, PegPoll *poll, int max_rows,
                            int out_width, int out_max_lines,
                            const char *trunc_note, bool *out_truncated);

// Render one candidate's per-ordering W/L/T rows as the compact outcomes-cell
// string (e.g. "W: DH/Ax2 DR/Hx2" or "W: C U Y, T: E"): the mover's drawn tiles
// as a sorted multiset prefix, then the bag remainder (a multiset, or
// "/"-segmented when its orderings split across buckets). The largest of the
// win/loss/tie lists is left implied (inferred from the count columns) and the
// smaller ones are printed comma-separated with W:/L:/T: labels; the implied
// list is named as ", otherwise wins/loses/ties" only when the cell is
// otherwise ambiguous (ties are the implied majority, or the tie list is the
// only one shown). The per-token "xN" weights sum to the count columns. Exposed
// for unit testing; production reaches it via peg_result_get_string. Caller
// frees.
char *peg_build_outcomes_string_rows(const PegPerScenario *rows, int n_rows);

// Caller frees the returned JSON string.
char *peg_snapshot_get_json(const PegPollSnapshot *snapshot, const Game *game);
// The live view of a running solve from a snapshot, as status shows it at
// monotonic time `now_ns`. With `outcomes` (NULL for none), the ranking has an
// outcomes column from those `n_outcomes` candidates' draws.
char *peg_snapshot_get_status_string(const PegPollSnapshot *snapshot,
                                     const Game *game, int64_t now_ns,
                                     PegCandOutcomes *outcomes, int n_outcomes);
// Read a synchronized live snapshot and serialize it.
char *peg_poll_outcomes_json(PegPoll *poll, const Board *board,
                             const LetterDistribution *ld,
                             const char *move_text);

char *peg_poll_get_json(PegPoll *poll, const Game *game);

#endif // PEG_STRING_H
