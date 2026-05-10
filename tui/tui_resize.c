#include "tui_resize.h"

#include <sys/ioctl.h>
#include <unistd.h>
#include <notcurses/notcurses.h>

bool tui_sync_plane_to_terminal(struct ncplane *plane) {
  if (plane == NULL) {
    return false;
  }
  struct winsize ws = {0};
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0 || ws.ws_row == 0 ||
      ws.ws_col == 0) {
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
