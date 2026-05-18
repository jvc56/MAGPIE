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

#endif
