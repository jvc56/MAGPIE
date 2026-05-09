#ifndef TUI_TIME_PICKER_H
#define TUI_TIME_PICKER_H

#include <notcurses/notcurses.h>
#include "theme.h"

// Renders a time-control picker with fixed presets (per-side total).
// Returns the chosen seconds-per-side, or -1 if the user cancels.
// `initial_seconds` selects the focused preset on entry; pass 0 to
// focus a sensible default.
int tui_time_picker_run(struct notcurses *nc, const Theme *theme,
                        int initial_seconds);

#endif
