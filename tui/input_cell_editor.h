#ifndef TUI_INPUT_CELL_EDITOR_H
#define TUI_INPUT_CELL_EDITOR_H

#include "game_state.h"
#include <notcurses/notcurses.h>
#include <stdbool.h>
#include <stdint.h>

// Key handling while a History cell editor (or its board move-entry
// sub-mode) is open. Returns true when the key was consumed.
bool tui_input_cell_editor(TuiGameState *state, uint32_t key, ncinput input);

// The annotation gesture a phony confirmation holds back, resumed when
// the annotator keeps the play (state->phony_confirm_action).
typedef enum {
  TUI_PHONY_ACTION_ENTER = 0, // Enter on the turn's RACK or LEAVE
  TUI_PHONY_ACTION_RIGHT = 1, // Right past the turn's LEAVE
  TUI_PHONY_ACTION_BOARD = 2, // Enter in board move entry
} TuiPhonyAction;

// Answers the pending phony confirmation: `keep` resumes the held-back
// commit with the phonies; otherwise the editor returns to the move.
void tui_cell_editor_phony_resolve(TuiGameState *state, bool keep);

#endif
