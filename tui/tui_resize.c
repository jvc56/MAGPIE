#include "tui_resize.h"

#include <notcurses/notcurses.h>
#include <sys/ioctl.h>
#include <unistd.h>

bool tui_sync_plane_to_terminal(struct ncplane *plane) {
  if (plane == NULL) {
    return false;
  }
  struct winsize ws = {0};
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0 || ws.ws_row == 0 ||
      ws.ws_col == 0) {
    return false;
  }
  // Edge-triggered on the terminal's reported size. Only act when the
  // ioctl winsize actually CHANGES from the previous poll — not whenever
  // it merely disagrees with notcurses' plane size.
  //
  // Some terminals report an ioctl winsize that persistently differs from
  // the size notcurses clamps the std plane to (pixel-cell rounding, or
  // Ghostty after a font tweak). A level-triggered "ioctl != plane ->
  // refresh + resize" check then fires notcurses_refresh on EVERY frame —
  // a full-screen redraw that re-emits all ~225 board sprixels (~250ms),
  // which is exactly the 4fps input-lag stall. notcurses already clamped
  // the plane for a reason, so forcing it back every frame is both futile
  // (it re-clamps) and ruinously expensive. Edge-triggering collapses this
  // to a single refresh when the user genuinely resizes; the steady-state
  // disagreement is left alone. Genuine resizes that DO emit NCKEY_RESIZE
  // are handled separately in the input loop; this is the backstop for
  // terminals that resize without sending that event.
  static unsigned prev_ws_row = 0;
  static unsigned prev_ws_col = 0;
  const bool first_poll = prev_ws_row == 0 && prev_ws_col == 0;
  const bool ws_changed = ws.ws_row != prev_ws_row || ws.ws_col != prev_ws_col;
  prev_ws_row = ws.ws_row;
  prev_ws_col = ws.ws_col;
  if (first_poll || !ws_changed) {
    return false;
  }
  unsigned plane_rows = 0;
  unsigned plane_cols = 0;
  ncplane_dim_yx(plane, &plane_rows, &plane_cols);
  if (plane_rows == ws.ws_row && plane_cols == ws.ws_col) {
    return false;
  }
  struct notcurses *nc = ncplane_notcurses(plane);
  if (nc != NULL) {
    notcurses_refresh(nc, NULL, NULL);
  }
  ncplane_resize_simple(plane, ws.ws_row, ws.ws_col);
  return true;
}
