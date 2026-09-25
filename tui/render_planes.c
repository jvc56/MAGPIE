#include "render_planes.h"

#include "frame_dump.h"
#include <stdint.h>

static TuiGridPlanes grid_planes;

void tui_plane_destroy(struct ncplane *plane) {
  tui_frame_dump_forget(plane);
  ncplane_destroy(plane);
}

TuiGridPlanes *tui_grid_planes(void) { return &grid_planes; }

void tui_planes_destroy_all(void) {
  if (grid_planes.board != NULL) {
    tui_plane_destroy(grid_planes.board);
    grid_planes.board = NULL;
  }
  if (grid_planes.rack != NULL) {
    tui_plane_destroy(grid_planes.rack);
    grid_planes.rack = NULL;
  }
  if (grid_planes.labels_col != NULL) {
    tui_plane_destroy(grid_planes.labels_col);
    grid_planes.labels_col = NULL;
  }
  if (grid_planes.labels_row != NULL) {
    tui_plane_destroy(grid_planes.labels_row);
    grid_planes.labels_row = NULL;
  }
  if (grid_planes.modal != NULL) {
    tui_plane_destroy(grid_planes.modal);
    grid_planes.modal = NULL;
  }
}

// Public accessor for the cached modal plane, shared across all modal
// renderers (menu / settings / time picker / lexicon picker). Creates
// the plane on first use; resizes/repositions on subsequent calls.
// Returns NULL on allocation failure. The plane is destroyed when
// tui_game_render sees modal == TUI_MODAL_NONE, so callers don't
// have to manage its lifetime.
struct ncplane *
tui_game_render_get_or_create_modal_plane(struct ncplane *parent, int top,
                                          int left, int rows, int cols) {
  if (grid_planes.modal == NULL) {
    ncplane_options opts = {0};
    opts.y = top;
    opts.x = left;
    opts.rows = (unsigned)rows;
    opts.cols = (unsigned)cols;
    opts.name = "modal";
    grid_planes.modal = ncplane_create(parent, &opts);
    return grid_planes.modal;
  }
  unsigned cur_rows = 0;
  unsigned cur_cols = 0;
  ncplane_dim_yx(grid_planes.modal, &cur_rows, &cur_cols);
  if ((int)cur_rows != rows || (int)cur_cols != cols) {
    ncplane_resize_simple(grid_planes.modal, (unsigned)rows, (unsigned)cols);
  }
  ncplane_move_yx(grid_planes.modal, top, left);
  return grid_planes.modal;
}
// Acquire (or move/resize) a cached child plane via a pointer-to-pointer
// slot in `grid_planes`. On first call we allocate and set the base cell
// to fully transparent; on subsequent calls we just reposition/resize.
struct ncplane *acquire_grid_plane(struct ncplane **slot,
                                   struct ncplane *parent, const char *name,
                                   int y, int x, int rows, int cols) {
  if (rows <= 0 || cols <= 0) {
    return NULL;
  }
  if (*slot == NULL) {
    ncplane_options opts = {0};
    opts.y = y;
    opts.x = x;
    opts.rows = (unsigned)rows;
    opts.cols = (unsigned)cols;
    opts.name = name;
    *slot = ncplane_create(parent, &opts);
    if (*slot == NULL) {
      return NULL;
    }
    uint64_t base_ch = 0;
    ncchannels_set_fg_alpha(&base_ch, NCALPHA_TRANSPARENT);
    ncchannels_set_bg_alpha(&base_ch, NCALPHA_TRANSPARENT);
    ncplane_set_base(*slot, " ", 0, base_ch);
    return *slot;
  }
  unsigned cur_rows = 0;
  unsigned cur_cols = 0;
  ncplane_dim_yx(*slot, &cur_rows, &cur_cols);
  if ((int)cur_rows != rows || (int)cur_cols != cols) {
    ncplane_resize_simple(*slot, (unsigned)rows, (unsigned)cols);
  }
  ncplane_move_yx(*slot, y, x);
  return *slot;
}
