#ifndef TUI_RESIZE_H
#define TUI_RESIZE_H

#include <notcurses/notcurses.h>
#include <stdbool.h>

// Resize 'plane' to match the kernel-reported terminal size if they
// disagree. Use this at the top of every render function: notcurses'
// own terminal-dim tracking can lag the actual terminal on macOS, and
// rendering past the visible bottom either scrolls the alt screen
// (Terminal.app) or smears the previous frame across columns (ghostty).
//
// Returns true when a resize happened — callers can use this to force
// a full redraw of every cell so notcurses' diff cache doesn't leave
// stale content visible (Terminal.app especially is fragile here).
bool tui_sync_plane_to_terminal(struct ncplane *plane);

#endif
