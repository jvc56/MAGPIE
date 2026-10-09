#ifndef TUI_SESSION_H
#define TUI_SESSION_H

#include "game_state.h"
#include "tui_ui_state.h"
#include <stdbool.h>

// Re-create the game state when the pending lexicon or RIT setting differs
// from the active one, keeping this session's sim plies / candidates. If the
// new lexicon fails to load, falls back to the session's current lexicon.
// *reinitialized reports whether a re-create happened. Returns false only
// when the state could not be re-created at all; the caller should quit.
// Call with the workers stopped and without holding state->mutex.
bool tui_reinit_game_state_if_needed(TuiGameState *state, TuiSession *session,
                                     bool *reinitialized);

#endif
