# Common Pitfalls & Architectural Notes

This document outlines common pitfalls, architectural quirks, and non-obvious behaviors in the MAGPIE codebase.

## QT Frontend

### GameHistoryModel State Synchronization

**The Issue:**
The `GameHistoryModel` (`src/qt/models/GameHistoryModel.cpp`) constructs the history list by replaying game events. A common point of confusion—and a source of bugs—is the timing of state capture for `HistoryItem` properties (specifically `unseenTiles`, `bagCount`, `vowelCount`, and `consonantCount`).

**Behavior:**
In `GameHistoryModel::updateHistory()`, the code iterates through game events. Currently, it captures the tile tracking state **BEFORE** the event is applied to the game board.
*   **Code Location:** `src/qt/models/GameHistoryModel.cpp` inside `updateHistory`.
*   **Consequence:** The "Tile Tracking" widget in the UI (which binds to these properties) displays the "Pre-move" state (e.g., tiles remaining *before* the player made their move and drew new tiles). The Board and Rack views, however, typically display the "Post-move" state (because the game is played forward to that index).
*   **Visual Discrepancy:** This results in a mismatch where the Board shows the move played, but the Tile Tracking pane shows the counts as if the move hasn't happened yet (incorrectly high counts for tiles that were just played/drawn).

**Diagnosis of Previous Failures:**
Attempts to fix this by simply moving the capture logic can be tricky due to the loop structure in `updateHistory`. The loop processes events and decides when to "flush" a `HistoryItem` (handling merged turns). 
*   `m_game` state advances *during* the loop.
*   When `flush()` is called at the start of a new turn iteration, `m_game` is already in the **Post-move** state of the *previous* turn.
*   **Correct Fix Strategy:** To show Post-move stats, the capture must happen **just before** `flush()` is called, using the current `m_game` state (which represents the state after the turn logic has executed), rather than capturing it at the initialization of the `current` item.

### Bridge & Data Types

**Memory Management:**
*   Strings returned by `bridge_*` functions (char*) are often malloc'd and must be `free()`'d by the C++ caller. Failure to do so leads to leaks.
*   `bridge_game_create_from_history` relies on `dataPath`. Ensure the path is correctly resolved relative to the application bundle or executable.

**Index alignment:**
*   Bridge indices are 0-based.
*   `bridge_game_play_to_index(..., i)` usually means "play until state `i` is reached", or "apply `i` events". Verify exact semantics in `src/bridge/` or `src/wasmentry/api.c`.
