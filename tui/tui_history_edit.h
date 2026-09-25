#ifndef TUI_TUI_HISTORY_EDIT_H
#define TUI_TUI_HISTORY_EDIT_H

#include "game_state.h"

void tui_commit_edit_and_revalidate(TuiGameState *gs);

// Whether the history-cell editor may open on entry `idx`, from the
// keyboard or a mouse double-click. Play-vs-computer edits only the
// human's live pending turn (the cell is its keyboard move-entry
// surface); reopening a committed turn would replay history from text
// and desync the bag-drawn racks.
bool tui_history_entry_editable(const TuiGameState *state, int idx);

#endif
