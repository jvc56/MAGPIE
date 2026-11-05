#include "list_nav.h"

#include <notcurses/notcurses.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// First enabled row from `start` stepping by `dir`, or -1 if none.
static int first_enabled(const bool *enabled, int count, int start, int dir) {
  for (int row_idx = start; row_idx >= 0 && row_idx < count; row_idx += dir) {
    if (enabled == NULL || enabled[row_idx]) {
      return row_idx;
    }
  }
  return -1;
}

int tui_list_nav(uint32_t key, const ncinput *input, int focus, int count,
                 const bool *enabled, unsigned keys) {
  const bool vi = (keys & TUI_LIST_NAV_VI) != 0;
  const bool tab = key == NCKEY_TAB || key == '\t';
  const bool shift_tab = tab && ncinput_shift_p(input);
  int dir = 0;
  if (key == NCKEY_UP || shift_tab || (vi && (key == 'k' || key == 'K'))) {
    dir = -1;
  } else if (key == NCKEY_DOWN || tab || (vi && (key == 'j' || key == 'J'))) {
    dir = 1;
  }
  if (dir != 0) {
    const int next = first_enabled(enabled, count, focus + dir, dir);
    return next < 0 ? focus : next;
  }
  if ((keys & TUI_LIST_NAV_HOME_END) != 0 &&
      (key == NCKEY_HOME || key == NCKEY_END)) {
    const int next = key == NCKEY_HOME
                         ? first_enabled(enabled, count, 0, 1)
                         : first_enabled(enabled, count, count - 1, -1);
    return next < 0 ? focus : next;
  }
  return -1;
}
