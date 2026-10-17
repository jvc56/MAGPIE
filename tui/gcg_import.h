#ifndef TUI_GCG_IMPORT_H
#define TUI_GCG_IMPORT_H

#include "../src/ent/game_history.h"
#include "game_state.h"

// Walk a parsed GCG's events and append one TuiHistoryEntry per move so
// the History panel reflects the whole game, with per-entry pre-move
// board / rack snapshots and tile owners, then leave the live game at its
// final state with the cursor on turn 1. Caller holds state->mutex and has
// already replayed the events onto state->game.
void tui_gcg_import_history(TuiGameState *state, GameHistory *history);

#endif
