#ifndef TUI_BOT_WORKER_H
#define TUI_BOT_WORKER_H

#include "game_state.h"

// Spawn a worker thread that plays both sides via static-eval equity
// move generation, with a 3-second pause between turns. Logs each move
// into state->history under state->mutex.
//
// Idempotent: a no-op if the worker is already running. Stop the worker
// (and release the thread) by calling tui_game_state_destroy.
void tui_bot_worker_start(TuiGameState *state);

// Spawn the pixel-worker thread that rasterizes the 2x board RGBA
// composite off the UI thread. Idempotent. The thread is joined by
// tui_game_state_destroy.
void tui_pixel_worker_start(TuiGameState *state);

// Append a pending history entry for the given player, snapshotting
// the board + racks + clocks at the moment of decision. Used both
// internally by the bot worker (at the start of each turn) and by
// main.c after Load-position completes — once a position is in
// place, we want a pending entry visible so the History panel
// reads "1. ..." waiting for the next move. Returns the new
// entry's index. Caller must hold state->mutex.
struct Rack;
int tui_bot_worker_append_pending_history(TuiGameState *state, int player_idx,
                                          const struct Rack *rack_at_start,
                                          int clock_at_start);

#endif
