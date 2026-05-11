#ifndef TUI_GLYPH_CACHE_H
#define TUI_GLYPH_CACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// FreeType-backed cache of alpha-only glyph bitmaps. Used by the 2x board
// render path: one glyph alpha buffer per ASCII letter, sized to the
// terminal's current cell-pixel height. Color is applied at composite
// time, not stored here — that lets the same cached bitmap render in
// every tile_fg/premium_fg color without re-rasterizing.
//
// The cache is keyed implicitly by the current (pixel_size, antialias)
// pair: setting a new size flushes every stored glyph. This matches the
// fact that a Ghostty font-size change makes every existing bitmap stale
// at once, so per-glyph age tracking would be wasted work.

typedef struct {
  int width;            // bitmap width in pixels (may be 0 for whitespace)
  int height;           // bitmap height in pixels
  int bearing_x;        // pixels from pen X to glyph's left edge
  int bearing_y;        // pixels from baseline up to glyph's top edge
  int advance;          // horizontal advance after drawing this glyph
  unsigned char *alpha; // width * height bytes, row-major, 0..255
} TuiGlyph;

typedef struct TuiGlyphCache TuiGlyphCache;

// Resolves a usable TTF path. Probes in order:
//   1. $MAGPIE_TUI_FONT          (env override)
//   2. tui/assets/magpie.ttf     (and ../, ./ variants — same logic as
//                                  find_data_paths in game_state.c)
//   3. ~/Library/Fonts/FiraCodeNerdFontMono-Regular.ttf  (macOS dev)
//   4. /usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf  (Debian)
// Returns true and fills `out` (caller buffer) on success. Returns false
// if no candidate was readable; the 2x board render path then stays off.
bool tui_glyph_cache_resolve_font_path(char *out, size_t out_size);

// Create a cache backed by `font_path`. Returns NULL if FreeType init
// fails or the font cannot be loaded — callers must treat this as
// "2x mode unavailable" rather than fatal.
TuiGlyphCache *tui_glyph_cache_create(const char *font_path);

void tui_glyph_cache_destroy(TuiGlyphCache *cache);

// Reconfigure the rasterizer. If either parameter changed since the last
// call, every previously rasterized glyph is freed; the next get() calls
// rebuild lazily. Calling with the existing parameters is a cheap no-op.
void tui_glyph_cache_set_size(TuiGlyphCache *cache, int pixel_size,
                              bool antialias);

// Drop every cached glyph without changing size/AA settings. Hook this
// into the resize path so a stale cache from before a font-size change
// is rebuilt at the new cell-to-pixel ratio.
void tui_glyph_cache_reset(TuiGlyphCache *cache);

// Returns a borrowed pointer to the cached glyph for `codepoint`, or
// NULL on rasterization failure (or if the cache itself is NULL). The
// pointer is invalidated by any subsequent set_size/reset call, so do
// not hold it across frames.
const TuiGlyph *tui_glyph_cache_get(TuiGlyphCache *cache, uint32_t codepoint);

#endif
