#ifndef TUI_FRAME_DUMP_H
#define TUI_FRAME_DUMP_H

#include "theme.h"
#include <notcurses/notcurses.h>
#include <stdbool.h>
#include <stdint.h>

// Off-terminal PNG screenshots of the running TUI.
//
// magpie_tui draws its board and rack tiles as FreeType-rasterized RGBA
// bitmaps blitted through the graphics protocol (Kitty / Sixel). That
// content is write-only from the app's side — the terminal never hands
// the composited pixels back, and PTY-side terminal-grid tools (vt100
// emulators like termwright) can't decode the graphics escapes at all.
// So the only way to observe what the renderer produced, for automated
// testing or remote inspection, is to capture the source RGBA the app
// already holds in its own memory and composite it ourselves.
//
// The flow is:
//   1. Every ncblit_rgba call site hands its source buffer to
//      tui_frame_dump_capture(), which keeps a per-plane copy keyed by
//      the notcurses plane pointer. Blits are cache-gated (they only
//      happen when a tile's content actually changes), so the registry
//      persists the last-blitted pixels for every live plane.
//   2. A SIGUSR1 handler (async-signal-safe) calls tui_frame_dump_request()
//      to set a one-shot flag.
//   3. After notcurses_render(), the main loop polls tui_frame_dump_pending()
//      and, when set, calls tui_frame_dump_write() — which walks the live
//      plane stack bottom-to-top (true z-order), alpha-composites each
//      plane's captured RGBA into a full-screen buffer at its pixel
//      position, and writes a PNG. No terminal involvement.
//
// Two layers are composited, in plane z-order:
//   - pixel planes (board + rack tiles): from the captured-RGBA registry.
//   - cell-text planes (panels, labels, status, borders): rasterized live
//     from each plane's nccells via a dedicated FreeType glyph cache, so
//     the PNG is a faithful full-UI screenshot.

// Record the RGBA source buffer most recently blitted to `plane`. `rgba`
// is `width * height * 4` bytes, row-major, 8-bit R,G,B,A (the same layout
// ncblit_rgba consumes). The buffer is copied; the caller may free its own
// copy immediately after. No-op if `plane`/`rgba` is NULL or dims <= 0.
void tui_frame_dump_capture(struct ncplane *plane, const uint8_t *rgba,
                            int width, int height);

// Async-signal-safe: arm a one-shot dump. Safe to call from a signal
// handler (it only stores to an atomic flag).
void tui_frame_dump_request(void);

// True if a dump has been requested and not yet serviced.
bool tui_frame_dump_pending(void);

// Composite the captured planes into a PNG and write it. `nc` and `std`
// are the live notcurses context and standard plane (for the plane-stack
// walk and pixel geometry). `theme` supplies the backdrop fill and the
// resolved colors for cells drawn with notcurses' default fg/bg.
// `out_path` is the destination file; if NULL, the value of the
// MAGPIE_TUI_DUMP_PATH environment variable is used, falling back to
// /tmp/magpie_tui_frame.png. Clears the pending flag. Returns true on
// success. Safe to call only from the render/UI thread.
bool tui_frame_dump_write(struct notcurses *nc, struct ncplane *std,
                          const Theme *theme, const char *out_path);

#endif
