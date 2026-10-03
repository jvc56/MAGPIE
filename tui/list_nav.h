#ifndef TUI_LIST_NAV_H
#define TUI_LIST_NAV_H

#include <notcurses/notcurses.h>
#include <stdbool.h>
#include <stdint.h>

// Which keys a dialog lets tui_list_nav move focus with. Text-entry rows
// need their letters and Home/End for themselves.
typedef enum {
  TUI_LIST_NAV_ARROWS = 0,        // Up/Down, Tab/Shift-Tab
  TUI_LIST_NAV_HOME_END = 1 << 0, // Home/End jump to the first/last row
  TUI_LIST_NAV_VI = 1 << 1,       // j/k step like Down/Up
} TuiListNavKeys;

// The one row-navigation key set every dialog shares: Up/Down and
// Tab/Shift-Tab step one row, Home/End jump to the ends, and focus stops
// at either end rather than wrapping. Rows whose `enabled` entry is false
// are skipped; pass NULL when every row is enabled. Returns the new focus
// (which may equal `focus` at an end), or -1 when `key` isn't a
// navigation key.
int tui_list_nav(uint32_t key, const ncinput *input, int focus, int count,
                 const bool *enabled, unsigned keys);

#endif
