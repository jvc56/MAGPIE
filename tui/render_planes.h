#ifndef TUI_RENDER_PLANES_H
#define TUI_RENDER_PLANES_H

#include <notcurses/notcurses.h>
#include <stdbool.h>
#include <stdint.h>

// All grid planes live in one module-level registry so a single
// invalidation call (on resize or after the onboarding picker closes)
// can destroy them and the next render rebuilds fresh. Without this,
// font-size changes leave the previous pixel image at its old cell
// offset and the terminal can show ghost lines smearing into nearby
// rows — most visibly cutting the player-pill box borders.
typedef struct {
  // 2x board pixel composite.
  struct ncplane *board;
  // 2x board row + column coordinate labels. Drawn as pixels so a
  // 1-cell-tall glyph can be centered against the 2-cell-tall board
  // rows (which is impossible with text mode).
  struct ncplane *labels_col;
  struct ncplane *labels_row;
  // 2x rack pixel composite. Tiles in the rack scale alongside the
  // board so they don't read as tiny next to a giant board.
  struct ncplane *rack;
  // Modal box renders to its own child plane so it sits above the 2x
  // pixel composite. A dedicated top-most modal plane keeps both the
  // board and the menu visible at once.
  struct ncplane *modal;
} TuiGridPlanes;

// Destroys `plane`, first dropping any pixels the frame dump recorded
// for it. Use for every plane the TUI destroys.
void tui_plane_destroy(struct ncplane *plane);

// Cache for the 2x board pixel composite. ncblit_rgba is the FPS
// bottleneck even when the buffer hasn't changed; tracking a signature
// lets us skip the work and rely on notcurses keeping the plane's
// previous pixel content.
typedef struct {
  uint64_t version;     // game_state.render_version at last blit
  unsigned cdy, cdx;    // notcurses cell-pixel dims at last blit
  int param_a, param_b; // scale, antialias
  // History-cursor index at last blit. -1 = live board (the
  // common case); 0..N-1 = previewing entry N's pre-move board.
  // Changing this invalidates the cache so the historical board
  // gets re-rasterized on the first frame after navigation.
  int history_cursor;
  bool valid;
} BlitCache;

// Accessor for the single plane registry.
TuiGridPlanes *tui_grid_planes(void);

// Destroy every registered plane (callers reset their own caches after).
void tui_planes_destroy_all(void);

struct ncplane *acquire_grid_plane(struct ncplane **slot,
                                   struct ncplane *parent, const char *name,
                                   int y, int x, int rows, int cols);
// Shared modal-plane accessor (used by render_modal and the lexicon
// picker). Creates the plane on first use; resizes/repositions on
// subsequent calls. The plane is destroyed when tui_game_render sees
// modal == TUI_MODAL_NONE, so callers don't manage its lifetime.
struct ncplane *
tui_game_render_get_or_create_modal_plane(struct ncplane *parent, int top,
                                          int left, int rows, int cols);

#endif
