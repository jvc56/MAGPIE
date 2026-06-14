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

// Append a finalized clock-event history entry (kind is
// TUI_HISTORY_ENTRY_TIME_PENALTY or TUI_HISTORY_ENTRY_TIME_FORFEIT).
// Mirrors the engine's GAME_EVENT_TIME_PENALTY game-history
// representation: `adjustment` is the (negative) score adjustment and
// `cumulative_after` the player's score after it. For forfeits both
// are informational (adjustment 0, cumulative = current score).
// Caller must hold state->mutex.
void tui_bot_worker_append_clock_event(TuiGameState *state, int kind,
                                       int player_idx, int adjustment,
                                       int cumulative_after);

// Start the post-game analysis-resume worker ("/resume") for history
// entry `turn_idx`: continues the turn's saved sim — accumulating
// onto the samples gathered during the game — or re-solves its
// endgame with the session's warm transposition table, streaming
// progress to the analysis panel until "/stop". Returns false (with a
// status-bar notice explaining why) when the entry can't be resumed:
// game still in progress, no saved analysis, no position snapshot, or
// a resume already running. Caller must hold state->mutex.
bool tui_analysis_worker_start(TuiGameState *state, int turn_idx);

// Interrupt the analysis worker (if any) and join its thread. The
// accumulated samples are saved back into the entry before the worker
// exits. Caller must NOT hold state->mutex — the worker takes it
// while finalizing.
void tui_analysis_worker_stop_and_join(TuiGameState *state);

// Compute and apply end-of-game overtime penalties: for each player
// who used overtime, subtract the penalty (at state->time_penalty_rate)
// from their engine score and append a TIME_PENALTY history entry.
// No-op for untimed games, the FLAG rule, non-play-vs-computer modes,
// or when penalties were already applied. Both game-ending paths (the
// human's commit and the bot's finalize) call this; the
// time_penalties_applied guard keeps it single-shot.
// Caller must hold state->mutex.
void tui_bot_worker_apply_time_penalties(TuiGameState *state);

#endif
