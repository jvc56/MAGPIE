#ifndef TUI_ANALYSIS_ROWS_H
#define TUI_ANALYSIS_ROWS_H

#include "../src/ent/game.h"
#include "game_state.h"
#include <stdbool.h>

const Game *analysis_source_game(const TuiGameState *state);
void populate_frame_analysis_rows(TuiGameState *state);
bool resume_active_for_cursor(const TuiGameState *state);
// The History cursor's turn when the Analysis panel shows the analysis
// saved on it rather than live results: a played turn, or in annotation
// a turn still being entered, analyzed from its rack. NULL when the turn
// has none saved or its analysis is running.
const TuiHistoryEntry *cursor_saved_analysis_entry(const TuiGameState *state);
// Populate a snapshot of the Analysis-panel contents for the
// currently-active sim or endgame solve. Called by the bot worker
// at finalize time (just after a move is chosen, just before
// play_move advances the board) so each history entry can
// preserve the analysis the user was looking at when the bot
// committed. Picks sim vs endgame the same way the live render
// does: endgame when the bag is empty and the endgame snapshot
// is valid, sim otherwise. Safe to call from any thread that
// already holds state->mutex.
void tui_capture_analysis_snapshot(const TuiGameState *state,
                                   TuiAnalysisSnapshot *out);

#endif
