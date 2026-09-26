#ifndef TUI_GCG_EXPORT_H
#define TUI_GCG_EXPORT_H

#include "game_state.h"
#include <stdbool.h>
#include <stddef.h>

// The game's history as GCG text (heap-allocated; caller frees): the
// lexicon and players, then one line per committed turn, with
// challenged-off phonies, challenge bonuses, time penalties, and the
// going-out bonus as their own GCG events. A turn with no recorded rack
// lists just the tiles it played. Caller holds state->mutex.
char *tui_gcg_export(const TuiGameState *state);

// Writes the GCG to `path`. Returns false when it can't be written.
// Caller holds state->mutex.
bool tui_gcg_save(const TuiGameState *state, const char *path);

// A fresh file name for a save with no name given:
// "magpie-YYYYMMDD-HHMMSS.gcg", in the working directory.
void tui_gcg_default_path(char *out, size_t out_size);

#endif
