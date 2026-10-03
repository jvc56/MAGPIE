#ifndef TUI_TIME_PICKER_H
#define TUI_TIME_PICKER_H

#include "theme.h"
#include <notcurses/notcurses.h>

// Renders a time-control picker with fixed presets (per-side total).
// Returns the chosen seconds-per-side, or -1 if the user cancels.
// `initial_seconds` selects the focused preset on entry; pass 0 to
// focus a sensible default. Full-screen variant — used by the
// first-run onboarding flow. The in-game "New game" path renders a
// modal version via tui_game_render_time_picker instead.
int tui_time_picker_run(struct notcurses *nc, const Theme *theme,
                        int initial_seconds);

// Preset accessors for the modal version of the picker.
int tui_time_picker_preset_count(void);
int tui_time_picker_preset_seconds(int idx);
const char *tui_time_picker_preset_label(int idx);
const char *tui_time_picker_preset_blurb(int idx);
// Index of the preset closest to `initial_seconds`, or the default
// preset index if `initial_seconds <= 0`.
int tui_time_picker_closest_index(int initial_seconds);

#endif
