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

// What the analysis commands ("/sim", "/resume", "/kibitz", "/stop")
// and the analysis panel's menu can do.
typedef enum {
  TUI_ANALYSIS_SIM,
  TUI_ANALYSIS_RESUME,
  TUI_ANALYSIS_KIBITZ,
  TUI_ANALYSIS_STOP,
} TuiAnalysisAction;

// NULL when `action` can run on history entry `turn_idx` now; otherwise
// a short reason it can't, for a notice or a disabled menu item's help.
// Caller holds state->mutex.
const char *tui_analysis_unavailable_reason(const TuiGameState *state,
                                            int turn_idx,
                                            TuiAnalysisAction action);

// Start the analysis worker on history entry `turn_idx`, streaming
// progress to the analysis panel until "/stop".
//   request_sim (true, "/sim"): simulate the turn, continuing its saved
//     sim when it has one, else simming its top candidates plus the
//     played move from scratch. Works on loaded and annotated games,
//     not while a computer game is still in progress.
//   request_sim (false, "/resume"): continue the turn's saved analysis
//     from the game — its sim, or its endgame with the session's warm
//     transposition table — once the game is over.
// Returns false (with a status-bar notice explaining why) when the
// entry can't be analyzed. Caller must hold state->mutex.
bool tui_analysis_worker_start(TuiGameState *state, int turn_idx,
                               bool request_sim);

// "/kibitz": rank the moves available at turn `turn_idx` (the position
// before it was played) by static equity and store them as that turn's
// analysis snapshot, marking where the played move ranks. Synchronous
// (movegen only). Caller holds state->mutex. Returns false and posts a
// notice when the turn can't be analyzed.
bool tui_analysis_kibitz(TuiGameState *state, int turn_idx);

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

// Stop the analysis-resume worker and the bot thread (if running) before
// the game is reset or reconfigured, and leave bot_stop cleared for the
// next bot run. The analysis-resume worker reads history entries and the
// endgame ctx, so it must be stopped first. Joins threads, so call it
// without holding state->mutex.
void tui_stop_workers(TuiGameState *state);

#endif
