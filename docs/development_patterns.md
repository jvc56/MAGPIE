# Development Guidelines & Patterns

This document captures successful patterns and workflows established during the development of MAGPIE, particularly for the Qt frontend and C bridge integration.

## Core Philosophies

### 1. Verify Requirements First
*   **Ask before assuming:** If a requirement (like how "Unseen" tiles should be counted) is ambiguous, ask the user for clarification *before* writing code.
*   **Use precise terminology:** Distinguish between "Unseen by Player" (Bag + Opponent Rack) vs "Unseen by Engine" (Bag only).

### 2. Diagnosis over Speculation
*   **Deep Dive First:** Use tools like `grep`, `glob`, and `read_file` to build a complete mental model of the relevant code before attempting a fix.
*   **Verify Assumptions:** Don't assume a function does what its name suggests. Check the implementation (e.g., `bridge_game_play_to_index` resetting the game vs re-creating it).
*   **Isolate Variables:** When debugging complex state (like `GameHistoryModel`), try to isolate the backend logic (Bridge/C) from the frontend presentation (QML/C++ Model).

## Architectural Patterns

### The Bridge Pattern (C to C++)
*   **Keep C pure:** `src/qt/bridge` should handle all C-struct manipulation and memory management. It should return simple types (int, string, char*) to C++.
*   **Opaque Handles:** Use opaque pointers (`BridgeGame*`, `BridgeGameHistory*`) to maintain encapsulation.
*   **Invariant Checking:** When calculating derived state (like "Bag Count" or "Unseen Count"), verify it against known invariants (e.g., Total Tiles = Board + Rack + Bag + Opponent Rack). This handles edge cases like annotated games with incomplete info.

### Qt/QML Models
*   **Pre-computation:** Calculate complex display logic (like formatting scores or determining valid move ranges) in the C++ Model (`GameHistoryModel`), not in QML.
*   **Property Binding:** Expose granular properties (`bagCount`, `blankCount`, `vowelCount`) rather than pre-formatted strings where possible, allowing QML to handle the final presentation.

## Debugging Workflows
*   **Console is King:** When values look wrong in the UI, use `printf` in the C bridge or `qDebug()` in C++ to verify the raw data flow.
*   **Reproducible Cases:** Use specific game files (like `anno54597.gcg`) that demonstrate the issue to verify fixes.

## Git & Version Control
*   **Atomic Commits:** Group related changes (e.g., "Fix bag count logic" vs "Update UI formatting") when possible.
*   **Verify before Commit:** Ensure the build passes (`./build_qtpie.sh`) before committing.
